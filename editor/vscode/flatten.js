'use strict';
/**
 * flatten.js: an Eskiu file that is part of a module (imported with `import "part.esk";`) is
 * checked in its module, not alone. A part sees what the module declares and imports before it,
 * which the part itself does not import, so checked alone it reports unknown types and names.
 *
 * `import "x.esk";` includes x textually where it stands (the module is one program, imported
 * once), so the module can be checked as one file: from its root, every relative import
 * replaced by the file's lines, the open document's unsaved text in place of its file. The
 * compiler runs on that file, and each line maps back to the file and line it came from.
 */

const path = require('path');
const fs = require('fs');

const IMPORT_RE = /^\s*import\s+"([^"]+)"\s*;/;

function readText(file, overrides) {
    if (overrides && overrides.has(file)) return overrides.get(file);
    try { return fs.readFileSync(file, 'utf8'); } catch { return null; }
}

// The .esk files that could import `file`: those in its directory and the directories above it,
// up to the package's root (the directory with eskiu.toml) or four levels.
function candidates(file) {
    const out = [];
    let dir = path.dirname(file);
    for (let level = 0; level < 4; level++) {
        let names = [];
        try { names = fs.readdirSync(dir); } catch {}
        const preferred = ['main.esk', 'core.esk', 'kernel.esk', 'app.esk', 'index.esk', 'root.esk', 'lib.esk'];
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
function importerOf(file) {
    for (const c of candidates(file)) {
        if (c === file) continue;
        const text = readText(c);
        if (!text || !text.includes('import')) continue;
        for (const line of text.split('\n')) {
            const m = line.match(IMPORT_RE);
            if (m && path.resolve(path.dirname(c), m[1]) === file) return c;
        }
    }
    return null;
}

// The root of the module `file` belongs to: the importer of its importer, and so on (the file
// itself when nothing imports it).
function rootOf(file) {
    const seen = new Set([file]);
    let cur = file;
    for (let i = 0; i < 16; i++) {
        const up = importerOf(cur);
        if (!up || seen.has(up)) break;
        seen.add(up);
        cur = up;
    }
    return cur;
}

// The module from `root` as one text, with `files[i]` and `lines[i]` (0-based) the origin of
// each of its lines. `overrides` maps a path to the text to use instead of the file's.
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
            if (target && target.endsWith('.esk') && fs.existsSync(target)) {
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
    return { text: text.join('\n'), files, lines };
}

// The flattened module `file` belongs to, written next to its root so its own imports resolve,
// or null when the file is not part of a module. `text` is the open document's text.
function flatFor(file, text) {
    const root = rootOf(file);
    if (root === file) return null;
    const overrides = new Map();
    if (text != null) overrides.set(file, text);
    const flat = flatten(root, overrides);
    const out = path.join(path.dirname(root), `.eskiu_lsp_flat_${process.pid}_${Date.now()}.esk`);
    fs.writeFileSync(out, flat.text);
    return {
        path: out,
        root,
        // 0-based line in `file` to 1-based line in the flat file (0 when not found)
        toFlat(origFile, line) {
            for (let i = 0; i < flat.files.length; i++) {
                if (flat.files[i] === origFile && flat.lines[i] === line) return i + 1;
            }
            return 0;
        },
        // 1-based line in the flat file to { file, line (0-based) }
        toOrig(flatLine) {
            const i = flatLine - 1;
            if (i < 0 || i >= flat.files.length) return null;
            return { file: flat.files[i], line: flat.lines[i] };
        },
        remove() { try { fs.rmSync(out, { force: true }); } catch {} },
    };
}

module.exports = { rootOf, flatten, flatFor };
