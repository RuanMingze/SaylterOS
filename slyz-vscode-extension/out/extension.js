"use strict";
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
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const diagnosticCollection = vscode.languages.createDiagnosticCollection('slyz');
const REQUEST_TIMEOUT_MS = 15000;
const MAX_RESTARTS = 10;
class SlyzChecker {
    constructor(extensionPath) {
        this.extensionPath = extensionPath;
        this.pending = new Map();
        this.nextId = 1;
        this.buffer = '';
        this.disposed = false;
        this.restartCount = 0;
        this.output = vscode.window.createOutputChannel('SLYZ');
    }
    dispose() {
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
            }
            catch {
                /* stdin 可能已经关闭，直接 kill */
            }
            proc.kill();
        }
        this.output.dispose();
    }
    /** 解析可用的 slyzc.exe：配置项 > 工作区构建产物 > 扩展自带 */
    resolveCompiler() {
        const config = vscode.workspace.getConfiguration('slyz');
        const configured = (config.get('compilerPath') ?? '').trim();
        const candidates = [];
        if (configured) {
            if (path.isAbsolute(configured)) {
                candidates.push(configured);
            }
            else {
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
    start() {
        if (this.disposed || this.proc) {
            return;
        }
        const compiler = this.resolveCompiler();
        if (!compiler) {
            this.reportMissingCompiler();
            return;
        }
        let proc;
        try {
            proc = (0, child_process_1.spawn)(compiler, ['serve'], {
                stdio: ['pipe', 'pipe', 'pipe'],
                windowsHide: true,
            });
        }
        catch (error) {
            this.output.appendLine(`[slyzc] 启动失败: ${String(error)}`);
            this.scheduleRestart();
            return;
        }
        this.proc = proc;
        proc.stdout.setEncoding('utf8');
        proc.stderr.setEncoding('utf8');
        proc.stdout.on('data', (chunk) => this.onStdout(chunk));
        proc.stderr.on('data', (chunk) => {
            this.output.append(chunk);
        });
        proc.on('error', (error) => {
            this.output.appendLine(`[slyzc] 进程错误: ${error.message}`);
        });
        proc.on('exit', (code) => this.onExit(code));
        this.output.appendLine(`[slyzc] 已启动: ${compiler}`);
    }
    reportMissingCompiler() {
        this.output.appendLine('[slyzc] 找不到 slyzc.exe。请先运行 tools\\slyzc\\build.ps1，' +
            '或在设置里配置 slyz.compilerPath 指向可执行文件。');
        this.output.show(true);
        vscode.window.setStatusBarMessage('SLYZ: 未找到 slyzc.exe（语法检查不可用）', 6000);
    }
    onStdout(chunk) {
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
            let message;
            try {
                message = JSON.parse(line);
            }
            catch {
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
    onExit(code) {
        this.proc = undefined;
        this.rejectAll(new Error(`SLYZ checker exited (${code ?? 'null'})`));
        if (this.disposed) {
            return;
        }
        this.scheduleRestart();
    }
    scheduleRestart() {
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
    rejectAll(reason) {
        for (const [, entry] of this.pending) {
            clearTimeout(entry.timer);
            entry.reject(reason);
        }
        this.pending.clear();
    }
    request(payload) {
        this.start();
        const proc = this.proc;
        if (!proc) {
            return Promise.reject(new Error('slyzc.exe 未启动'));
        }
        const id = this.nextId++;
        const body = JSON.stringify({ id, ...payload });
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error('检查超时'));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, { resolve, reject, timer });
            try {
                proc.stdin.write(body + '\n');
            }
            catch (error) {
                clearTimeout(timer);
                this.pending.delete(id);
                reject(error instanceof Error ? error : new Error(String(error)));
            }
        });
    }
    /** 请求一次检查；失败时返回空数组并记录原因，不让编辑器弹窗。 */
    async check(pathLabel, text) {
        let response;
        try {
            response = await this.request({ path: pathLabel, text });
            this.restartCount = 0;
        }
        catch (error) {
            this.output.appendLine(`[slyzc] 检查失败: ${String(error)}`);
            return [];
        }
        if (response.fatal) {
            this.output.appendLine(`[slyzc] ${response.fatal}`);
            return [];
        }
        const result = [];
        for (const item of response.diagnostics ?? []) {
            const line = Math.max(0, (item.line | 0) - 1);
            const column = Math.max(0, (item.col | 0) - 1);
            const range = new vscode.Range(new vscode.Position(line, column), new vscode.Position(line, column + 1));
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
let checker;
let debounceTimer;
function debounceMs() {
    const value = vscode.workspace.getConfiguration('slyz').get('checkDebounceMs', 250);
    if (typeof value !== 'number' || Number.isNaN(value)) {
        return 250;
    }
    return Math.max(0, Math.min(5000, value));
}
async function refresh(document) {
    if (!checker || document.languageId !== 'slyz') {
        return;
    }
    // 异步返回期间文档可能又被编辑：版本不一致就丢弃这次结果，
    // 否则旧诊断会覆盖新诊断。
    const version = document.version;
    const diagnostics = await checker.check(document.uri.fsPath || document.fileName, document.getText());
    if (document.version !== version) {
        return;
    }
    diagnosticCollection.set(document.uri, diagnostics);
}
function scheduleRefresh(document) {
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
function activate(context) {
    context.subscriptions.push(diagnosticCollection);
    checker = new SlyzChecker(context.extensionPath);
    context.subscriptions.push(checker);
    checker.start();
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument((document) => {
        void refresh(document);
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => {
        scheduleRefresh(event.document);
    }));
    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument((document) => {
        void refresh(document);
    }));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((document) => {
        diagnosticCollection.delete(document.uri);
    }));
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        if (editor) {
            void refresh(editor.document);
        }
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration('slyz.compilerPath')) {
            checker?.dispose();
            checker = new SlyzChecker(context.extensionPath);
            context.subscriptions.push(checker);
            checker.start();
        }
    }));
    const active = vscode.window.activeTextEditor;
    if (active) {
        void refresh(active.document);
    }
}
function deactivate() {
    if (debounceTimer) {
        clearTimeout(debounceTimer);
        debounceTimer = undefined;
    }
    checker?.dispose();
    checker = undefined;
    diagnosticCollection.clear();
}
