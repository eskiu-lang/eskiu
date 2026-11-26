// The editors' shared modules, without an editor: editor/vscode/project.js (the program a file
// belongs to, settings, columns, checks with unsaved buffers) and features.js.
//     node tests/editor/project_test.js    (ESKIUC picks the compiler; prints ok)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const project = require('../../editor/vscode/project');
const features = require('../../editor/vscode/features');

let fails = 0;
const check = (what, ok, got) => { if (!ok) { fails++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-project-')));
const write = (f, t) => { fs.mkdirSync(path.dirname(path.join(dir, f)), { recursive: true }); fs.writeFileSync(path.join(dir, f), t); };
const at = (f) => path.join(dir, f);
// pkg: a package without `main` (src/main.esk); src/b.esk is reached through src/a.esk and has a
// sibling src/c.esk that imports it too; src/lone.esk is in no program; tools/ is not a package.
write('pkg/eskiu.toml', '[package]\nname = "pkg"\n\n[deps]\n');
write('pkg/src/main.esk', 'import "a.esk";\nint main() { return b(); }\n');
write('pkg/src/a.esk', 'import "b.esk";\n');
write('pkg/src/b.esk', 'int b() { return 1; }\n');
write('pkg/src/c.esk', 'import "b.esk";\n');
write('pkg/src/lone.esk', 'int lone() { return 2; }\n');
write('pkg/src/other.esk', 'import "lone.esk";\n');
write('alt/eskiu.toml', '# a comment\n[package]\nname = "alt"\nmain = "app/start.esk" # the entry\n[deps]\nmain = "not this"\n');
write('alt/app/start.esk', 'import "../lib/x.esk";\nint main() { return 0; }\n');
write('alt/lib/x.esk', 'int x() { return 0; }\n');
write('tools/run.esk', 'import "helper.esk";\nint main() { return h(); }\n');
write('tools/helper.esk', 'int h() { return 0; }\n');

const env = (settings = {}, buffers = new Map()) => ({
    settings: project.normalizeSettings(settings), buffers, folders: [dir], trusted: false,
    compilerPath: undefined,
});
const program = (f, s, b) => { const p = project.findProgram(at(f), env(s, b)); return `${path.relative(dir, p.root)} ${p.reason}`; };

check('manifest main default', project.manifestMain('[package]\nname = "x"\n') === 'src/main.esk');
check('manifest main', project.manifestMain(fs.readFileSync(at('alt/eskiu.toml'), 'utf8')) === 'app/start.esk');
check('package main reaches an indirect import', program('pkg/src/b.esk') === 'pkg/src/main.esk package', program('pkg/src/b.esk'));
check('the package main itself', program('pkg/src/main.esk') === 'pkg/src/main.esk package', program('pkg/src/main.esk'));
check('a manifest main', program('alt/lib/x.esk') === 'alt/app/start.esk package', program('alt/lib/x.esk'));
check('not reached from main: its importer', program('pkg/src/lone.esk') === 'pkg/src/other.esk importer', program('pkg/src/lone.esk'));
check('no package: its importer', program('tools/helper.esk') === 'tools/run.esk importer', program('tools/helper.esk'));
check('a file alone', program('tools/run.esk') === 'tools/run.esk file', program('tools/run.esk'));
check('entryPoint (relative to a folder)', program('pkg/src/lone.esk', { entryPoint: 'pkg/src/other.esk' }) === 'pkg/src/other.esk entryPoint');
check('entryPoint that does not reach the file is skipped',
      program('pkg/src/b.esk', { entryPoint: 'tools/run.esk' }) === 'pkg/src/main.esk package', program('pkg/src/b.esk', { entryPoint: 'tools/run.esk' }));
const unsaved = new Map([[at('pkg/src/main.esk'), 'import "a.esk";\nimport "lone.esk";\nint main() { return b(); }\n']]);
check('an unsaved import counts', program('pkg/src/lone.esk', {}, unsaved) === 'pkg/src/main.esk package', program('pkg/src/lone.esk', {}, unsaved));

const s = project.normalizeSettings({ compilerFlags: ' -Wall  -Wextra ', checkOnType: 'false', checkDelay: 20 });
check('settings', s.compilerFlags.join() === '-Wall,-Wextra' && s.checkOnType === false && s.checkDelay === 100, s);
check('settings keep the base', project.normalizeSettings({ entryPoint: 'a.esk' }, s).compilerFlags.length === 2);

check('UTF-16 from bytes', project.toChar('s = "ñu"; x', 12) === 10, project.toChar('s = "ñu"; x', 12));
check('bytes from UTF-16', project.toByte('s = "ñu"; x', 10) === 12, project.toByte('s = "ñu"; x', 10));

const text = 'int f() {\n    return g(1, h(2), ';
const call = features.enclosingCall(text, text.length);
check('enclosing call', call && call.active === 2 && !call.dot && text.slice(call.offset, call.offset + 1) === 'g', call);
const sig = features.signature('int P_m(*P self, int a, int b)\n', { active: 1, dot: true });
check('dot-call signature', sig && sig.params.join('|') === 'int a|int b' && sig.active === 1, sig);
check('no completion in a string', features.completionContext('x = "ab') === null);
check('member completion', features.completionContext('    p.').member === true);
const h = features.hover('int String_len(String* self)\n');
check('hover note for a method', h && /call it as `x.len/.test(h.note), h);
const plan = features.renamePlan([{ file: '/a', line: 0, start: 4, end: 10 }, { file: '/b', line: 2, start: 2, end: 6 }],
                                 new Set(['/a', '/b']), '/b', { line: 2, character: 3 });
check('rename keeps a method prefix', plan.edits && plan.edits[0].start === 6 && plan.here.start === 2, plan);
check('rename outside the program', /outside/.test(features.renamePlan([{ file: '/std', line: 0, start: 0, end: 3 },
    { file: '/a', line: 0, start: 0, end: 3 }], new Set(['/a']), '/a', { line: 0, character: 1 }).error));

(async () => {
    // a check of an indirectly imported file with an unsaved buffer, against the compiler
    const e = env({}, new Map([[at('pkg/src/b.esk'), 'int b() { return "s"; }\n']]));
    const found = await project.check(at('pkg/src/b.esk'), e);
    check('check: the unsaved error of an indirect import', found && found.length === 1 && found[0].line === 0 && found[0].start === 10, found);
    const clean = await project.check(at('pkg/src/b.esk'), env({}));
    check('check: clean on disk', clean && clean.length === 0, clean);
    // the program stops in another file: one note, not a clean file
    const broken = new Map([[at('pkg/src/main.esk'), 'import "a.esk";\nint main( { return b(); }\n']]);
    const note = await project.check(at('pkg/src/b.esk'), env({}, broken));
    check('check: a syntax error elsewhere is said', note && note.length === 1 && note[0].severity === 'info' && /main\.esk:2/.test(note[0].message), note);
    fs.rmSync(dir, { recursive: true, force: true });
    console.log(fails ? `${fails} failed` : 'ok');
    process.exit(fails ? 1 : 0);
})();
