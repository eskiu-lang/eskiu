#!/usr/bin/env node
/**
 * server.js: the Eskiu language server, for editors that speak LSP (Neovim, Helix, Zed, and
 * JetBrains IDEs through the Eskiu IntelliJ plugin with LSP4IJ). JSON-RPC over stdio, Node.js
 * 16+, no npm packages.
 *
 * It does what the VS Code extension (extension.js) does, through the same modules: project.js
 * runs the compiler on a document in its program, features.js turns the output into hover,
 * completion, signature help, the outline, quick fixes and rename edits.
 *
 * Settings (the extension's `eskiu.*`): initializationOptions or workspace/didChangeConfiguration
 * (`{ eskiu: {...} }` or the keys themselves): compilerPath, compilerFlags, entryPoint,
 * checkOnType, checkDelay; and searchWorkspace (the client trusts the workspace, so build
 * directories in it may provide the compiler). Environment defaults: ESKIUC, ESKIU_FLAGS,
 * ESKIU_ENTRY_POINT, ESKIU_CHECK_ON_TYPE, ESKIU_CHECK_DELAY, ESKIU_SEARCH_WORKSPACE=1.
 */

'use strict';

const path = require('path');
const fs   = require('fs');
const { fileURLToPath, pathToFileURL } = require('url');
const project = require('./project');
const features = require('./features');

// ── LSP message framing ───────────────────────────────────────────────────────

let buffer = Buffer.alloc(0);

process.stdin.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (true) {
        const headerEnd = buffer.indexOf('\r\n\r\n');
        if (headerEnd === -1) break;
        const headers = buffer.slice(0, headerEnd).toString();
        const lenMatch = headers.match(/Content-Length:\s*(\d+)/i);
        if (!lenMatch) { buffer = buffer.slice(headerEnd + 4); continue; }
        const len = parseInt(lenMatch[1], 10);
        if (buffer.length < headerEnd + 4 + len) break;
        const body = buffer.slice(headerEnd + 4, headerEnd + 4 + len).toString();
        buffer = buffer.slice(headerEnd + 4 + len);
        let msg = null;
        try { msg = JSON.parse(body); } catch {}
        if (msg) handleMessage(msg);
    }
});

function send(obj) {
    const body = JSON.stringify(obj);
    process.stdout.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

// ── Settings ──────────────────────────────────────────────────────────────────

let settings = project.normalizeSettings({
    compilerFlags: process.env.ESKIU_FLAGS || '',
    entryPoint: process.env.ESKIU_ENTRY_POINT || '',
    checkOnType: process.env.ESKIU_CHECK_ON_TYPE,
    checkDelay: process.env.ESKIU_CHECK_DELAY,
});
let searchWorkspace = process.env.ESKIU_SEARCH_WORKSPACE === '1';
let folders = [];

function applySettings(raw) {
    if (!raw || typeof raw !== 'object') return;
    const s = raw.eskiu && typeof raw.eskiu === 'object' ? raw.eskiu : raw;
    settings = project.normalizeSettings(s, settings);
    if (typeof s.searchWorkspace === 'boolean') searchWorkspace = s.searchWorkspace;
}

// The extension's own version (serverInfo), read from package.json next to us.
const VERSION = (() => {
    try { return JSON.parse(fs.readFileSync(path.join(__dirname, 'package.json'), 'utf8')).version; }
    catch { return 'unknown'; }
})();

// ── Documents ─────────────────────────────────────────────────────────────────

const docs = new Map();   // uri → current text

function uriToPath(uri) {
    try { return path.resolve(fileURLToPath(uri)); } catch { return null; }
}

// The URI of `file`: the client's own for an open document.
function uriOf(file) {
    for (const uri of docs.keys()) if (uriToPath(uri) === file) return uri;
    return pathToFileURL(file).href;
}

function env() {
    const buffers = new Map();
    for (const [uri, text] of docs) {
        const p = uriToPath(uri);
        if (p && p.endsWith('.esk')) buffers.set(p, text);
    }
    return {
        settings, buffers, folders, trusted: searchWorkspace,
        onMissing: (name) => notify(`Eskiu: could not run '${name}'. Install eskiuc or set compilerPath.`),
        onOutdated: (name) => notify(`Eskiu: '${name}' has no --overlay (it is older than this server); unsaved changes in imported files are not checked. Update eskiuc.`),
    };
}

const notified = new Set();
function notify(message) {
    if (notified.has(message)) return;
    notified.add(message);
    send({ jsonrpc: '2.0', method: 'window/showMessage', params: { type: 2, message } });
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

const SEVERITY = { error: 1, warning: 2, info: 3 };
const pending = new Map();   // uri → timer

async function validate(uri) {
    const file = uriToPath(uri);
    if (!file || !file.endsWith('.esk') || !docs.has(uri)) return;
    const text = docs.get(uri);
    const found = await project.check(file, env());
    // a newer text is being checked, or the document closed
    if (found == null || docs.get(uri) !== text) return;
    send({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics', params: { uri, diagnostics: found.map((d) => ({
        range: { start: { line: d.line, character: d.start }, end: { line: d.line, character: d.end } },
        severity: SEVERITY[d.severity], source: 'eskiuc', message: d.message })) } });
}

