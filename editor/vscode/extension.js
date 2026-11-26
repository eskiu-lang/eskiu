'use strict';
/**
 * extension.js: VS Code extension entry point for the Eskiu language.
 *
 * Every feature shells out to the compiler through project.js, which finds the program a
 * file belongs to and runs eskiuc on the current text (unsaved buffers as --overlay), and
 * features.js, which reads the output. server.js (the language server for other editors)
 * uses the same two modules, so both editors behave alike. This file only converts to and
 * from the VS Code API; the Run command is its own.
 */

const vscode = require('vscode');
const path = require('path');
const fs   = require('fs');
const os   = require('os');
const project = require('./project');
const features = require('./features');

let diagnosticCollection;
const pendingChecks = new Map();

function config() {
    return vscode.workspace.getConfiguration('eskiu');
}

// The shared settings. In a workspace the user has not trusted, VS Code ignores the
// workspace's compilerPath, compilerFlags and entryPoint (restricted settings in
// package.json), and project.js does not look in its build directories.
function settings() {
    const c = config();
    return project.normalizeSettings({
        compilerPath: c.get('compilerPath'), compilerFlags: c.get('compilerFlags'), entryPoint: c.get('entryPoint'),
        checkOnType: c.get('checkOnType'), checkDelay: c.get('checkDelay'),
    });
}

let missingReported = false;
function reportMissingCompiler(name) {
    if (missingReported) return;
    missingReported = true;
    vscode.window.showWarningMessage(
        `Eskiu: could not run '${name || 'eskiuc'}'. Install eskiuc or set "eskiu.compilerPath".`,
        'Open Settings'
    ).then((choice) => {
        if (choice) vscode.commands.executeCommand('workbench.action.openSettings', 'eskiu.compilerPath');
    });
}

let outdatedReported = false;
function reportOutdatedCompiler(name) {
    if (outdatedReported) return;
    outdatedReported = true;
    vscode.window.showWarningMessage(`Eskiu: '${name}' has no --overlay (it is older than this extension); ` +
        'unsaved changes in imported files are not checked. Update eskiuc.');
}

// An untitled document has no file: it is checked as a file of its own in a temp directory.
const untitled = new Map();
function docPath(document) {
    if (!document.isUntitled) return path.resolve(document.uri.fsPath);
    const key = document.uri.toString();
    if (!untitled.has(key)) {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-untitled-'));
        untitled.set(key, path.join(dir, 'untitled.esk'));
    }
    const p = untitled.get(key);
    try { fs.writeFileSync(p, document.getText()); } catch {}
    return p;
}

// What project.js needs: the settings and the open Eskiu buffers by path.
function env() {
    const buffers = new Map();
    for (const d of vscode.workspace.textDocuments) {
        if (d.languageId === 'eskiu' && !d.isClosed) buffers.set(docPath(d), d.getText());
    }
    return {
        settings: settings(), buffers, trusted: vscode.workspace.isTrusted,
        folders: (vscode.workspace.workspaceFolders || []).map((f) => f.uri.fsPath),
        onMissing: reportMissingCompiler, onOutdated: reportOutdatedCompiler,
    };
}

// The URI of `file`: the open document's own, else a file URI.
function uriOf(file) {
    for (const d of vscode.workspace.textDocuments) {
        const p = d.isUntitled ? untitled.get(d.uri.toString()) : path.resolve(d.uri.fsPath);
        if (p === file) return d.uri;
    }
    return vscode.Uri.file(file);
}

const SEVERITY = () => ({ error: vscode.DiagnosticSeverity.Error, warning: vscode.DiagnosticSeverity.Warning,
                          info: vscode.DiagnosticSeverity.Information });

async function validate(document) {
    if (document.languageId !== 'eskiu' || document.isClosed) return;
    const version = document.version;
    const found = await project.check(docPath(document), env());
    if (found == null || document.isClosed || document.version !== version) return;
    diagnosticCollection.set(document.uri, found.map((f) => {
        const d = new vscode.Diagnostic(new vscode.Range(f.line, f.start, f.line, f.end), f.message, SEVERITY()[f.severity]);
        d.source = 'eskiuc';
        return d;
    }));
}

function scheduleValidate(document) {
    const s = settings();
    if (document.languageId !== 'eskiu' || !s.checkOnType) return;
    const key = document.uri.toString();
    clearTimeout(pendingChecks.get(key));
    pendingChecks.set(key, setTimeout(() => {
        pendingChecks.delete(key);
        validate(document);
    }, s.checkDelay));
}

