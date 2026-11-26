// A file is checked in its program, over the language server (editor/vscode/server.js): a package
// whose src/main.esk imports control.esk, then world.esk -> island.esk -> atmos.esk, where
// atmos.esk calls control.esk's bounded() without importing it. island.esk imports atmos.esk but
// not control.esk, so checking atmos.esk from island.esk reported "undefined function 'bounded'".
// Also: an unsaved edit in an indirectly imported file, columns after a non-ASCII string, and
// references and rename across files (a function and a dot-called method used from other files;
// strings, comments and same-named locals untouched).
//     node tests/editor/lsp_program_test.js    (ESKIUC picks the compiler; prints ok)
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-program-')));
const src = path.join(dir, 'src');
fs.mkdirSync(src);
const files = {
    'eskiu.toml': '[package]\nname = "demo"\nversion = "0.1.0"\n',
    'src/main.esk': 'import "geom.esk";\nimport "control.esk";\nimport "world.esk";\nimport "uses.esk";\nimport "mathy.esk";\n\nint main() {\n    return world_tick(3) + use_geom();\n}\n',
    'src/control.esk': 'int bounded(int v, int lo, int hi) {\n    if (v < lo) return lo;\n    if (v > hi) return hi;\n    return v;\n}\n',
    'src/world.esk': 'import "island.esk";\n\nint world_tick(int n) {\n    return island_step(n);\n}\n',
    'src/island.esk': 'import "atmos.esk";\n\nint island_step(int n) {\n    return atmos_level(n) + 1;\n}\n',
    'src/atmos.esk': '// atmos.esk: bounded() is control.esk\'s, which main.esk imports first.\nint atmos_level(int n) {\n    string label = "niebla ligera";\n    return bounded(n, 0, 10);\n}\n',
    'src/geom.esk': 'struct Vec {\n    int x;\n    int y;\n}\n\nint scale(int v, int k) {\n    return v * k;\n}\n\nint Vec_norm(*Vec self) {\n    return scale(self.x, 1) + self.y;\n}\n',
    'src/mathy.esk': 'import <path>;\n\nint absolute() {\n    return path_is_absolute("/x");\n}\n',
    'src/uses.esk': '// uses.esk: calls scale and v.norm() from geom.esk\nint use_geom() {\n    Vec v = Vec{x: 1, y: 2};\n    string s = "scale and norm";\n    int a = scale(v.x, 2);\n    return a + v.norm();\n}\n\nint shadow() {\n    int scale = 4;\n    int norm = 5;\n    return scale + norm;\n}\n',
};
for (const [f, t] of Object.entries(files)) fs.writeFileSync(path.join(dir, f), t);
const uri = (f) => 'file://' + path.join(dir, f);

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
const diagnostics = (u) => wait((m) => m.method === 'textDocument/publishDiagnostics' && m.params.uri === u);
const open = (f, text) => send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: uri(f), languageId: 'eskiu', version: 1, text: text == null ? files[f] : text } } });
const change = (f, text, version) => send({ jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri(f), version }, contentChanges: [{ text }] } });

