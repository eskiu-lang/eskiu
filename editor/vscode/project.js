'use strict';
/**
 * project.js: how both editors (extension.js for VS Code, server.js for LSP clients) run the
 * compiler on a document. It holds no editor API, so the two cannot drift apart.
 *
 * A file is checked in the program it belongs to (`findProgram`):
 *   1. the `entryPoint` setting, when the file is that file or one it reaches;
 *   2. else the `main` of the nearest eskiu.toml above the file (`src/main.esk` when the
 *      manifest names none, as eskiuc assumes), when it reaches the file;
 *   3. else the top of the file's importer chain (flatten.js rootOf);
 *   4. else the file itself.
 * "Reaches" means through relative `import "x.esk";` lines, a bounded walk.
 *
 * Diagnostics (`check`) run `eskiuc ROOT --test-typechecker` with `--overlay=PATH=TMP` for each
 * open buffer that differs from its file, so the compiler reads the unsaved text where the file
 * is imported and reports real paths and exact columns. Only the document's own diagnostics are
 * kept. The editor modes that take a LINE:COL (hover, definition, references, completion,
 * symbols) read it in the root file, so `query` runs them on the program flattened into one
 * text (flatten.js), given as the overlay of the root, and maps lines back.
 *
 * Columns: the compiler counts bytes, editors count UTF-16 units; both are converted here.
 */

const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { readText, reaches, rootOf, flatten } = require('./flatten');

// ── Settings ─────────────────────────────────────────────────────────────────

// The settings both editors share, from a VS Code configuration or LSP initializationOptions /
// workspace/didChangeConfiguration (`raw` may be partial; missing keys keep `base`).
const DEFAULTS = { compilerPath: '', compilerFlags: [], entryPoint: '', checkOnType: true, checkDelay: 400 };

function normalizeSettings(raw, base = DEFAULTS) {
    const s = { ...base };
    if (!raw || typeof raw !== 'object') return s;
    if (typeof raw.compilerPath === 'string') s.compilerPath = raw.compilerPath.trim();
    if (Array.isArray(raw.compilerFlags)) s.compilerFlags = raw.compilerFlags.filter((f) => typeof f === 'string' && f);
    else if (typeof raw.compilerFlags === 'string') s.compilerFlags = raw.compilerFlags.trim().split(/\s+/).filter(Boolean);
    if (typeof raw.entryPoint === 'string') s.entryPoint = raw.entryPoint.trim();
    if (typeof raw.checkOnType === 'boolean') s.checkOnType = raw.checkOnType;
    else if (raw.checkOnType === 'false' || raw.checkOnType === '0') s.checkOnType = false;
    else if (raw.checkOnType === 'true' || raw.checkOnType === '1') s.checkOnType = true;
    const delay = Number(raw.checkDelay);
    if (raw.checkDelay != null && raw.checkDelay !== '' && Number.isFinite(delay)) s.checkDelay = Math.max(100, delay);
    return s;
}

// ── The compiler ─────────────────────────────────────────────────────────────

const BUILD_NAMES = ['build22', 'build', 'build-debug', 'build-release'];

function executable(p) {
    try { fs.accessSync(p, fs.constants.X_OK); return fs.statSync(p).isFile(); } catch { return false; }
}

// The compiler: the compilerPath setting, else $ESKIUC, else (only in a trusted workspace)
// build22/build/... in a workspace folder or a directory above `hint`, else a build next to
// this checkout, else the installer's ~/.eskiu/bin/eskiuc, else `eskiuc` on PATH. An untrusted
// workspace never picks the program run: its build directories are not searched.
function findCompiler(env, hint) {
    const s = env.settings;
    if (s.compilerPath) return s.compilerPath;
    const fromEnv = process.env.ESKIUC;
    if (fromEnv && executable(fromEnv)) return /[\\/]/.test(fromEnv) ? path.resolve(fromEnv) : fromEnv;
    if (env.trusted) {
        for (const wf of env.folders || []) {
            for (const b of BUILD_NAMES) if (executable(path.join(wf, b, 'eskiuc'))) return path.join(wf, b, 'eskiuc');
        }
        if (hint) {
            let cur = path.dirname(path.resolve(hint));
            for (let i = 0; i < 6; i++) {
                for (const b of BUILD_NAMES) if (executable(path.join(cur, b, 'eskiuc'))) return path.join(cur, b, 'eskiuc');
                const up = path.dirname(cur);
                if (up === cur) break;
                cur = up;
            }
        }
    }
    for (const up of [['..', '..'], ['..', '..', '..']]) {
        for (const b of ['build22', 'build']) {
            const c = path.join(__dirname, ...up, b, 'eskiuc');
            if (executable(c)) return c;
        }
    }
    const installed = path.join(os.homedir(), '.eskiu', 'bin', 'eskiuc');
    if (executable(installed)) return installed;
    return 'eskiuc';
}

