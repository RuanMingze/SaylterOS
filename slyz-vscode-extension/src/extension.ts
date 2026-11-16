import * as vscode from 'vscode';

const diagnosticCollection = vscode.languages.createDiagnosticCollection('slyz');

/* 与内核 gui.c 的解析器保持一致的上限：
 *   GUI_SLZ_MAX   = 32  控件数量
 *   GUI_SLZ_SRC   = 16384  源文件字节数
 *   单行          = 199  字符（第 200 个字符起被截断） */
const GUI_MAX_CONTROLS = 32;
const GUI_MAX_SRC_BYTES = 16384;
const GUI_MAX_LINE_CHARS = 199;

const GUI_KEYWORDS = ['title', 'size', 'label', 'input', 'button', 'space', 'hr'];
const GUI_ACTION_PREFIXES = ['set:', 'open:', 'shell:'];

function makeDiagnostic(message: string, line: number, column: number,
                        length: number,
                        severity: vscode.DiagnosticSeverity): vscode.Diagnostic {
  const end = Math.max(column + Math.max(1, length), column + 1);
  const range = new vscode.Range(new vscode.Position(line, column),
                                 new vscode.Position(line, end));
  return new vscode.Diagnostic(range, message, severity);
}

function errorAt(message: string, line: number, column = 0, length = 1): vscode.Diagnostic {
  return makeDiagnostic(message, line, column, length, vscode.DiagnosticSeverity.Error);
}

function warnAt(message: string, line: number, column = 0, length = 1): vscode.Diagnostic {
  return makeDiagnostic(message, line, column, length, vscode.DiagnosticSeverity.Warning);
}

function infoAt(message: string, line: number, column = 0, length = 1): vscode.Diagnostic {
  return makeDiagnostic(message, line, column, length, vscode.DiagnosticSeverity.Information);
}

/* 取一行去掉注释后的有效内容。SLYZ 的注释规则是行首 #，
 * 但 #gui 是程序标记，不能当注释吃掉。 */
function stripGuiComment(line: string): string {
  const trimmed = line.trim();
  if (trimmed.startsWith('#') && !/^#gui\b/i.test(trimmed)) {
    return '';
  }
  return trimmed;
}

function isGuiProgram(lines: string[]): boolean {
  for (const line of lines) {
    if (!line.trim()) continue;
    return /^\s*#gui\b/i.test(line);
  }
  return false;
}

/* 收集所有 input 的名称，供按钮动作里的 {name} 占位符校验 */
function collectInputNames(lines: string[]): Set<string> {
  const names = new Set<string>();
  for (let i = 1; i < lines.length; i++) {
    const line = stripGuiComment(lines[i]);
    if (!line) continue;
    const m = /^input\b(.*)$/i.exec(line);
    if (!m) continue;
    const parts = m[1].trim().split(/\s+/).filter(Boolean);
    if (parts.length === 0) continue;
    let name = parts[0];
    if (name.startsWith('"')) {
      const close = name.indexOf('"', 1);
      name = close > 1 ? name.slice(1, close) : name.slice(1);
    }
    if (name) names.add(name);
  }
  return names;
}

