'use strict';
/**
 * flatten.js: the import graph of an Eskiu program, for the editors (project.js).
 *
 * `import "x.esk";` includes x textually where it stands, once per program, so a file sees
 * what the files before it declared even when it does not import them. A file is therefore
 * checked in the program it belongs to. This module answers which files a program reaches
 * (`reaches`), which file imports a given one (`rootOf` walks importers up to the top), and
 * writes a program as one text (`flatten`): from its root, every relative import replaced by
 * the file's lines, each line mapped back to the file and line it came from.
 *
 * `overrides` (a Map from absolute path to text) holds the editor's unsaved buffers: they are
 * read in place of the files on disk.
 */

const path = require('path');
const fs = require('fs');

const IMPORT_RE = /^\s*import\s+"([^"]+)"\s*;/;

function readText(file, overrides) {
    if (overrides && overrides.has(file)) return overrides.get(file);
    try { return fs.readFileSync(file, 'utf8'); } catch { return null; }
}

// The .esk files `file` imports with a relative `import "x.esk";`, in order.
function importsOf(file, overrides) {
    const text = readText(file, overrides);
    if (!text || !text.includes('import')) return [];
    const out = [];
    for (const line of text.split('\n')) {
        const m = line.match(IMPORT_RE);
        if (!m) continue;
        const target = path.resolve(path.dirname(file), m[1]);
        if (target.endsWith('.esk')) out.push(target);
    }
    return out;
}

// Whether `file` is part of the program `root`: reachable through relative imports, those
// inside an #if branch included (the editor does not know which branch a build takes). The
// walk stops after `limit` files.
function reaches(root, file, overrides, limit = 4000) {
    const seen = new Set([root]);
    const todo = [root];
    while (todo.length && seen.size <= limit) {
        const cur = todo.pop();
        if (cur === file) return true;
        for (const next of importsOf(cur, overrides)) {
            if (next === file) return true;
            if (!seen.has(next)) { seen.add(next); todo.push(next); }
        }
    }
    return false;
}

// The .esk files that could import `file`: those in its directory and the directories above it,
// up to the package's root (the directory with eskiu.toml) or four levels.
function candidates(file) {
    const out = [];
    let dir = path.dirname(file);
    const preferred = ['main.esk', 'core.esk', 'kernel.esk', 'app.esk', 'index.esk', 'root.esk', 'lib.esk'];
    for (let level = 0; level < 4; level++) {
        let names = [];
        try { names = fs.readdirSync(dir); } catch {}
        names = names.filter((n) => n.endsWith('.esk') && !n.startsWith('.'))
            .sort((a, b) => (preferred.indexOf(b) >= 0) - (preferred.indexOf(a) >= 0) || a.localeCompare(b));
        for (const n of names) out.push(path.join(dir, n));
        if (fs.existsSync(path.join(dir, 'eskiu.toml'))) break;
        const up = path.dirname(dir);
        if (up === dir) break;
        dir = up;
    }
    return out;
}

// The file that imports `file` with a relative import, if any.
function importerOf(file, overrides) {
    for (const c of candidates(file)) {
        if (c !== file && importsOf(c, overrides).includes(file)) return c;
    }
    return null;
}

// The root of the module `file` belongs to: the importer of its importer, and so on (the file
// itself when nothing imports it).
function rootOf(file, overrides) {
    const seen = new Set([file]);
    let cur = file;
    for (let i = 0; i < 16; i++) {
        const up = importerOf(cur, overrides);
        if (!up || seen.has(up)) break;
        seen.add(up);
        cur = up;
    }
    return cur;
}

// The program from `root` as one text, with `files[i]` and `lines[i]` (0-based) the origin of
// each of its lines.
function flatten(root, overrides) {
    const text = [];
    const files = [];
    const lines = [];
    // A file is included once, as the compiler imports it once; but one included inside an #if
    // branch may be needed again in another branch (the compiler drops the inactive one first),
    // so only an inclusion outside every conditional counts.
    const included = new Set();
    let cond = 0;
    function add(file, depth) {
        if (cond === 0) included.add(file);
        const src = readText(file, overrides);
        if (src == null) return;
        const ls = src.split('\n');
        for (let i = 0; i < ls.length; i++) {
            if (/^\s*#\s*if/.test(ls[i])) cond++;
            else if (/^\s*#\s*endif/.test(ls[i])) cond = Math.max(0, cond - 1);
            const m = depth < 32 && ls[i].match(IMPORT_RE);
            const target = m && path.resolve(path.dirname(file), m[1]);
            if (target && target.endsWith('.esk') && (overrides && overrides.has(target) || fs.existsSync(target))) {
                // a file already in the program is not included again (imports happen once)
                if (!included.has(target)) add(target, depth + 1);
                text.push('');
                files.push(file);
                lines.push(i);
                continue;
            }
            text.push(ls[i]);
            files.push(file);
            lines.push(i);
        }
    }
    add(root, 0);
    return {
        text: text.join('\n'),
        files,
        lines,
        // the files the text holds
        members: new Set(files),
        // 0-based line of `file` to the 1-based line of the text (0 when it is not there)
        toFlat(file, line) {
            for (let i = 0; i < files.length; i++) if (files[i] === file && lines[i] === line) return i + 1;
            return 0;
        },
        // 1-based line of the text to { file, line (0-based) }
        toOrig(flatLine) {
            const i = flatLine - 1;
            return i >= 0 && i < files.length ? { file: files[i], line: lines[i] } : null;
        },
    };
}

module.exports = { IMPORT_RE, readText, importsOf, reaches, rootOf, flatten };