// Run the compiler: { ok, stdout, stderr }. A compiler that cannot be started is reported once
// per change of settings through env.onMissing.
function runCompiler(env, args, opts = {}) {
    const compiler = findCompiler(env, opts.hint);
    return new Promise((resolve) => {
        execFile(compiler, args, { timeout: opts.timeout || 10000, cwd: opts.cwd, maxBuffer: 64 << 20 },
            (err, stdout, stderr) => {
                if (err && err.code === 'ENOENT') {
                    if (env.onMissing) env.onMissing(compiler);
                    resolve({ ok: false, missing: true, stdout: '', stderr: '' });
                    return;
                }
                resolve({ ok: !err, stdout: stdout || '', stderr: stderr || '', compiler });
            });
    });
}

// ── The program a file belongs to ────────────────────────────────────────────

// The `main` of an eskiu.toml's [package] table (src/main.esk when it names none).
function manifestMain(text) {
    let table = '';
    for (const raw of text.split('\n')) {
        const line = raw.replace(/#.*$/, '').trim();
        const t = line.match(/^\[([^\]]*)\]$/);
        if (t) { table = t[1].trim(); continue; }
        const m = table === 'package' && line.match(/^main\s*=\s*"([^"]*)"$/);
        if (m) return m[1];
    }
    return 'src/main.esk';
}

function exists(p, buffers) {
    return (buffers && buffers.has(p)) || fs.existsSync(p);
}

// { root, reason }: the file the compiler is run on for `file` (see the header).
function findProgram(file, env) {
    file = path.resolve(file);
    const buffers = env.buffers;
    const fits = (root) => root === file || reaches(root, file, buffers);
    const entry = (env.settings.entryPoint || '').trim();
    if (entry) {
        const bases = path.isAbsolute(entry) ? [''] : [...(env.folders || []), path.dirname(file)];
        for (const b of bases) {
            const p = path.resolve(b, entry);
            if (exists(p, buffers) && fits(p)) return { root: p, reason: 'entryPoint' };
        }
    }
    for (let dir = path.dirname(file), i = 0; i < 32; i++) {
        const toml = path.join(dir, 'eskiu.toml');
        if (fs.existsSync(toml)) {
            let text = '';
            try { text = fs.readFileSync(toml, 'utf8'); } catch {}
            const main = path.resolve(dir, manifestMain(text));
            if (exists(main, buffers) && fits(main)) return { root: main, reason: 'package' };
            break;
        }
        const up = path.dirname(dir);
        if (up === dir) break;
        dir = up;
    }
    const top = rootOf(file, buffers);
    if (top !== file) return { root: top, reason: 'importer' };
    return { root: file, reason: 'file' };
}

// ── Running on the current text ──────────────────────────────────────────────

// Compilers older than --overlay (0.16.3 and before), by path.
const noOverlay = new Set();

// Run `eskiuc ROOT ...args` with `overlays` (path → text read instead of the file).
async function runOn(env, root, args, overlays, opts = {}) {
    const cwd = path.dirname(root);
    const flags = env.settings.compilerFlags || [];
    const compiler = findCompiler(env, root);
    let dir = null;
    const extra = [];
    let target = root;
    let sibling = null;
    if (overlays.size && !noOverlay.has(compiler)) {
        dir = fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-edit-'));
        let i = 0;
        for (const [p, text] of overlays) {
            const tmp = path.join(dir, `${i++}-${path.basename(p)}`);
            fs.writeFileSync(tmp, text);
            extra.push(`--overlay=${p}=${tmp}`);
        }
    } else if (overlays.has(root)) {
        // A compiler without --overlay: the root's text goes in a hidden copy next to it (the
        // other buffers are read from disk).
        sibling = path.join(cwd, `.${path.basename(root)}.${process.pid}.${Date.now()}.eskiu-check.esk`);
        try { fs.writeFileSync(sibling, overlays.get(root)); target = sibling; } catch { sibling = null; }
    }
    try {
        const r = await runCompiler(env, [target, ...extra, ...flags, ...args], { cwd, timeout: opts.timeout, hint: root });
        if (extra.length && /--overlay/.test(r.stderr) && /[Uu]nknown command line argument/.test(r.stderr)) {
            noOverlay.add(compiler);
            if (env.onOutdated) env.onOutdated(compiler);
            return runOn(env, root, args, overlays, opts);
        }
        if (sibling) {
            const swap = (t) => t.split(sibling).join(root).split(path.basename(sibling)).join(path.basename(root));
            r.stdout = swap(r.stdout);
            r.stderr = swap(r.stderr);
        }
        return r;
    } finally {
        if (dir) fs.rm(dir, { recursive: true, force: true }, () => {});
        if (sibling) fs.rm(sibling, { force: true }, () => {});
    }
}