function scheduleValidate(uri) {
    if (!settings.checkOnType) return;
    clearTimeout(pending.get(uri));
    pending.set(uri, setTimeout(() => { pending.delete(uri); validate(uri); }, settings.checkDelay));
}

// A saved file changes what the files of its program see: check every open document again.
function validateAll() {
    for (const uri of docs.keys()) validate(uri);
}

// ── Requests ──────────────────────────────────────────────────────────────────

const COMPLETION_KINDS = { field: 5, method: 2, function: 3, variable: 6, global: 6, struct: 22,
                           interface: 8, enum: 13, enummember: 20, type: 25, keyword: 14 };
const SYMBOL_KINDS = { function: 12, method: 6, struct: 23, field: 8, enum: 10, interface: 11,
                       type: 26, variable: 13, constant: 14 };

// A request that fails with a message the client shows (prepareRename, rename).
class RequestFailed extends Error {}

const range = (line, start, end) => ({ start: { line, character: start }, end: { line, character: end } });

async function renamePlan(p) {
    const file = uriToPath(p.textDocument.uri);
    const { refs, members } = await project.references(file, env(), p.position);
    const plan = features.renamePlan(refs, members, file, p.position);
    if (plan.error) throw new RequestFailed(plan.error);
    return plan;
}

const REQUESTS = {
    'textDocument/hover': async (p) => {
        const { out } = await project.query(uriToPath(p.textDocument.uri), env(), [], '--hover-at', p.position, 5000);
        const h = features.hover(out);
        return h && { contents: { kind: 'markdown', value: features.hoverMarkdown(h) } };
    },
    'textDocument/definition': async (p) => {
        const d = await project.definition(uriToPath(p.textDocument.uri), env(), p.position);
        return d && { uri: uriOf(d.file), range: range(d.line, d.character, d.character) };
    },
    'textDocument/references': async (p) => {
        const { refs } = await project.references(uriToPath(p.textDocument.uri), env(), p.position);
        return refs.map((r) => ({ uri: uriOf(r.file), range: range(r.line, r.start, r.end) }));
    },
    'textDocument/prepareRename': async (p) => {
        const { here } = await renamePlan(p);
        return range(here.line, here.start, here.end);
    },
    'textDocument/rename': async (p) => {
        if (!features.validName(p.newName)) throw new RequestFailed(`'${p.newName}' is not a valid name.`);
        const { edits } = await renamePlan(p);
        const changes = {};
        for (const e of edits) (changes[uriOf(e.file)] = changes[uriOf(e.file)] || []).push({ range: range(e.line, e.start, e.end), newText: p.newName });
        return { changes };
    },
    'textDocument/completion': async (p) => {
        const text = docs.get(p.textDocument.uri) || '';
        const line = text.split('\n')[p.position.line] || '';
        const ctx = features.completionContext(line.slice(0, p.position.character));
        if (!ctx) return [];
        const { out } = await project.query(uriToPath(p.textDocument.uri), env(), [], '--complete-at', p.position, 8000);
        const items = features.completionItems(out, ctx.member).map((i) => ({
            label: i.label, kind: COMPLETION_KINDS[i.kind], detail: i.detail, sortText: i.sortText }));
        return { isIncomplete: false, items };
    },
    'textDocument/signatureHelp': async (p) => {
        const text = docs.get(p.textDocument.uri) || '';
        const call = features.enclosingCall(text, features.offsetAt(text, p.position));
        if (!call) return null;
        const { out } = await project.query(uriToPath(p.textDocument.uri), env(), [], '--hover-at',
                                            features.positionAt(text, call.offset), 5000);
        const sig = features.signature(out, call);
        return sig && { signatures: [{ label: sig.label, parameters: sig.params.map((x) => ({ label: x })) }],
                        activeSignature: 0, activeParameter: sig.active };
    },
    'textDocument/documentSymbol': async (p) => {
        const lines = (docs.get(p.textDocument.uri) || '').split('\n');
        const rows = await project.symbols(uriToPath(p.textDocument.uri), env());
        const lsp = (s) => ({ name: s.name, detail: s.detail, kind: SYMBOL_KINDS[s.kind] || 13,
                              range: { start: { line: s.line, character: s.character }, end: { line: s.endLine, character: s.endCharacter } },
                              selectionRange: range(s.line, s.character, s.character + s.name.length),
                              children: s.children.map(lsp) });
        return features.symbolTree(rows.filter((r) => r.line < lines.length), (n) => (lines[n] || '').length).map(lsp);
    },
    'textDocument/codeAction': async (p) => {
        const lines = (docs.get(p.textDocument.uri) || '').split('\n');
        const diags = ((p.context && p.context.diagnostics) || []).map((d) => ({
            line: d.range.start.line, start: d.range.start.character, message: d.message, source: d.source, original: d }));
        return features.quickFixes((n) => lines[n], diags).map((f) => ({
            title: f.title, kind: 'quickfix', diagnostics: [f.diagnostic], isPreferred: true,
            edit: { changes: { [p.textDocument.uri]: [{ range: range(f.line, f.start, f.end), newText: f.newText }] } } }));
    },
    'textDocument/formatting': async (p) => {
        const text = docs.get(p.textDocument.uri);
        if (text == null) return [];
        const formatted = await project.format(text, env(), uriToPath(p.textDocument.uri));
        if (formatted == null || formatted === text) return [];
        const lines = text.split('\n');
        return [{ range: { start: { line: 0, character: 0 }, end: { line: lines.length, character: 0 } }, newText: formatted }];
    },
};