// A saved file changes what the files of its program see: check every open document again.
function validateAll() {
    vscode.workspace.textDocuments.forEach(validate);
}

const pos = (p) => ({ line: p.line, character: p.character });

async function hover(document, position) {
    const { out } = await project.query(docPath(document), env(), [], '--hover-at', pos(position), 5000);
    const h = features.hover(out);
    return h && new vscode.Hover(new vscode.MarkdownString(features.hoverMarkdown(h)));
}

async function definition(document, position) {
    const d = await project.definition(docPath(document), env(), pos(position));
    return d && new vscode.Location(uriOf(d.file), new vscode.Position(d.line, d.character));
}

async function references(document, position) {
    const { refs } = await project.references(docPath(document), env(), pos(position));
    return refs.map((r) => new vscode.Location(uriOf(r.file), new vscode.Range(r.line, r.start, r.line, r.end)));
}

async function renamePlan(document, position) {
    const file = docPath(document);
    const { refs, members } = await project.references(file, env(), pos(position));
    const plan = features.renamePlan(refs, members, file, pos(position));
    if (plan.error) throw new Error(plan.error);
    return plan;
}

async function prepareRename(document, position) {
    const { here } = await renamePlan(document, position);
    return new vscode.Range(here.line, here.start, here.line, here.end);
}

async function rename(document, position, newName) {
    if (!features.validName(newName)) throw new Error(`'${newName}' is not a valid name.`);
    const { edits } = await renamePlan(document, position);
    const edit = new vscode.WorkspaceEdit();
    for (const e of edits) edit.replace(uriOf(e.file), new vscode.Range(e.line, e.start, e.line, e.end), newName);
    return edit;
}

const COMPLETION_KINDS = () => ({
    field: vscode.CompletionItemKind.Field, method: vscode.CompletionItemKind.Method,
    function: vscode.CompletionItemKind.Function, variable: vscode.CompletionItemKind.Variable,
    global: vscode.CompletionItemKind.Variable, struct: vscode.CompletionItemKind.Struct,
    interface: vscode.CompletionItemKind.Interface, enum: vscode.CompletionItemKind.Enum,
    enummember: vscode.CompletionItemKind.EnumMember, type: vscode.CompletionItemKind.TypeParameter,
    keyword: vscode.CompletionItemKind.Keyword,
});

async function complete(document, position) {
    const ctx = features.completionContext(document.lineAt(position.line).text.slice(0, position.character));
    if (!ctx) return [];
    const { out } = await project.query(docPath(document), env(), [], '--complete-at', pos(position), 8000);
    const kinds = COMPLETION_KINDS();
    return features.completionItems(out, ctx.member).map((i) => {
        const item = new vscode.CompletionItem(i.label, kinds[i.kind]);
        item.detail = i.detail;
        item.sortText = i.sortText;
        return item;
    });
}

async function signatureHelp(document, position) {
    const text = document.getText();
    const call = features.enclosingCall(text, document.offsetAt(position));
    if (!call) return null;
    const { out } = await project.query(docPath(document), env(), [], '--hover-at', pos(document.positionAt(call.offset)), 5000);
    const sig = features.signature(out, call);
    if (!sig) return null;
    const info = new vscode.SignatureInformation(sig.label);
    info.parameters = sig.params.map((p) => new vscode.ParameterInformation(p));
    const help = new vscode.SignatureHelp();
    help.signatures = [info];
    help.activeSignature = 0;
    help.activeParameter = sig.active;
    return help;
}

const SYMBOL_KINDS = () => ({
    function: vscode.SymbolKind.Function, method: vscode.SymbolKind.Method, struct: vscode.SymbolKind.Struct,
    field: vscode.SymbolKind.Field, enum: vscode.SymbolKind.Enum, interface: vscode.SymbolKind.Interface,
    type: vscode.SymbolKind.TypeParameter, variable: vscode.SymbolKind.Variable, constant: vscode.SymbolKind.Constant,
});

async function documentSymbols(document) {
    const rows = await project.symbols(docPath(document), env());
    const kinds = SYMBOL_KINDS();
    const convert = (s) => {
        const sel = new vscode.Range(s.line, s.character, s.line, s.character + s.name.length);
        const sym = new vscode.DocumentSymbol(s.name, s.detail, kinds[s.kind] || vscode.SymbolKind.Variable,
            new vscode.Range(s.line, s.character, s.endLine, s.endCharacter), sel);
        sym.children = s.children.map(convert);
        return sym;
    };
    return features.symbolTree(rows.filter((r) => r.line < document.lineCount),
                               (n) => document.lineAt(n).text.length).map(convert);
}