let fails = 0;
const check = (what, ok, got) => { if (!ok) { fails++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

// The texts after applying a WorkspaceEdit's changes.
function apply(changes) {
    const out = {};
    for (const [u, edits] of Object.entries(changes || {})) {
        const f = path.relative(dir, decodeURIComponent(u.replace(/^file:\/\//, '')));
        const lines = files[f].split('\n');
        const sorted = [...edits].sort((a, b) => b.range.start.line - a.range.start.line || b.range.start.character - a.range.start.character);
        for (const e of sorted) {
            const l = e.range.start.line;
            lines[l] = lines[l].slice(0, e.range.start.character) + e.newText + lines[l].slice(e.range.end.character);
        }
        out[f] = lines.join('\n');
    }
    return out;
}

(async () => {
    const timer = setTimeout(() => { console.log('FAIL timeout'); process.exit(1); }, 60000);
    await request('initialize', { initializationOptions: { checkDelay: 100 } });

    // the indirect import: atmos.esk is checked in src/main.esk's program
    open('src/atmos.esk');
    const d1 = await diagnostics(uri('src/atmos.esk'));
    check('atmos.esk checks clean in its program', d1.params.diagnostics.length === 0, d1.params.diagnostics);

    // an unsaved error in it, after a non-ASCII string: on its line, at its UTF-16 column
    const bad = files['src/atmos.esk'].replace('string label = "niebla ligera";\n    return bounded(n, 0, 10);',
                                                'string label = "niebla ligera"; return boundd(n, 0, 10);\n    return 0;')
        .replace('niebla ligera', 'niebla ñ');
    change('src/atmos.esk', bad, 2);
    const d2 = await diagnostics(uri('src/atmos.esk'));
    const want = bad.split('\n')[2].indexOf('boundd');
    const e = d2.params.diagnostics[0];
    check('an unsaved error in an indirectly imported file', d2.params.diagnostics.length === 1 && e.range.start.line === 2 &&
          e.range.start.character === want && e.range.end.character === want + 6 && /did you mean 'bounded'/.test(e.message), d2.params.diagnostics);
    const fix = e ? await request('textDocument/codeAction', { textDocument: { uri: uri('src/atmos.esk') }, range: e.range, context: { diagnostics: [e] } }) : { result: [] };
    check('the quick fix', fix.result.length === 1 && fix.result[0].title === "Change to 'bounded'", fix.result);
    change('src/atmos.esk', files['src/atmos.esk'], 3);
    const d3 = await diagnostics(uri('src/atmos.esk'));
    check('clean again', d3.params.diagnostics.length === 0, d3.params.diagnostics);

    // hover and definition in an indirectly imported file reach the program
    const hov = await request('textDocument/hover', { textDocument: { uri: uri('src/atmos.esk') }, position: { line: 3, character: 12 } });
    check('hover on bounded()', hov.result && /int bounded\(int v, int lo, int hi\)/.test(hov.result.contents.value), hov.result);
    const def = await request('textDocument/definition', { textDocument: { uri: uri('src/atmos.esk') }, position: { line: 3, character: 12 } });
    check('definition of bounded() is in control.esk', def.result && def.result.uri === uri('src/control.esk') && def.result.range.start.line === 0, def.result);

    // references and rename of a function, from the declaring file and from a using file
    open('src/geom.esk');
    open('src/uses.esk');
    await diagnostics(uri('src/geom.esk'));
    await diagnostics(uri('src/uses.esk'));
    const refs = await request('textDocument/references', { textDocument: { uri: uri('src/geom.esk') }, position: { line: 5, character: 5 } });
    const where = (refs.result || []).map((r) => `${path.basename(r.uri)}:${r.range.start.line}:${r.range.start.character}`).sort().join(' ');
    check('references of scale across files', where === 'geom.esk:10:11 geom.esk:5:4 uses.esk:4:12', where);
    const wantScale = {
        'src/geom.esk': files['src/geom.esk'].replace('int scale(', 'int resize(').replace('return scale(', 'return resize('),
        'src/uses.esk': files['src/uses.esk'].replace('int a = scale(', 'int a = resize('),
    };
    for (const [f, at] of [['src/geom.esk', { line: 5, character: 6 }], ['src/uses.esk', { line: 4, character: 14 }]]) {
        const prep = await request('textDocument/prepareRename', { textDocument: { uri: uri(f) }, position: at });
        check(`prepareRename scale from ${f}`, prep.result && prep.result.end.character - prep.result.start.character === 5, prep);
        const ren = await request('textDocument/rename', { textDocument: { uri: uri(f) }, position: at, newName: 'resize' });
        const got = apply(ren.result && ren.result.changes);
        check(`rename scale from ${f}`, JSON.stringify(got) === JSON.stringify(wantScale), got);
    }

    // a dot-called method: Vec_norm and v.norm(), renamed from the call in another file
    const wantNorm = {
        'src/geom.esk': files['src/geom.esk'].replace('Vec_norm', 'Vec_length'),
        'src/uses.esk': files['src/uses.esk'].replace('a + v.norm()', 'a + v.length()'),
    };
    for (const [f, at] of [['src/uses.esk', { line: 5, character: 18 }], ['src/geom.esk', { line: 9, character: 9 }]]) {
        const ren = await request('textDocument/rename', { textDocument: { uri: uri(f) }, position: at, newName: 'length' });
        const got = apply(ren.result && ren.result.changes);
        const sorted = Object.fromEntries(Object.entries(got).sort());
        check(`rename the method norm from ${f}`, JSON.stringify(sorted) === JSON.stringify(wantNorm), got);
    }

    // a local that shadows the function renames only itself
    const loc = await request('textDocument/rename', { textDocument: { uri: uri('src/uses.esk') }, position: { line: 9, character: 9 }, newName: 'k' });
    const gotLocal = apply(loc.result && loc.result.changes);
    check('a same-named local is renamed alone', Object.keys(gotLocal).join() === 'src/uses.esk' &&
          gotLocal['src/uses.esk'] === files['src/uses.esk'].replace('int scale = 4;', 'int k = 4;').replace('return scale + norm', 'return k + norm'), gotLocal);

    // a symbol declared outside the program (the stdlib) is not renamed
    open('src/mathy.esk');
    await diagnostics(uri('src/mathy.esk'));
    const std = await request('textDocument/rename', { textDocument: { uri: uri('src/mathy.esk') }, position: { line: 3, character: 14 }, newName: 'root' });
    check('a stdlib function is not renamed', std.error && /outside the program/.test(std.error.message), std);
    const bad2 = await request('textDocument/rename', { textDocument: { uri: uri('src/uses.esk') }, position: { line: 1, character: 1 }, newName: 'x' });
    check('nothing to rename on a keyword', bad2.error && /No symbol/.test(bad2.error.message), bad2);

    // the outline of a part lists its own declarations only, on its own lines
    const syms = await request('textDocument/documentSymbol', { textDocument: { uri: uri('src/geom.esk') } });
    const names = (syms.result || []).map((s) => `${s.name}:${s.range.start.line}`).join(' ');
    check('document symbols of a part', names === 'Vec:0 scale:5 Vec_norm:9', names);

    // settings from workspace/didChangeConfiguration apply to every open document
    notes.length = 0;
    send({ jsonrpc: '2.0', method: 'workspace/didChangeConfiguration', params: { settings: { eskiu: { compilerFlags: ['-Wall'] } } } });
    const d4 = await diagnostics(uri('src/atmos.esk'));
    check('compilerFlags from didChangeConfiguration', d4.params.diagnostics.length === 1 && d4.params.diagnostics[0].severity === 2 &&
          /unused variable 'label'/.test(d4.params.diagnostics[0].message), d4.params.diagnostics);

    clearTimeout(timer);
    send({ jsonrpc: '2.0', id: next++, method: 'shutdown' });
    send({ jsonrpc: '2.0', method: 'exit' });
    fs.rmSync(dir, { recursive: true, force: true });
    console.log(fails ? `${fails} failed` : 'ok');
    process.exit(fails ? 1 : 0);
})();
