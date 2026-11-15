import * as vscode from 'vscode';

const diagnosticCollection = vscode.languages.createDiagnosticCollection('slyz');

function makeDiagnostic(message: string, line: number, column: number, severity: vscode.DiagnosticSeverity = vscode.DiagnosticSeverity.Warning): vscode.Diagnostic {
  const range = new vscode.Range(new vscode.Position(line, column), new vscode.Position(line, Math.max(column + 1, column + 1)));
  return new vscode.Diagnostic(range, message, severity);
}

function analyzeDocument(document: vscode.TextDocument): vscode.Diagnostic[] {
  const diagnostics: vscode.Diagnostic[] = [];
  const lines = document.getText().split(/\r?\n/);
  const ifStack: Array<{ line: number; indent: number }> = [];

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const trimmed = line.trimStart();
    if (!trimmed || trimmed.startsWith('#')) {
      continue;
    }

    const indent = line.length - trimmed.length;

    if (ifStack.length > 0 && /^ +/.test(line) && !line.startsWith('\t')) {
      diagnostics.push(makeDiagnostic("Nested block uses spaces; consider using tabs for indentation.", i, 0, vscode.DiagnosticSeverity.Information));
    }

    const invalidIf = /^\s*if\b(?!\s+(yes|no)\b)/i.exec(line);
    if (invalidIf) {
      diagnostics.push(makeDiagnostic("Invalid if condition. Use: if yes or if no.", i, invalidIf.index, vscode.DiagnosticSeverity.Error));
    }

    const ifMatch = /^\s*if\s+(yes|no)\b/i.exec(line);
    if (ifMatch) {
      ifStack.push({ line: i, indent });
      continue;
    }

    const endifMatch = /^\s*endif\b/i.exec(line);
    if (endifMatch) {
      if (ifStack.length === 0) {
        diagnostics.push(makeDiagnostic("Unexpected endif: no matching if block.", i, endifMatch.index, vscode.DiagnosticSeverity.Error));
      } else {
        ifStack.pop();
      }
      continue;
    }

    const askMatch = /^\s*ask\b/i.exec(line);
    if (askMatch) {
      const remaining = trimmed.slice(3).trim();
      if (!remaining || remaining === '""' || remaining === "''") {
        diagnostics.push(makeDiagnostic("ask requires a prompt string.", i, askMatch.index, vscode.DiagnosticSeverity.Error));
      }
    }
  }

  for (const entry of ifStack) {
    diagnostics.push(makeDiagnostic("Unclosed if block. Missing endif.", entry.line, 0, vscode.DiagnosticSeverity.Error));
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
}

export function deactivate(): void {
  diagnosticCollection.clear();
}
