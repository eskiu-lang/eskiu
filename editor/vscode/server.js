#!/usr/bin/env node
/**
 * server.js — Eskiu Language Server
 *
 * LSP server over the compiler's editor modes: diagnostics (--test-typechecker),
 * hover (--hover-at), go to definition (--definition-at), completion (--complete-at),
 * signature help (the callee's --hover-at), document symbols (--symbols),
 * formatting (eskiuc fmt) and a quick fix for "did you mean" errors. For editors that speak LSP: Neovim, Helix, Zed, and
 * JetBrains IDEs through the Eskiu IntelliJ plugin (LSP4IJ).
 *
 * Protocol: JSON-RPC over stdin/stdout (standard LSP transport).
 *
 * Requires: Node.js 16+. No npm dependencies — pure Node.js stdlib.
 *
 * Install (already done if you used the symlink method):
 *   ln -s /path/to/eskiu/editor/vscode ~/.vscode/extensions/eskiu-language
 *   # Then restart VS Code.
 */

'use strict';

const { execFile } = require('child_process');
const path = require('path');
const fs   = require('fs');
const { flatFor } = require('./flatten');

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
        handleMessage(JSON.parse(body));
    }
});

function send(obj) {
    const body = JSON.stringify(obj);
    process.stdout.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

// ── Message handler ───────────────────────────────────────────────────────────

// Path to eskiuc: $ESKIUC, else build22/build walking upwards or next to this file, else PATH
let compilerOverride = '';   // initializationOptions.compilerPath
// initializationOptions.compilerFlags (e.g. -Wall, -Wno-format), else $ESKIU_FLAGS split on spaces
let compilerFlags = (process.env.ESKIU_FLAGS || '').split(/\s+/).filter(Boolean);

function findEskiuc(hintPath) {
    if (compilerOverride) return compilerOverride;
    if (process.env.ESKIUC) {
        try { fs.accessSync(process.env.ESKIUC, fs.constants.X_OK); return process.env.ESKIUC; } catch {}
    }

    const buildNames = ['build22', 'build', 'build-debug', 'build-release'];

    if (hintPath) {
        let cur = path.dirname(path.resolve(hintPath));
        for (let i = 0; i < 6; i++) {
            for (const b of buildNames) {
                const c = path.join(cur, b, 'eskiuc');
                try { fs.accessSync(c, fs.constants.X_OK); return c; } catch {}
            }
            const parent = path.dirname(cur);
            if (parent === cur) break;
            cur = parent;
        }
    }

    const candidates = [
        path.join(__dirname, '..', '..', 'build22', 'eskiuc'),
        path.join(__dirname, '..', '..', 'build', 'eskiuc'),
        path.join(__dirname, '..', '..', '..', 'build22', 'eskiuc'),
        path.join(__dirname, '..', '..', '..', 'build', 'eskiuc'),
    ];
    for (const c of candidates) {
        try { fs.accessSync(c, fs.constants.X_OK); return c; } catch {}
    }
    // The installer's location (an editor opened from the Dock lacks the shell PATH).
    const installed = path.join(require('os').homedir(), '.eskiu', 'bin', 'eskiuc');
    try { fs.accessSync(installed, fs.constants.X_OK); return installed; } catch {}
    return 'eskiuc'; // rely on PATH
}

// The extension's own version (serverInfo), read from package.json next to us.
const VERSION = (() => {
    try { return JSON.parse(fs.readFileSync(path.join(__dirname, 'package.json'), 'utf8')).version; }
    catch { return 'unknown'; }
})();

// Parse "file.esk:8:22: message" → LSP Diagnostic.
function parseErrors(text, checkedPath, origPath, fileContent) {
    const diagnostics = [];
    const re = /^(?:error:\s*)?(.+?):(\d+):(\d+):\s*(.+)$/gm;
    const wantChecked = path.resolve(checkedPath);
    const wantOrig = origPath ? path.resolve(origPath) : null;
    const lines = fileContent ? fileContent.split('\n') : null;
    let m;
    while ((m = re.exec(text)) !== null) {
        const [, file, line, col, msg] = m;
        const resFile = path.resolve(file);
        const isMatch = (resFile === wantChecked) ||
                        (wantOrig && resFile === wantOrig) ||
                        (path.basename(resFile) === path.basename(wantChecked)) ||
                        (wantOrig && path.basename(resFile) === path.basename(wantOrig));
        if (!isMatch) continue;

        const ln = Math.max(0, parseInt(line, 10) - 1);
        const ch = Math.max(0, parseInt(col,  10) - 1);

        let endCh = ch + 1;
        if (lines && ln < lines.length) {
            const lineText = lines[ln];
            while (endCh < lineText.length && /[a-zA-Z0-9_]/.test(lineText[endCh])) {
                endCh++;
            }
        }

        diagnostics.push({
            range: { start: { line: ln, character: ch },
                     end:   { line: ln, character: endCh } },
            severity: msg.startsWith('warning') ? 2 : 1,
            source:   'eskiuc',
            message:  msg.replace(/^error:\s*/, '').replace(/^warning:\s*/, ''),
        });
    }
    return diagnostics;
}

// Run eskiuc and publish diagnostics for a document. A part of a module (flatten.js) is checked
// in its module; any other file alone, an unsaved buffer through a hidden sibling copy so its
// relative imports resolve.
function validate(uri, filePath, content) {
    const compiler = findEskiuc(filePath);
    let fileText = content;
    if (fileText == null) {
        try { fileText = fs.readFileSync(filePath, 'utf8'); } catch {}
    }
    const publish = (diagnostics) => send({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics',
                                           params: { uri, diagnostics } });
    let flat = null;
    try { flat = flatFor(filePath, content); } catch {}
    if (flat) {
        execFile(compiler, [flat.path, ...compilerFlags, '--test-typechecker'], { timeout: 15000, cwd: path.dirname(flat.path) },
            (err, stdout, stderr) => {
                flat.remove();
                const output = (stdout || '') + (stderr || '');
                // the flat file's lines back to their files: this document's only
                const mapped = output.replace(/^((?:error:\s*)?)(.+?):(\d+):(\d+):/gm, (all, pre, file, line, col) => {
                    if (path.resolve(file) !== path.resolve(flat.path)) return all;
                    const o = flat.toOrig(parseInt(line, 10));
                    return o && o.file === filePath ? `${pre}${filePath}:${o.line + 1}:${col}:` : `${pre}(other):0:0:`;
                });
                publish(parseErrors(mapped, filePath, filePath, fileText));
            });
        return;
    }
    let target = filePath;
    let tmp = null;
    if (content != null) {
        tmp = path.join(path.dirname(filePath), `.eskiu_lsp_${process.pid}_${Date.now()}.esk`);
        try { fs.writeFileSync(tmp, content); target = tmp; } catch { tmp = null; }
    }
    execFile(compiler, [target, ...compilerFlags, '--test-typechecker'], { timeout: 10000, cwd: path.dirname(target) },
        (err, stdout, stderr) => {
            if (tmp) { try { fs.rmSync(tmp, { force: true }); } catch {} }
            const output = (stdout || '') + (stderr || '');
            publish(parseErrors(output, target, filePath, fileText));
        });
}

// ── Requests over the compiler's editor modes ─────────────────────────────────

const docs = new Map();   // uri → current text

function run(args, cwd, timeout) {
    return new Promise((resolve) => {
        execFile(findEskiuc(cwd && path.join(cwd, 'x.esk')), args, { timeout, cwd },
            (err, stdout) => resolve(stdout || ''));
    });
}

// Run `eskiuc <file> args` on the document's current text: an unsaved buffer goes
// through a hidden sibling file, so its relative imports resolve. With `pos` (0-based), `flag`
// gets it as LINE:COL; a part of a module runs in its flattened module (flatten.js), the position
// moved there. `loc(file, line)` maps a location in the output back to { uri, line } (0-based).
async function onText(uri, args, timeout = 8000, flag = null, pos = null) {
    const orig = uriToPath(uri);
    const text = docs.get(uri);
    const own = (file) => file === orig || path.basename(file).startsWith(`.${path.basename(orig)}.`);
    const toUri = (file) => 'file://' + path.resolve(file).split('/').map(encodeURIComponent).join('/');
    let flat = null;
    try { flat = flatFor(orig, text); } catch {}
    if (flat) {
        try {
            const line = pos ? flat.toFlat(orig, pos.line) : 0;
            const extra = flag ? [flag, `${line || pos.line + 1}:${pos.character + 1}`] : [];
            const out = await run([flat.path, ...args, ...extra], path.dirname(flat.path), timeout);
            return { out, file: flat.path, loc: (file, ln) => {
                if (path.resolve(file) !== path.resolve(flat.path)) return { uri: toUri(file), line: ln - 1 };
                const o = flat.toOrig(ln);
                return o ? { uri: o.file === orig ? uri : toUri(o.file), line: o.line } : null;
            } };
        } finally {
            flat.remove();
        }
    }
    const extra = flag ? [flag, `${pos.line + 1}:${pos.character + 1}`] : [];
    const loc = (file, ln) => ({ uri: own(path.resolve(file)) ? uri : toUri(file), line: ln - 1 });
    let onDisk = null;
    try { onDisk = fs.readFileSync(orig, 'utf8'); } catch {}
    if (text == null || text === onDisk) return { out: await run([orig, ...args, ...extra], path.dirname(orig), timeout), file: orig, loc };
    const tmp = path.join(path.dirname(orig), `.${path.basename(orig)}.${process.pid}.${Date.now()}.eskiu-check.esk`);
    try {
        fs.writeFileSync(tmp, text);
        return { out: await run([tmp, ...args, ...extra], path.dirname(orig), timeout), file: tmp, loc };
    } catch {
        return { out: '', file: tmp, loc };
    } finally {
        fs.rm(tmp, { force: true }, () => {});
    }
}

// --references-at: one FILE:LINE:COL:WIDTH per line; the checked file (a temp copy of
// an unsaved buffer) is this document.
async function references(uri, position) {
    const { out, loc } = await onText(uri, [], 5000, '--references-at', position);
    const refs = [];
    for (const row of out.split('\n')) {
        const m = row.trim().match(/^(.+):(\d+):(\d+):(\d+)$/);
        if (!m) continue;
        const where = loc(m[1], parseInt(m[2], 10));
        if (!where) continue;
        const target = where.uri, line = where.line, ch = parseInt(m[3], 10) - 1;
        refs.push({ uri: target, range: { start: { line, character: ch },
                                          end: { line, character: ch + parseInt(m[4], 10) } } });
    }
    return refs;
}
const lastLine = (out) => (out.trim().split('\n').pop() || '').trim();

const COMPLETION_KINDS = { field: 5, method: 2, function: 3, variable: 6, global: 6, struct: 22,
                           interface: 8, enum: 13, enummember: 20, type: 25 };
const SYMBOL_KINDS = { function: 12, method: 6, struct: 23, field: 8, enum: 10, interface: 11,
                       type: 26, variable: 13, constant: 14 };
const KEYWORDS = [
    'if', 'else', 'for', 'in', 'while', 'do', 'switch', 'case', 'default', 'break', 'continue',
    'return', 'match', 'struct', 'union', 'enum', 'interface', 'type', 'let', 'const', 'static',
    'volatile', 'extern', 'intrinsic', 'import', 'async', 'await', 'try', 'catch', 'finally',
    'throw', 'defer', 'errdefer', 'sizeof', 'null', 'true', 'false', 'fn', 'operator', 'packed',
    'escaping', 'must_use', 'alloc_with', 'free_closure', 'thread_create', 'thread_join',
    'int', 'int8', 'int16', 'int32', 'int64', 'uint', 'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'bool', 'char', 'string', 'void',
];

// The call around `pos` in `text`: the callee name's position, the argument index and
// whether it is a dot-call (scanning back over balanced brackets and strings).
function enclosingCall(text, pos) {
    const lines = text.split('\n');
    let off = 0;
    for (let i = 0; i < pos.line && i < lines.length; i++) off += lines[i].length + 1;
    off += pos.character;
    let depth = 0, commas = 0, inStr = false;
    for (let i = off - 1; i >= 0 && i > off - 4000; i--) {
        const c = text[i];
        if (inStr) { if (c === '"' && text[i - 1] !== '\\') inStr = false; continue; }
        if (c === '"') { inStr = true; continue; }
        if (c === ')' || c === ']' || c === '}') depth++;
        else if (c === '[' || c === '{') { if (depth === 0) return null; depth--; }
        else if (c === '(') {
            if (depth > 0) { depth--; continue; }
            let j = i;
            while (j > 0 && /\s/.test(text[j - 1])) j--;
            let k = j;
            while (k > 0 && /[A-Za-z0-9_]/.test(text[k - 1])) k--;
            if (k === j) return null;
            const before = text.slice(0, k).split('\n');
            return { pos: { line: before.length - 1, character: before[before.length - 1].length },
                     active: commas, dot: text[k - 1] === '.' };
        } else if (c === ',' && depth === 0) commas++;
        else if (c === ';' && depth === 0) return null;
    }
    return null;
}

const REQUESTS = {
    'textDocument/hover': async (p) => {
        const { out } = await onText(p.textDocument.uri, [], 5000, '--hover-at', p.position);
        const t = lastLine(out);
        if (!t || t.startsWith('(') || t.startsWith('error:')) return null;
        return { contents: { kind: 'markdown', value: '```eskiu\n' + t + '\n```' } };
    },
    'textDocument/definition': async (p) => {
        const { out, loc } = await onText(p.textDocument.uri, [], 5000, '--definition-at', p.position);
        const m = out.trim().match(/^(.+):(\d+):(\d+)$/m);
        if (!m) return null;
        const where = loc(m[1], parseInt(m[2], 10));
        if (!where) return null;
        const pos = { line: where.line, character: parseInt(m[3], 10) - 1 };
        return { uri: where.uri, range: { start: pos, end: pos } };
    },
    'textDocument/references': async (p) => {
        const refs = await references(p.textDocument.uri, p.position);
        return refs.map(({ uri, range }) => ({ uri, range }));
    },
    'textDocument/prepareRename': async (p) => {
        const refs = await references(p.textDocument.uri, p.position);
        const here = refs.find((r) => r.uri === p.textDocument.uri && r.range.start.line === p.position.line &&
            r.range.start.character <= p.position.character && p.position.character <= r.range.end.character);
        if (!here || refs.some((r) => r.uri !== p.textDocument.uri)) return null;
        // The part rename replaces: `dist` of a `P_dist` that is also dot-called (`p.dist()`).
        const w = Math.min(...refs.map((r) => r.range.end.character - r.range.start.character));
        return { start: { line: here.range.end.line, character: here.range.end.character - w }, end: here.range.end };
    },
    'textDocument/rename': async (p) => {
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(p.newName) || KEYWORDS.includes(p.newName)) return null;
        const refs = await references(p.textDocument.uri, p.position);
        // A symbol declared in another file (an import) is not renamed from here.
        if (!refs.length || refs.some((r) => r.uri !== p.textDocument.uri)) return null;
        // A wider declaration (`P_sum` for `p.sum()`) keeps its prefix: only the name is replaced.
        const w = Math.min(...refs.map((r) => r.range.end.character - r.range.start.character));
        return { changes: { [p.textDocument.uri]: refs.map((r) => ({
            range: { start: { line: r.range.start.line, character: r.range.end.character - w }, end: r.range.end },
            newText: p.newName })) } };
    },
    'textDocument/completion': async (p) => {
        const text = docs.get(p.textDocument.uri) || '';
        const line = text.split('\n')[p.position.line] || '';
        const before = line.slice(0, p.position.character);
        if (/^\s*(\/\/|#)/.test(before) || before.split('"').length % 2 === 0) return [];
        const member = /\.\s*[A-Za-z_0-9]*$/.test(before);
        const { out } = await onText(p.textDocument.uri, [], 8000, '--complete-at', p.position);
        const items = [];
        for (const row of out.split('\n')) {
            const [kind, name, detail] = row.split('\t');
            if (!name || !COMPLETION_KINDS[kind]) continue;
            items.push({ label: name, kind: COMPLETION_KINDS[kind], detail: detail || '',
                         sortText: (kind === 'variable' ? '0' : kind === 'field' ? '1' : kind === 'method' ? '2' : '5') + name });
        }
        if (!member) for (const k of KEYWORDS) items.push({ label: k, kind: 14, sortText: '9' + k });
        return { isIncomplete: false, items };
    },
    'textDocument/signatureHelp': async (p) => {
        const call = enclosingCall(docs.get(p.textDocument.uri) || '', p.position);
        if (!call) return null;
        const { out } = await onText(p.textDocument.uri, [], 5000, '--hover-at', call.pos);
        const sig = lastLine(out);
        const open = sig.indexOf('(');
        if (open < 0 || !sig.endsWith(')') || sig.startsWith('fn(')) return null;
        const params = [];
        let depth = 0, cur = '';
        for (const c of sig.slice(open + 1, -1)) {
            if ('(<['.includes(c)) depth++;
            if (')>]'.includes(c)) depth--;
            if (c === ',' && depth === 0) { params.push(cur.trim()); cur = ''; } else cur += c;
        }
        if (cur.trim()) params.push(cur.trim());
        const shown = call.dot && params.length && /\bself$/.test(params[0]) ? params.slice(1) : params;
        const variadic = shown.length && shown[shown.length - 1] === '...';
        return { signatures: [{ label: sig, parameters: shown.map((x) => ({ label: x })) }],
                 activeSignature: 0,
                 activeParameter: variadic ? Math.min(call.active, shown.length - 1) : call.active };
    },
    'textDocument/documentSymbol': async (p) => {
        const { out } = await onText(p.textDocument.uri, ['--symbols']);
        const lines = (docs.get(p.textDocument.uri) || '').split('\n');
        const top = [];
        const structs = new Map();
        for (const row of out.split('\n')) {
            const [kind, name, pos, container, detail] = row.split('\t');
            if (!name || !SYMBOL_KINDS[kind] || !pos) continue;
            const [l, c] = pos.split(':').map((n) => Math.max(0, parseInt(n, 10) - 1));
            const end = { line: l, character: (lines[l] || '').length };
            const sym = { name, detail: detail || '', kind: SYMBOL_KINDS[kind],
                          range: { start: { line: l, character: c }, end },
                          selectionRange: { start: { line: l, character: c },
                                            end: { line: l, character: c + name.length } },
                          children: [] };
            const parent = container && structs.get(container);
            if (parent) {
                parent.children.push(sym);
                if (parent.range.end.line < l) parent.range.end = end;
            } else {
                top.push(sym);
                if (kind === 'struct') structs.set(name, sym);
            }
        }
        return top;
    },
    // "undefined variable 'totl' (did you mean 'total'?)": replace the name on the
    // diagnostic's line nearest to its column.
    'textDocument/codeAction': async (p) => {
        const lines = (docs.get(p.textDocument.uri) || '').split('\n');
        const actions = [];
        for (const d of (p.context && p.context.diagnostics) || []) {
            const m = (d.message || '').match(/(?:variable|function|method|member|type) '([^']+)'[^]*\(did you mean '([^']+)'\?\)/);
            if (!m) continue;
            const [, wrong, right] = m;
            const ln = d.range.start.line;
            const text = lines[ln] || '';
            const re = new RegExp('\\b' + wrong.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\b', 'g');
            let best = -1, hit;
            while ((hit = re.exec(text)) !== null) {
                if (best < 0 || Math.abs(hit.index - d.range.start.character) < Math.abs(best - d.range.start.character)) best = hit.index;
            }
            if (best < 0) continue;
            actions.push({ title: `Change to '${right}'`, kind: 'quickfix', diagnostics: [d], isPreferred: true,
                           edit: { changes: { [p.textDocument.uri]: [{
                               range: { start: { line: ln, character: best }, end: { line: ln, character: best + wrong.length } },
                               newText: right }] } } });
        }
        return actions;
    },
    'textDocument/formatting': async (p) => {
        const text = docs.get(p.textDocument.uri);
        if (text == null) return [];
        const tmp = path.join(require('os').tmpdir(), `eskiu-fmt-${process.pid}-${Date.now()}.esk`);
        try {
            fs.writeFileSync(tmp, text);
            await run(['fmt', tmp], undefined, 10000);
            const formatted = fs.readFileSync(tmp, 'utf8');
            if (formatted === text) return [];
            const lines = text.split('\n');
            return [{ range: { start: { line: 0, character: 0 },
                               end: { line: lines.length, character: 0 } }, newText: formatted }];
        } catch {
            return [];
        } finally {
            fs.rm(tmp, { force: true }, () => {});
        }
    },
};

// URI → filesystem path
function uriToPath(uri) {
    return decodeURIComponent(uri.replace(/^file:\/\//, ''));
}

function handleMessage(msg) {
    const { id, method, params } = msg;

    if (method === 'initialize') {
        const opts = (params && params.initializationOptions) || {};
        if (typeof opts.compilerPath === 'string') compilerOverride = opts.compilerPath.trim();
        if (Array.isArray(opts.compilerFlags)) compilerFlags = opts.compilerFlags.filter(f => typeof f === 'string');
        send({ jsonrpc: '2.0', id, result: {
            capabilities: {
                textDocumentSync: 1,          // full: every change carries the whole text
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

    if (method === 'textDocument/didOpen') {
        const { uri, text } = params.textDocument;
        docs.set(uri, text);
        if (uri.endsWith('.esk')) validate(uri, uriToPath(uri), text);
        return;
    }

    if (method === 'textDocument/didSave') {
        const { uri } = params.textDocument;
        if (uri.endsWith('.esk')) validate(uri, uriToPath(uri));
        return;
    }

    if (method === 'textDocument/didChange') {
        const uri = params.textDocument && params.textDocument.uri;
        const contentChanges = params.contentChanges;
        if (!uri || !uri.endsWith('.esk')) return;
        const content = contentChanges?.[contentChanges.length - 1]?.text;
        if (content == null) return;
        docs.set(uri, content);
        validate(uri, uriToPath(uri), content);
        return;
    }

    if (method === 'textDocument/didClose') {
        docs.delete(params.textDocument.uri);
        send({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics',
               params: { uri: params.textDocument.uri, diagnostics: [] } });
        return;
    }

    const handler = REQUESTS[method];
    if (handler && id !== undefined) {
        handler(params).then(
            (result) => send({ jsonrpc: '2.0', id, result: result === undefined ? null : result }),
            () => send({ jsonrpc: '2.0', id, result: null }));
        return;
    }

    // Respond to unknown requests with null to avoid timeouts
    if (id !== undefined) send({ jsonrpc: '2.0', id, result: null });
}

process.on('uncaughtException', () => {});