// The open buffers whose text differs from their file.
function dirtyBuffers(buffers) {
    const out = new Map();
    for (const [p, text] of buffers || []) {
        let disk = null;
        try { disk = fs.readFileSync(p, 'utf8'); } catch {}
        if (text !== disk) out.set(p, text);
    }
    return out;
}

const canonCache = new Map();
function canon(p) {
    p = path.resolve(p);
    if (canonCache.has(p)) return canonCache.get(p);
    let c = p;
    try { c = fs.realpathSync.native(p); } catch {}
    if (canonCache.size > 4096) canonCache.clear();
    canonCache.set(p, c);
    return c;
}

// UTF-16 column (0-based) of 1-based byte column `byteCol` in `line`, and back.
function toChar(line, byteCol) {
    if (line == null) return Math.max(0, byteCol - 1);
    const b = Buffer.from(line, 'utf8');
    if (byteCol - 1 >= b.length) return line.length + (byteCol - 1 - b.length);
    return b.subarray(0, Math.max(0, byteCol - 1)).toString('utf8').length;
}
function toByte(line, ch) {
    return line == null ? ch + 1 : Buffer.byteLength(line.slice(0, ch), 'utf8') + 1;
}

// Line `n` (0-based) of `file` as the editor shows it.
function lineReader(buffers) {
    const cache = new Map();
    return (file, n) => {
        if (!cache.has(file)) {
            const t = readText(file, buffers);
            cache.set(file, t == null ? null : t.split('\n'));
        }
        const ls = cache.get(file);
        return ls && n < ls.length ? ls[n].replace(/\r$/, '') : null;
    };
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

// The diagnostics of `file` checked in its program: [{ line, start, end, severity ('error',
// 'warning' or 'info'), message }], 0-based, UTF-16 columns; null when the compiler did not run.
async function check(file, env) {
    file = path.resolve(file);
    const { root } = findProgram(file, env);
    const args = ['--diagnostics=json', '--test-typechecker'];
    const dirty = dirtyBuffers(env.buffers);
    const cwd = path.dirname(root);
    let r = await runOn(env, root, args, dirty, { timeout: 20000 });
    if (r.missing) return null;
    // A compiler without --overlay checks the program flattened into the root (as the editor
    // modes do), its lines mapped back.
    let lineMap = (f, line1) => ({ file: path.resolve(cwd, f), line: line1 - 1 });
    if (dirty.size && noOverlay.has(r.compiler)) {
        const flat = flatten(root, env.buffers);
        r = await runOn(env, root, args, new Map([[root, flat.text]]), { timeout: 20000 });
        const rootCanon = canon(root);
        lineMap = (f, line1) => (canon(path.resolve(cwd, f)) === rootCanon ? flat.toOrig(line1)
                                                                           : { file: path.resolve(cwd, f), line: line1 - 1 });
    }
    const me = canon(file);
    const lineOf = lineReader(env.buffers);
    const out = [];
    let elsewhere = null;
    for (const row of r.stderr.split('\n')) {
        if (!row.startsWith('{')) continue;
        let d;
        try { d = JSON.parse(row); } catch { continue; }
        if (d.severity !== 'error' && d.severity !== 'warning') continue;
        let message = d.message + (d.hints || []).map((h) => ` (${h})`).join('') + (d.option ? ` [${d.option}]` : '');
        for (const n of d.notes || []) message += `\n${n.message}`;
        if (!d.file || !d.line) continue;
        const at = lineMap(d.file, d.line);
        if (!at) continue;
        if (canon(at.file) !== me) {
            if (!elsewhere && d.severity === 'error') elsewhere = { where: at.file, line: at.line + 1, col: d.col, message };
            continue;
        }
        const ln = at.line;
        const text = lineOf(file, ln);
        const start = toChar(text, d.col || 1);
        let end;
        if (d.end_line === d.line && d.end_col) end = toChar(text, d.end_col);
        else {
            end = start + 1;
            if (text) while (end < text.length && /[A-Za-z0-9_]/.test(text[end])) end++;
        }
        out.push({ line: ln, start, end: Math.max(end, start + 1), severity: d.severity, message });
    }
    // The program stopped before checking this file (a syntax error in another file): say so,
    // instead of showing a clean file.
    if (!out.length && elsewhere && !/Type checking/.test(r.stdout)) {
        const { where, line, col, message } = elsewhere;
        out.push({ line: 0, start: 0, end: 0, severity: 'info',
                   message: `Not checked: the program stops at ${path.relative(cwd, where) || where}:${line}:${col}: ${message}` });
    }
    return out;
}

// ── Editor modes at a position ───────────────────────────────────────────────

// Run an editor mode (`--hover-at`, `--references-at`, ...) on `file`'s program. `pos` (0-based
// line, UTF-16 character) goes to `flag` as LINE:COL. Returns { out, loc, members, root }:
// loc(fileFromOutput, line1, byteCol) maps a location in the output to { file, line, character }
// (0-based) or null, and members is the set of files the flattened program holds.
async function query(file, env, args, flag = null, pos = null, timeout = 8000) {
    file = path.resolve(file);
    const { root } = findProgram(file, env);
    const flat = flatten(root, env.buffers);
    const lineOf = lineReader(env.buffers);
    const extra = [];
    if (flag) {
        const at = flat.toFlat(file, pos.line);
        if (!at) return { out: '', loc: () => null, members: flat.members, root };
        extra.push(flag, `${at}:${toByte(lineOf(file, pos.line), pos.character)}`);
    }
    const r = await runOn(env, root, [...args, ...extra], new Map([[root, flat.text]]), { timeout });
    const cwd = path.dirname(root);
    const rootCanon = canon(root);
    const loc = (f, line1, byteCol) => {
        const p = path.resolve(cwd, f);
        let where;
        if (canon(p) === rootCanon) where = flat.toOrig(line1);
        else where = { file: p, line: line1 - 1 };
        if (!where) return null;
        return { file: where.file, line: where.line, character: toChar(lineOf(where.file, where.line), byteCol) };
    };
    return { out: r.stdout, loc, members: flat.members, root };
}

// --references-at as [{ file, line, start, end }] (0-based, UTF-16), without duplicates.
async function references(file, env, pos) {
    const { out, loc, members } = await query(file, env, [], '--references-at', pos, 8000);
    const seen = new Set();
    const refs = [];
    for (const row of out.split('\n')) {
        const m = row.trim().match(/^(.+):(\d+):(\d+):(\d+)$/);
        if (!m) continue;
        const col = parseInt(m[3], 10);
        const a = loc(m[1], parseInt(m[2], 10), col);
        const b = loc(m[1], parseInt(m[2], 10), col + parseInt(m[4], 10));
        if (!a || !b) continue;
        const key = `${a.file}:${a.line}:${a.character}`;
        if (seen.has(key)) continue;
        seen.add(key);
        refs.push({ file: a.file, line: a.line, start: a.character, end: b.character });
    }
    return { refs, members };
}

// --definition-at as { file, line, character } or null.
async function definition(file, env, pos) {
    const { out, loc } = await query(file, env, [], '--definition-at', pos, 8000);
    const m = out.trim().match(/^(.+):(\d+):(\d+)$/m);
    return m ? loc(m[1], parseInt(m[2], 10), parseInt(m[3], 10)) : null;
}

// --symbols of `file` (declared in it, in its program): [{ kind, name, line, character,
// container, detail }].
async function symbols(file, env) {
    file = path.resolve(file);
    const { out, loc, root } = await query(file, env, ['--symbols']);
    const rows = [];
    for (const row of out.split('\n')) {
        const [kind, name, at, container, detail] = row.split('\t');
        if (!name || !at) continue;
        const [l, c] = at.split(':').map((n) => parseInt(n, 10));
        // the positions are in the root, the flattened text
        const w = loc(root, l, c);
        if (!w || w.file !== file) continue;
        rows.push({ kind, name, line: w.line, character: w.character, container: container || '', detail: detail || '' });
    }
    return rows;
}

// `eskiuc fmt` rewrites a file in place: format a temporary copy. The formatted text, or null
// when the compiler failed (a syntax error).
async function format(text, env, hint) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'eskiu-fmt-'));
    const tmp = path.join(dir, 'format.esk');
    try {
        fs.writeFileSync(tmp, text);
        const r = await runCompiler(env, ['fmt', tmp], { timeout: 10000, hint });
        if (!r.ok) return null;
        return fs.readFileSync(tmp, 'utf8');
    } catch {
        return null;
    } finally {
        fs.rm(dir, { recursive: true, force: true }, () => {});
    }
}

module.exports = {
    DEFAULTS, normalizeSettings, findCompiler, runCompiler, manifestMain, findProgram,
    check, query, references, definition, symbols, format, toChar, toByte,
};
