/* ============================================================
 * SLYZ 语言支持：语法高亮 + 由 slyzc 提供的实时诊断
 * ------------------------------------------------------------
 * 诊断规则只保留一份——编译器本身。扩展启动一个常驻的
 * `slyzc serve` 进程，按 JSON Lines 协议把文档全文发过去，
 * 再把返回的诊断映射成 VS Code Diagnostic。
 *
 * 为什么不在这里重新实现语法检查：手写规则一定会和编译器漂移，
 * 之前那一版还停留在旧文本脚本（if yes/endif），对 v2 的
 * let/fn/while/for 完全无能为力。
 * ============================================================ */

import * as vscode from 'vscode';
import { spawn, ChildProcessWithoutNullStreams } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';

const diagnosticCollection = vscode.languages.createDiagnosticCollection('slyz');

interface RawDiagnostic {
  line: number;
  col: number;
  severity: string;
  message: string;
}

interface CheckResponse {
  id?: number;
  ok?: boolean;
  errorCount?: number;
  diagnostics?: RawDiagnostic[];
  fatal?: string;
  restart?: boolean;
}

interface PendingRequest {
  resolve: (value: CheckResponse) => void;
  reject: (reason: Error) => void;
  timer: NodeJS.Timeout;
}

const REQUEST_TIMEOUT_MS = 15000;
const MAX_RESTARTS = 10;

class SlyzChecker implements vscode.Disposable {
  private proc: ChildProcessWithoutNullStreams | undefined;
  private readonly pending = new Map<number, PendingRequest>();
  private nextId = 1;
  private buffer = '';
  private disposed = false;
  private restartTimer: NodeJS.Timeout | undefined;
  private restartCount = 0;
  private readonly output = vscode.window.createOutputChannel('SLYZ');

  constructor(private readonly extensionPath: string) {}

  dispose(): void {
    this.disposed = true;
    if (this.restartTimer) {
      clearTimeout(this.restartTimer);
      this.restartTimer = undefined;
    }
    this.rejectAll(new Error('SLYZ checker disposed'));
    const proc = this.proc;
    this.proc = undefined;
    if (proc && !proc.killed) {
      try {
        proc.stdin.write('{"method":"exit"}\n');
      } catch {
        /* stdin 可能已经关闭，直接 kill */
      }
      proc.kill();
    }
    this.output.dispose();
  }

  /** 解析可用的 slyzc.exe：配置项 > 工作区构建产物 > 扩展自带 */
  private resolveCompiler(): string | undefined {
    const config = vscode.workspace.getConfiguration('slyz');
    const configured = (config.get<string>('compilerPath') ?? '').trim();
    const candidates: string[] = [];

    if (configured) {
      if (path.isAbsolute(configured)) {
        candidates.push(configured);
      } else {
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
          candidates.push(path.join(folder.uri.fsPath, configured));
        }
      }
    }

    for (const folder of vscode.workspace.workspaceFolders ?? []) {
      candidates.push(path.join(folder.uri.fsPath, 'build', 'slyzc.exe'));
      candidates.push(path.join(folder.uri.fsPath, 'tools', 'slyzc', 'build', 'slyzc.exe'));
    }
    candidates.push(path.join(this.extensionPath, 'bin', 'slyzc.exe'));