function quickFixes(document, range, context) {
    const diags = context.diagnostics.map((d) => ({ line: d.range.start.line, start: d.range.start.character,
                                                    message: d.message, source: d.source, original: d }));
    return features.quickFixes((n) => (n < document.lineCount ? document.lineAt(n).text : ''), diags).map((f) => {
        const fix = new vscode.CodeAction(f.title, vscode.CodeActionKind.QuickFix);
        fix.edit = new vscode.WorkspaceEdit();
        fix.edit.replace(document.uri, new vscode.Range(f.line, f.start, f.line, f.end), f.newText);
        fix.diagnostics = [f.diagnostic];
        fix.isPreferred = true;
        return fix;
    });
}

async function format(document) {
    const text = document.getText();
    const formatted = await project.format(text, env(), docPath(document));
    if (formatted == null || formatted === text) return [];
    const all = new vscode.Range(document.positionAt(0), document.positionAt(text.length));
    return [vscode.TextEdit.replace(all, formatted)];
}

function quote(s) {
    return process.platform === 'win32' ? `"${s}"` : `'${s.replace(/'/g, `'\\''`)}'`;
}

async function runFile(uri) {
    const editor = vscode.window.activeTextEditor;
    const document = uri
        ? await vscode.workspace.openTextDocument(uri)
        : editor && editor.document;
    if (!document || document.languageId !== 'eskiu') {
        vscode.window.showInformationMessage('Eskiu: open a .esk file to run it.');
        return;
    }
    if (document.isUntitled) {
        vscode.window.showInformationMessage('Eskiu: save the file before running it.');
        return;
    }
    if (document.isDirty) await document.save();
    let terminal = vscode.window.terminals.find((t) => t.name === 'Eskiu');
    if (!terminal) terminal = vscode.window.createTerminal('Eskiu');
    terminal.show(true);
    const compiler = project.findCompiler(env(), document.uri.fsPath);
    terminal.sendText(`${quote(compiler)} run ${quote(document.uri.fsPath)}`);
}

function activate(context) {
    diagnosticCollection = vscode.languages.createDiagnosticCollection('eskiu');
    context.subscriptions.push(diagnosticCollection);

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(validate),
        vscode.workspace.onDidSaveTextDocument(validateAll),
        vscode.workspace.onDidChangeTextDocument((e) => scheduleValidate(e.document)),
        vscode.workspace.onDidCloseTextDocument((doc) => {
            clearTimeout(pendingChecks.get(doc.uri.toString()));
            pendingChecks.delete(doc.uri.toString());
            diagnosticCollection.delete(doc.uri);
            const tmp = untitled.get(doc.uri.toString());
            if (tmp) { untitled.delete(doc.uri.toString()); fs.rm(path.dirname(tmp), { recursive: true, force: true }, () => {}); }
        }),
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('eskiu')) {
                missingReported = false;
                outdatedReported = false;
                validateAll();
            }
        }),
        vscode.languages.registerHoverProvider('eskiu', { provideHover: hover }),
        vscode.languages.registerDefinitionProvider('eskiu', { provideDefinition: definition }),
        vscode.languages.registerReferenceProvider('eskiu', { provideReferences: references }),
        vscode.languages.registerRenameProvider('eskiu', { provideRenameEdits: rename, prepareRename }),
        vscode.languages.registerCompletionItemProvider('eskiu', { provideCompletionItems: complete }, '.'),
        vscode.languages.registerSignatureHelpProvider('eskiu', { provideSignatureHelp: signatureHelp }, '(', ','),
        vscode.languages.registerDocumentSymbolProvider('eskiu', { provideDocumentSymbols: documentSymbols }),
        vscode.languages.registerCodeActionsProvider('eskiu', { provideCodeActions: quickFixes },
            { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }),
        vscode.languages.registerDocumentFormattingEditProvider('eskiu', { provideDocumentFormattingEdits: format }),
        vscode.commands.registerCommand('eskiu.runFile', runFile),
    );

    validateAll();
}

function deactivate() {
    for (const t of pendingChecks.values()) clearTimeout(t);
    if (diagnosticCollection) diagnosticCollection.dispose();
    for (const tmp of untitled.values()) fs.rm(path.dirname(tmp), { recursive: true, force: true }, () => {});
}

module.exports = { activate, deactivate };