function analyzeGuiProgram(lines: string[], diagnostics: vscode.Diagnostic[]): void {
  const first = lines.length > 0 ? lines[0] : '';
  if (!/^\s*#gui\b/i.test(first)) {
    diagnostics.push(errorAt('GUI 程序第一行必须是 #gui（大小写不敏感，允许前导空白）', 0, 0, first.length));
    return;
  }

  const inputs = collectInputNames(lines);
  let controls = 0;

  for (let i = 1; i < lines.length; i++) {
    const raw = lines[i];

    if (raw.length > GUI_MAX_LINE_CHARS) {
      diagnostics.push(warnAt(
        `单行超过 ${GUI_MAX_LINE_CHARS} 字符，内核会截断第 ${GUI_MAX_LINE_CHARS + 1} 个字符之后的内容`,
        i, GUI_MAX_LINE_CHARS, raw.length - GUI_MAX_LINE_CHARS));
    }

    const line = stripGuiComment(raw);
    if (!line) continue;

    const m = /^([A-Za-z]+)\b(.*)$/.exec(line);
    if (!m) {
      diagnostics.push(warnAt('无法识别的行：GUI 指令必须以字母关键字开头', i, 0, line.length));
      continue;
    }

    const keyword = m[1].toLowerCase();
    const rest = m[2].trim();

    switch (keyword) {
      case 'title': {
        if (!rest) {
          diagnostics.push(errorAt('title 需要窗口标题，例如：title "My App"', i, 0, 5));
        }
        break;
      }

      case 'size': {
        const nums = rest.split(/\s+/).filter(Boolean);
        if (nums.length === 0) {
          diagnostics.push(errorAt('size 需要宽度和高度，例如：size 560 460', i, 0, 4));
          break;
        }
        const limits = [360, 260];
        const names = ['宽度', '高度'];
        for (let k = 0; k < nums.length && k < 2; k++) {
          if (!/^\d+$/.test(nums[k])) {
            diagnostics.push(errorAt(`size 的${names[k]}必须是十进制数字`, i, 0, line.length));
            continue;
          }
          const value = parseInt(nums[k], 10);
          if (value < limits[k]) {
            diagnostics.push(warnAt(
              `size 的${names[k]} ${value} 小于内核下限 ${limits[k]}，会被提升到 ${limits[k]}`,
              i, 0, line.length));
          }
        }
        if (nums.length > 2) {
          diagnostics.push(warnAt('size 只接受宽度和高度两个参数', i, 0, line.length));
        }
        break;
      }

      case 'label': {
        if (!rest) {
          diagnostics.push(errorAt('label 需要显示文本，例如：label "Hello"', i, 0, 5));
        }
        controls++;
        break;
      }

      case 'input': {
        const parts = rest.split(/\s+/).filter(Boolean);
        if (parts.length === 0) {
          diagnostics.push(errorAt('input 需要名称和标签，例如：input name "Your name"', i, 0, 5));
        } else {
          let name = parts[0];
          if (name.startsWith('"')) {
            const close = name.indexOf('"', 1);
            name = close > 1 ? name.slice(1, close) : name.slice(1);
          }
          if (!name) {
            diagnostics.push(errorAt('input 的名称不能为空', i, 0, line.length));
          } else if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
            diagnostics.push(warnAt(
              `输入框名称 "${name}" 含有非字母数字字符；{${name}} 占位符可能无法匹配`,
              i, 0, line.length));
          }
        }
        controls++;
        break;
      }

      case 'button': {
        const arrow = rest.indexOf('->');
        if (arrow < 0) {
          diagnostics.push(errorAt('button 必须写成：button "文本" -> 动作', i, 0, line.length));
          break;
        }

        const labelText = rest.slice(0, arrow).trim();
        const action = rest.slice(arrow + 2).trim();

        if (!labelText) {
          diagnostics.push(warnAt('button 缺少显示文本', i, 0, 6));
        }
        if (!action) {
          diagnostics.push(errorAt('button 缺少动作，支持 close / set: / open: / shell:', i, arrow, 2));
          break;
        }

        const isClose = /^close$/i.test(action);
        const prefix = GUI_ACTION_PREFIXES.find(p => action.toLowerCase().startsWith(p));
        if (!isClose && !prefix) {
          diagnostics.push(warnAt(
            `未知动作 "${action}"；支持 close / set:<文本> / open:<路径> / shell:<命令>`,
            i, 0, line.length));
        } else if (prefix) {
          const payload = action.slice(prefix.length).trim();
          if (!payload) {
            diagnostics.push(errorAt(`${prefix} 动作缺少参数`, i, arrow, 2));
          }
        }

        const placeholder = /\{([^}]*)\}/g;
        let match: RegExpExecArray | null;
        while ((match = placeholder.exec(action)) !== null) {
          const key = match[1].trim();
          if (key && !inputs.has(key)) {
            diagnostics.push(warnAt(
              `占位符 {${key}} 没有对应的 input 名称`,
              i, 0, line.length));
          }
        }

        controls++;
        break;
      }

      case 'space': {
        if (rest && !/^\d+$/.test(rest)) {
          diagnostics.push(warnAt('space 需要一个像素数字，例如：space 8', i, 0, line.length));
        }
        controls++;
        break;
      }

      case 'hr': {
        if (rest) {
          diagnostics.push(warnAt('hr 不接受参数', i, 0, line.length));
        }
        controls++;
        break;
      }

      default: {
        diagnostics.push(warnAt(
          `未知的 GUI 指令 "${m[1]}"；支持 ${GUI_KEYWORDS.join(' / ')}`,
          i, 0, m[1].length));
        break;
      }
    }
  }

  if (controls > GUI_MAX_CONTROLS) {
    diagnostics.push(errorAt(
      `控件数量 ${controls} 超过内核上限 ${GUI_MAX_CONTROLS}，超出的控件会被静默忽略`,
      0, 0, Math.max(1, first.length)));
  }
}

