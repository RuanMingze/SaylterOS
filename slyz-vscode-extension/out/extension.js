"use strict";
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
const diagnosticCollection = vscode.languages.createDiagnosticCollection('slyz');
function makeDiagnostic(message, line, column, severity = vscode.DiagnosticSeverity.Warning) {
    const range = new vscode.Range(new vscode.Position(line, column), new vscode.Position(line, Math.max(column + 1, column + 1)));
    return new vscode.Diagnostic(range, message, severity);
}
function analyzeDocument(document) {
    const diagnostics = [];
    const lines = document.getText().split(/\r?\n/);
    const ifStack = [];
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
            }
            else {
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
function refreshDiagnostics(document) {
    if (!document || document.languageId !== 'slyz') {
        return;
    }
    diagnosticCollection.set(document.uri, analyzeDocument(document));
}
function activate(context) {
    context.subscriptions.push(diagnosticCollection);
    const trigger = () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'slyz') {
            refreshDiagnostics(editor.document);
        }
    };
    trigger();
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        if (editor && editor.document.languageId === 'slyz') {
            refreshDiagnostics(editor.document);
        }
    }));
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(refreshDiagnostics));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => {
        refreshDiagnostics(event.document);
    }));
}
function deactivate() {
    diagnosticCollection.clear();
}