    for (const candidate of candidates) {
      if (candidate && fs.existsSync(candidate)) {
        return candidate;
      }
    }
    return undefined;
  }

  start(): void {
    if (this.disposed || this.proc) {
      return;
    }

    const compiler = this.resolveCompiler();
    if (!compiler) {
      this.reportMissingCompiler();
      return;
    }

    let proc: ChildProcessWithoutNullStreams;
    try {
      proc = spawn(compiler, ['serve'], {
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
      });
    } catch (error) {
      this.output.appendLine(`[slyzc] 启动失败: ${String(error)}`);
      this.scheduleRestart();
      return;
    }

    this.proc = proc;
    proc.stdout.setEncoding('utf8');
    proc.stderr.setEncoding('utf8');
    proc.stdout.on('data', (chunk: string) => this.onStdout(chunk));
    proc.stderr.on('data', (chunk: string) => {
      this.output.append(chunk);
    });
    proc.on('error', (error: Error) => {
      this.output.appendLine(`[slyzc] 进程错误: ${error.message}`);
    });
    proc.on('exit', (code: number | null) => this.onExit(code));

    this.output.appendLine(`[slyzc] 已启动: ${compiler}`);
  }

  private reportMissingCompiler(): void {
    this.output.appendLine(
      '[slyzc] 找不到 slyzc.exe。请先运行 tools\\slyzc\\build.ps1，' +
      '或在设置里配置 slyz.compilerPath 指向可执行文件。'
    );
    this.output.show(true);
    vscode.window.setStatusBarMessage('SLYZ: 未找到 slyzc.exe（语法检查不可用）', 6000);
  }

  private onStdout(chunk: string): void {
    this.buffer += chunk;
    for (;;) {
      const index = this.buffer.indexOf('\n');
      if (index < 0) {
        break;
      }
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      if (!line) {
        continue;
      }

      let message: CheckResponse;
      try {
        message = JSON.parse(line) as CheckResponse;
      } catch {
        this.output.appendLine(`[slyzc] 无法解析的输出: ${line}`);
        continue;
      }

      if (message.restart) {
        this.output.appendLine('[slyzc] 达到内存水位，重启检查进程');
        continue;
      }

      if (typeof message.id !== 'number') {
        continue;
      }
      const entry = this.pending.get(message.id);
      if (!entry) {
        continue;
      }
      this.pending.delete(message.id);
      clearTimeout(entry.timer);
      entry.resolve(message);
    }
  }

  private onExit(code: number | null): void {
    this.proc = undefined;
    this.rejectAll(new Error(`SLYZ checker exited (${code ?? 'null'})`));
    if (this.disposed) {
      return;
    }
    this.scheduleRestart();
  }

  private scheduleRestart(): void {
    if (this.disposed || this.restartTimer) {
      return;
    }
    if (this.restartCount >= MAX_RESTARTS) {
      this.output.appendLine('[slyzc] 连续重启次数过多，已停止自动重启');
      return;
    }
    const delay = Math.min(30000, 1000 * Math.pow(2, this.restartCount));
    this.restartCount++;
    this.restartTimer = setTimeout(() => {
      this.restartTimer = undefined;
      this.start();
    }, delay);
  }

  private rejectAll(reason: Error): void {
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(reason);
    }
    this.pending.clear();
  }

  private request(payload: Record<string, unknown>): Promise<CheckResponse> {
    this.start();
    const proc = this.proc;
    if (!proc) {
      return Promise.reject(new Error('slyzc.exe 未启动'));
    }

    const id = this.nextId++;
    const body = JSON.stringify({ id, ...payload });

    return new Promise<CheckResponse>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('检查超时'));
      }, REQUEST_TIMEOUT_MS);

      this.pending.set(id, { resolve, reject, timer });
      try {
        proc.stdin.write(body + '\n');
      } catch (error) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  /** 请求一次检查；失败时返回空数组并记录原因，不让编辑器弹窗。 */
  async check(pathLabel: string, text: string): Promise<vscode.Diagnostic[]> {
    let response: CheckResponse;
    try {
      response = await this.request({ path: pathLabel, text });
      this.restartCount = 0;
    } catch (error) {
      this.output.appendLine(`[slyzc] 检查失败: ${String(error)}`);
      return [];
    }

    if (response.fatal) {
      this.output.appendLine(`[slyzc] ${response.fatal}`);
      return [];
    }

    const result: vscode.Diagnostic[] = [];
    for (const item of response.diagnostics ?? []) {
      const line = Math.max(0, (item.line | 0) - 1);
      const column = Math.max(0, (item.col | 0) - 1);
      const range = new vscode.Range(
        new vscode.Position(line, column),
        new vscode.Position(line, column + 1)
      );
      const severity = item.severity === 'warning'
        ? vscode.DiagnosticSeverity.Warning
        : vscode.DiagnosticSeverity.Error;
      const diagnostic = new vscode.Diagnostic(range, item.message, severity);
      diagnostic.source = 'slyzc';
      result.push(diagnostic);
    }
    return result;
  }
}

let checker: SlyzChecker | undefined;
let debounceTimer: NodeJS.Timeout | undefined;

function debounceMs(): number {
  const value = vscode.workspace.getConfiguration('slyz').get<number>('checkDebounceMs', 250);
  if (typeof value !== 'number' || Number.isNaN(value)) {
    return 250;
  }
  return Math.max(0, Math.min(5000, value));
}

async function refresh(document: vscode.TextDocument): Promise<void> {
  if (!checker || document.languageId !== 'slyz') {
    return;
  }
  // 异步返回期间文档可能又被编辑：版本不一致就丢弃这次结果，
  // 否则旧诊断会覆盖新诊断。
  const version = document.version;
  const diagnostics = await checker.check(document.uri.fsPath || document.fileName,
                                         document.getText());
  if (document.version !== version) {
    return;
  }
  diagnosticCollection.set(document.uri, diagnostics);
}

function scheduleRefresh(document: vscode.TextDocument): void {
  if (document.languageId !== 'slyz') {
    return;
  }
  if (debounceTimer) {
    clearTimeout(debounceTimer);
  }
  debounceTimer = setTimeout(() => {
    debounceTimer = undefined;
    void refresh(document);
  }, debounceMs());
}

export function activate(context: vscode.ExtensionContext): void {
  context.subscriptions.push(diagnosticCollection);

  checker = new SlyzChecker(context.extensionPath);
  context.subscriptions.push(checker);
  checker.start();

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((document) => {
      void refresh(document);
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((event) => {
      scheduleRefresh(event.document);
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument((document) => {
      void refresh(document);
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((document) => {
      diagnosticCollection.delete(document.uri);
    })
  );

  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      if (editor) {
        void refresh(editor.document);
      }
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration('slyz.compilerPath')) {
        checker?.dispose();
        checker = new SlyzChecker(context.extensionPath);
        context.subscriptions.push(checker);
        checker.start();
      }
    })
  );

  const active = vscode.window.activeTextEditor;
  if (active) {
    void refresh(active.document);
  }
}

export function deactivate(): void {
  if (debounceTimer) {
    clearTimeout(debounceTimer);
    debounceTimer = undefined;
  }
  checker?.dispose();
  checker = undefined;
  diagnosticCollection.clear();
}
