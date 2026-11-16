// Parts of a module in the language server: a.esk imports b.esk then c.esk, and c.esk uses a
// type b.esk declares without importing it. Opened alone, c.esk must check clean in its module,
// and hover, definition and completion must see b.esk's declarations; an error in c.esk is
// reported on c.esk's own line.
//     node tests/editor/lsp_parts_test.js    (ESKIUC picks the compiler; prints ok)
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-parts-'));
fs.writeFileSync(path.join(dir, 'a.esk'), 'import "b.esk";\nimport "c.esk";\n\nint main() {\n    return total(Point{x: 1, y: 2});\n}\n');
fs.writeFileSync(path.join(dir, 'b.esk'), '// b.esk: part of a.esk\n\nstruct Point {\n    int x;\n    int y;\n}\n');
const c = '// c.esk: part of a.esk, after b.esk\n\nint total(Point p) {\n    return p.x + p.y;\n}\n';
fs.writeFileSync(path.join(dir, 'c.esk'), c);
const uri = 'file://' + path.join(dir, 'c.esk');

const server = spawn(process.execPath, [path.join(__dirname, '..', '..', 'editor', 'vscode', 'server.js')], { stdio: ['pipe', 'pipe', 'inherit'] });
let buf = Buffer.alloc(0);
const waiting = [];
const notes = [];
server.stdout.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
        const h = buf.indexOf('\r\n\r\n');
        if (h < 0) return;
        const len = parseInt(buf.slice(0, h).toString().match(/Content-Length: (\d+)/i)[1], 10);
        if (buf.length < h + 4 + len) return;
        const msg = JSON.parse(buf.slice(h + 4, h + 4 + len).toString());
        buf = buf.slice(h + 4 + len);
        const w = waiting.findIndex((x) => x.test(msg));
        if (w >= 0) waiting.splice(w, 1)[0].resolve(msg); else notes.push(msg);
    }
});
const send = (m) => { const b = JSON.stringify(m); server.stdin.write(`Content-Length: ${Buffer.byteLength(b)}\r\n\r\n${b}`); };
const wait = (test) => new Promise((resolve) => {
    const i = notes.findIndex(test);
    if (i >= 0) return resolve(notes.splice(i, 1)[0]);
    waiting.push({ test, resolve });
});
let next = 1;
const request = (method, params) => { const id = next++; send({ jsonrpc: '2.0', id, method, params }); return wait((m) => m.id === id); };
const diagnostics = () => wait((m) => m.method === 'textDocument/publishDiagnostics' && m.params.uri === uri);

let fails = 0;
const check = (what, ok, got) => { if (!ok) { fails++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

(async () => {
    await request('initialize', { initializationOptions: process.env.ESKIUC ? { compilerPath: process.env.ESKIUC } : {} });
    send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri, languageId: 'eskiu', version: 1, text: c } } });
    const d1 = await diagnostics();
    check('a part that uses another part checks clean', d1.params.diagnostics.length === 0, d1.params.diagnostics);
    const hover = await request('textDocument/hover', { textDocument: { uri }, position: { line: 2, character: 16 } });
    check('hover on the parameter sees the other part\'s type', hover.result && /Point/.test(hover.result.contents.value), hover.result);
    const def = await request('textDocument/definition', { textDocument: { uri }, position: { line: 3, character: 13 } });
    check('definition of a field is in b.esk', def.result && def.result.uri.endsWith('/b.esk') && def.result.range.start.line === 3, def.result);
    const comp = await request('textDocument/completion', { textDocument: { uri }, position: { line: 3, character: 13 } });
    check('completion of a member of the other part\'s struct', comp.result && comp.result.items.some((i) => i.label === 'x'), comp.result && comp.result.items.slice(0, 5));
    // an unsaved error in c.esk, on its own line
    const bad = c.replace('p.x + p.y', 'p.x + p.z');
    send({ jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri, version: 2 }, contentChanges: [{ text: bad }] } });
    const d2 = await diagnostics();
    check('an error in the part is reported on its line', d2.params.diagnostics.length === 1 && d2.params.diagnostics[0].range.start.line === 3, d2.params.diagnostics);
    send({ jsonrpc: '2.0', id: next++, method: 'shutdown' });
    send({ jsonrpc: '2.0', method: 'exit' });
    fs.rmSync(dir, { recursive: true, force: true });
    console.log(fails ? `${fails} failed` : 'ok');
    process.exit(fails ? 1 : 0);
})();