function handleMessage(msg) {
    const { id, method, params } = msg;

    if (method === 'initialize') {
        const opts = (params && params.initializationOptions) || {};
        applySettings(opts);
        folders = ((params && params.workspaceFolders) || []).map((f) => uriToPath(f.uri)).filter(Boolean);
        if (!folders.length && params && params.rootUri) folders = [uriToPath(params.rootUri)].filter(Boolean);
        send({ jsonrpc: '2.0', id, result: {
            capabilities: {
                textDocumentSync: { openClose: true, change: 1, save: { includeText: false } },
                hoverProvider: true,
                definitionProvider: true,
                referencesProvider: true,
                renameProvider: { prepareProvider: true },
                completionProvider: { triggerCharacters: ['.'] },
                signatureHelpProvider: { triggerCharacters: ['(', ','] },
                documentSymbolProvider: true,
                documentFormattingProvider: true,
                codeActionProvider: { codeActionKinds: ['quickfix'] },
            },
            serverInfo: { name: 'eskiu-lsp', version: VERSION },
        }});
        return;
    }

    if (method === 'initialized') return;
    if (method === 'shutdown') { send({ jsonrpc: '2.0', id, result: null }); return; }
    if (method === 'exit')     { process.exit(0); }

    if (method === 'workspace/didChangeConfiguration') {
        applySettings(params && params.settings);
        notified.clear();
        validateAll();
        return;
    }

    if (method === 'textDocument/didOpen') {
        const { uri, text } = params.textDocument;
        docs.set(uri, text);
        validate(uri);
        return;
    }

    if (method === 'textDocument/didChange') {
        const uri = params.textDocument && params.textDocument.uri;
        const changes = params.contentChanges || [];
        const content = changes.length ? changes[changes.length - 1].text : null;
        if (!uri || content == null) return;
        docs.set(uri, content);
        scheduleValidate(uri);
        return;
    }

    if (method === 'textDocument/didSave') {
        validateAll();
        return;
    }

    if (method === 'textDocument/didClose') {
        const uri = params.textDocument.uri;
        clearTimeout(pending.get(uri));
        pending.delete(uri);
        docs.delete(uri);
        send({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics', params: { uri, diagnostics: [] } });
        return;
    }

    const handler = REQUESTS[method];
    if (handler && id !== undefined) {
        handler(params).then(
            (result) => send({ jsonrpc: '2.0', id, result: result === undefined ? null : result }),
            (err) => send(err instanceof RequestFailed
                ? { jsonrpc: '2.0', id, error: { code: -32803, message: err.message } }
                : { jsonrpc: '2.0', id, result: null }));
        return;
    }

    // Respond to unknown requests with null to avoid timeouts
    if (id !== undefined) send({ jsonrpc: '2.0', id, result: null });
}

process.on('uncaughtException', () => {});
