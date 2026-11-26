#!/usr/bin/env node
// Exercises editor/vscode/server.js over stdio (run from tests/run.sh when node is
// available): full-document sync, serverInfo.version read from package.json, and an
// unsaved buffer checked next to its file so a relative import still resolves, with
// only this document's own diagnostics published, and a rename of a function that is
// also dot-called (`P_dist` and `p.dist()`): only `dist` is offered and replaced.
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const root = path.resolve(__dirname, '..', '..');
const server = spawn(process.execPath, [path.join(root, 'editor', 'vscode', 'server.js')]);
const pkg = JSON.parse(fs.readFileSync(path.join(root, 'editor', 'vscode', 'package.json'), 'utf8'));
const docPath = path.join(__dirname, 'unsaved_main.esk');
const uri = 'file://' + docPath;
const text = [
    'import "helper.esk";',
    'int main() {',
    '    int x = "oops";',
    '    return helper_value();',
    '}',
    '',
].join('\n');

const renameUri = 'file://' + path.join(__dirname, 'unsaved_rename.esk');
const renameText = [
    'struct P { int x; }',
    'int P_dist(*P self, int k) { return self.x * k; }',
    'int main() {',
    '    P p = P{x: 1};',
    '    return p.dist(2) + P_dist(&p, 3);',
    '}',
    '',
].join('\n');
const at = { line: 1, character: 5 };

function send(obj) {
    const body = JSON.stringify(obj);
    server.stdin.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

let buf = Buffer.alloc(0);
const failures = [];
let gotInit = false;
const timer = setTimeout(() => { failures.push('timeout'); finish(); }, 20000);

function finish() {
    clearTimeout(timer);
    server.kill();
    if (failures.length) { console.log('FAIL ' + failures.join('; ')); process.exit(1); }
    console.log('ok');
    process.exit(0);
}

server.stdout.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    while (true) {
        const he = buf.indexOf('\r\n\r\n');
        if (he < 0) return;
        const len = parseInt(/Content-Length:\s*(\d+)/i.exec(buf.slice(0, he).toString())[1], 10);
        if (buf.length < he + 4 + len) return;
        const msg = JSON.parse(buf.slice(he + 4, he + 4 + len).toString());
        buf = buf.slice(he + 4 + len);
        if (msg.id === 1) {
            gotInit = true;
            const r = msg.result;
            if (r.capabilities.textDocumentSync.change !== 1) failures.push('textDocumentSync is not full (1)');
            if (r.serverInfo.version !== pkg.version)
                failures.push(`serverInfo.version ${r.serverInfo.version} != package.json ${pkg.version}`);
            send({ jsonrpc: '2.0', method: 'textDocument/didChange', params: {
                textDocument: { uri, version: 2 }, contentChanges: [{ text }] } });
        } else if (msg.method === 'textDocument/publishDiagnostics' && msg.params.uri === uri) {
            const d = msg.params.diagnostics;
            if (d.length !== 1) failures.push(`expected 1 diagnostic, got ${d.length}: ${JSON.stringify(d)}`);
            else if (d[0].range.start.line !== 2) failures.push(`diagnostic on line ${d[0].range.start.line + 1}, expected 3`);
            if (fs.readdirSync(__dirname).some((f) => f.startsWith('.eskiu_lsp_')))
                failures.push('temp file left behind');
            send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: {
                textDocument: { uri: renameUri, languageId: 'eskiu', version: 1, text: renameText } } });
            send({ jsonrpc: '2.0', id: 2, method: 'textDocument/prepareRename', params: {
                textDocument: { uri: renameUri }, position: at } });
        } else if (msg.id === 2) {
            const r = msg.result;
            if (!r || r.start.line !== 1 || r.start.character !== 6 || r.end.character !== 10)
                failures.push(`prepareRename: expected 2:7-2:11 (dist), got ${JSON.stringify(r)}`);
            send({ jsonrpc: '2.0', id: 3, method: 'textDocument/rename', params: {
                textDocument: { uri: renameUri }, position: at, newName: 'len' } });
        } else if (msg.id === 3) {
            const edits = ((msg.result || {}).changes || {})[renameUri] || [];
            const got = edits.map((e) => `${e.range.start.line}:${e.range.start.character}-${e.range.end.character}=${e.newText}`)
                .sort().join(' ');
            const want = '1:6-10=len 4:13-17=len 4:25-29=len';
            if (got !== want) failures.push(`rename: expected ${want}, got ${got}`);
            finish();
        }
    }
});

send({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {} });
