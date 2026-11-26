'use strict';
/**
 * features.js: what both editors (extension.js, server.js) make of the compiler's output, as
 * plain data each editor turns into its own types: hover text, completion items, signature
 * help, the outline, the "did you mean" quick fix and rename edits. Positions are 0-based
 * lines and UTF-16 characters.
 */

const path = require('path');

const KEYWORDS = [
    'if', 'else', 'for', 'in', 'while', 'do', 'switch', 'case', 'default', 'break', 'continue',
    'return', 'match', 'struct', 'union', 'enum', 'interface', 'type', 'let', 'const', 'static',
    'volatile', 'extern', 'intrinsic', 'import', 'async', 'await', 'try', 'catch', 'finally',
    'throw', 'defer', 'errdefer', 'sizeof', 'null', 'true', 'false', 'fn', 'operator', 'packed',
    'escaping', 'must_use', 'alloc_with', 'free_closure', 'thread_create', 'thread_join',
    'int', 'int8', 'int16', 'int32', 'int64', 'uint', 'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'bool', 'char', 'string', 'void',
];

const lastLine = (out) => (out.trim().split('\n').pop() || '').trim();

// --hover-at's answer as { code, note } (note: how to call a `Type_method` with dot syntax),
// or null.
function hover(out) {
    const code = lastLine(out);
    if (!code || code.startsWith('(') || code.startsWith('error:')) return null;
    const m = code.match(/^\S.*?\b([A-Z][A-Za-z0-9]*)_([a-z_][A-Za-z0-9_]*)(<[^(]*>)?\(\s*(const\s+)?\*?\1\b[^,)]*\bself\b/);
    return { code, note: m ? `Method of \`${m[1]}\`: call it as \`x.${m[2]}(...)\`.` : '' };
}

function hoverMarkdown(h) {
    return '```eskiu\n' + h.code + '\n```' + (h.note ? '\n\n' + h.note : '');
}

// Whether to complete after `before` (the line up to the cursor): null in a comment, a
// directive or a string; else { member } (after `recv.`).
function completionContext(before) {
    if (/^\s*(\/\/|#)/.test(before) || before.split('"').length % 2 === 0) return null;
    return { member: /\.\s*[A-Za-z_0-9]*$/.test(before) };
}

const COMPLETION_KINDS = ['field', 'method', 'function', 'variable', 'global', 'struct', 'interface',
                          'enum', 'enummember', 'type'];

// --complete-at's rows as [{ label, kind, detail, sortText }]; kind is a COMPLETION_KINDS name
// or 'keyword' (the keywords are offered except after `recv.`).
function completionItems(out, member) {
    const items = [];
    for (const row of out.split('\n')) {
        const [kind, name, detail] = row.split('\t');
        if (!name || !COMPLETION_KINDS.includes(kind)) continue;
        // locals first, then members, then the rest of the program
        const rank = kind === 'variable' ? '0' : kind === 'field' ? '1' : kind === 'method' ? '2' : '5';
        items.push({ label: name, kind, detail: detail || '', sortText: rank + name });
    }
    if (!member) for (const k of KEYWORDS) items.push({ label: k, kind: 'keyword', detail: '', sortText: '9' + k });
    return items;
}

// The call around offset `off` of `text`: the offset of the callee's name, the index of the
// argument being written and whether it is a dot-call (scanning back over balanced brackets
// and strings, at most 4000 characters).
function enclosingCall(text, off) {
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
            return { offset: k, active: commas, dot: text[k - 1] === '.' };
        } else if (c === ',' && depth === 0) commas++;
        else if (c === ';' && depth === 0) return null;
    }
    return null;
}

// Offset of a 0-based { line, character } in `text`, and back.
function offsetAt(text, pos) {
    let off = 0;
    const lines = text.split('\n');
    for (let i = 0; i < pos.line && i < lines.length; i++) off += lines[i].length + 1;
    return off + pos.character;
}
function positionAt(text, off) {
    const before = text.slice(0, off).split('\n');
    return { line: before.length - 1, character: before[before.length - 1].length };
}

// The callee's --hover-at signature as { label, params, active } for the call `call`, or null.
function signature(out, call) {
    const sig = lastLine(out);
    const open = sig.indexOf('(');
    if (open < 0 || !sig.endsWith(')') || sig.startsWith('fn(')) return null;
    // split the parameter list at top-level commas (a `fn(A,B)->C` parameter has its own)
    const params = [];
    let depth = 0, cur = '';
    for (const c of sig.slice(open + 1, -1)) {
        if ('(<['.includes(c)) depth++;
        if (')>]'.includes(c)) depth--;
        if (c === ',' && depth === 0) { params.push(cur.trim()); cur = ''; } else cur += c;
    }
    if (cur.trim()) params.push(cur.trim());
    // a dot-call passes the receiver as `self`: the first argument written is the second parameter
    const shown = call.dot && params.length && /\bself$/.test(params[0]) ? params.slice(1) : params;
    const variadic = shown.length && shown[shown.length - 1] === '...';
    return { label: sig, params: shown, active: variadic ? Math.min(call.active, shown.length - 1) : call.active };
}

// The outline from symbols rows (project.js symbols): [{ name, detail, kind, line, character,
// endCharacter, children }], fields and inline methods under their struct. `lineLength(n)` is
// the length of line n.
function symbolTree(rows, lineLength) {
    const top = [];
    const structs = new Map();
    for (const r of rows) {
        const sym = { name: r.name, detail: r.detail, kind: r.kind, line: r.line, character: r.character,
                      endLine: r.line, endCharacter: Math.max(r.character + r.name.length, lineLength(r.line)),
                      children: [] };
        const parent = r.container && structs.get(r.container);
        if (parent) {
            parent.children.push(sym);
            if (parent.endLine < sym.endLine) { parent.endLine = sym.endLine; parent.endCharacter = sym.endCharacter; }
        } else {
            top.push(sym);
            if (r.kind === 'struct') structs.set(r.name, sym);
        }
    }
    return top;
}

// "undefined variable 'totl' (did you mean 'total'?)": replace the name on the diagnostic's
// line nearest to its column. `diagnostics` are [{ line, start, message, source }];
// returns [{ title, line, start, end, newText, diagnostic }].
const MISSPELLED = /(?:variable|function|method|member|type) '([^']+)'[^]*\(did you mean '([^']+)'\?\)/;

function quickFixes(lineText, diagnostics) {
    const fixes = [];
    for (const d of diagnostics) {
        if (d.source && d.source !== 'eskiuc') continue;
        const m = (d.message || '').match(MISSPELLED);
        if (!m) continue;
        const [, wrong, right] = m;
        const text = lineText(d.line) || '';
        const re = new RegExp(`\\b${wrong.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`, 'g');
        let best = -1, hit;
        while ((hit = re.exec(text)) !== null) {
            if (best < 0 || Math.abs(hit.index - d.start) < Math.abs(best - d.start)) best = hit.index;
        }
        if (best < 0) continue;
        fixes.push({ title: `Change to '${right}'`, line: d.line, start: best, end: best + wrong.length,
                     newText: right, diagnostic: d.original || d });
    }
    return fixes;
}

function validName(name) {
    return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name) && !KEYWORDS.includes(name);
}

// What a rename at `pos` of `file` replaces, from references(): { here, edits } or { error }.
// Every reference must be in a file of the program (members); a declaration outside it (the
// stdlib, a package) is not renamed. A wider declaration (`P_sum` for `p.sum()`) keeps its
// prefix: only the last `w` characters of each reference are replaced, w the narrowest one.
function renamePlan(refs, members, file, pos) {
    const here = refs.find((r) => r.file === file && r.line === pos.line && r.start <= pos.character && pos.character <= r.end);
    if (!here) return { error: 'No symbol to rename here.' };
    const outside = refs.find((r) => !members.has(r.file));
    if (outside) return { error: `'${path.basename(outside.file)}' declares this symbol outside the program; it is not renamed.` };
    const w = Math.min(...refs.map((r) => r.end - r.start));
    const edits = refs.map((r) => ({ file: r.file, line: r.line, start: r.end - w, end: r.end }));
    return { here: { line: here.line, start: here.end - w, end: here.end }, edits };
}

module.exports = {
    KEYWORDS, COMPLETION_KINDS, hover, hoverMarkdown, completionContext, completionItems,
    enclosingCall, offsetAt, positionAt, signature, symbolTree, quickFixes, validName, renamePlan,
};