/* 旧文本脚本：if / endif / ask 规则保持不变 */
function analyzeTextScript(lines: string[], diagnostics: vscode.Diagnostic[]): void {
  const ifStack: Array<{ line: number; indent: number }> = [];

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const trimmed = line.trimStart();
    if (!trimmed || trimmed.startsWith('#')) {
      continue;
    }

    const indent = line.length - trimmed.length;

    if (ifStack.length > 0 && /^ +/.test(line) && !line.startsWith('\t')) {
      diagnostics.push(infoAt(
        '嵌套块使用空格缩进；建议统一使用 Tab。',
        i, 0, Math.max(1, indent)));
    }

    const invalidIf = /^\s*if\b(?!\s+(yes|no)\b)/i.exec(line);
    if (invalidIf) {
      diagnostics.push(errorAt('if 条件无效，只能写：if yes 或 if no', i, invalidIf.index));
    }

    const ifMatch = /^\s*if\s+(yes|no)\b/i.exec(line);
    if (ifMatch) {
      ifStack.push({ line: i, indent });
      continue;
    }

    const endifMatch = /^\s*endif\b/i.exec(line);
    if (endifMatch) {
      if (ifStack.length === 0) {
        diagnostics.push(errorAt('多余的 endif：没有匹配的 if 块', i, endifMatch.index));
      } else {
        ifStack.pop();
      }
      continue;
    }

    const askMatch = /^\s*ask\b/i.exec(line);
    if (askMatch) {
      const remaining = trimmed.slice(3).trim();
      if (!remaining || remaining === '""' || remaining === "''") {
        diagnostics.push(errorAt('ask 需要提示文本', i, askMatch.index));
      }
    }
  }

  for (const entry of ifStack) {
    diagnostics.push(errorAt('if 块未闭合：缺少 endif', entry.line, 0));
  }

  /* GUI 指令出现在没有 #gui 的文件里，内核不会把它当界面程序 */
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const m = /^([A-Za-z]+)\b/.exec(trimmed);
    if (m && GUI_KEYWORDS.includes(m[1].toLowerCase())) {
      diagnostics.push(infoAt(
        `这看起来是 GUI 指令，但文件第一行不是 #gui，内核会把它当普通脚本执行`,
        i, 0, m[1].length));
      break;
    }
  }
}

function analyzeDocument(document: vscode.TextDocument): vscode.Diagnostic[] {
  const diagnostics: vscode.Diagnostic[] = [];
  const text = document.getText();
  const lines = text.split(/\r?\n/);

  if (isGuiProgram(lines)) {
    if (text.length > GUI_MAX_SRC_BYTES) {
      diagnostics.push(warnAt(
        `文件 ${text.length} 字节，超过内核读取上限 ${GUI_MAX_SRC_BYTES} 字节，尾部会被截断`,
        0, 0, (lines[0] ?? '').length));
    }
    analyzeGuiProgram(lines, diagnostics);
  } else {
    analyzeTextScript(lines, diagnostics);
  }

  return diagnostics;
}

function refreshDiagnostics(document?: vscode.TextDocument): void {
  if (!document || document.languageId !== 'slyz') {
    return;
  }

  diagnosticCollection.set(document.uri, analyzeDocument(document));
}

export function activate(context: vscode.ExtensionContext): void {
  context.subscriptions.push(diagnosticCollection);

  const trigger = () => {
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === 'slyz') {
      refreshDiagnostics(editor.document);
    }
  };

  trigger();

  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      if (editor && editor.document.languageId === 'slyz') {
        refreshDiagnostics(editor.document);
      }
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(refreshDiagnostics)
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((event) => {
      refreshDiagnostics(event.document);
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((document) => {
      diagnosticCollection.delete(document.uri);
    })
  );
}

export function deactivate(): void {
  diagnosticCollection.clear();
}
