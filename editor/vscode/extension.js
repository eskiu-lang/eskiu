'use strict';
/**
 * extension.js: VS Code extension entry point for the Eskiu language.
 *
 * Every feature shells out to the compiler: diagnostics from `--test-typechecker`,
 * hover (types and signatures) from `--hover-at`, go-to-definition from
 * `--definition-at`, completion from `--complete-at`, the outline from `--symbols`,
 * signature help from the callee's `--hover-at`, formatting from `eskiuc fmt`, and the
 * Run command from `eskiuc run`. An unsaved document is checked through a hidden
 * sibling file, so relative imports still resolve.
 */

const vscode = require('vscode');
const { execFile } = require('child_process');
const path = require('path');
const fs   = require('fs');
const os   = require('os');

let diagnosticCollection;
const pendingChecks = new Map();

function config() {
    return vscode.workspace.getConfiguration('eskiu');
}

// Extra flags configured by user, e.g. ["--freestanding"]
function getCompilerFlags() {
    const flags = config().get('compilerFlags');
    if (Array.isArray(flags)) return flags.filter(Boolean);
    if (typeof flags === 'string' && flags.trim()) return flags.trim().split(/\s+/);
    return [];
}

// The compiler: the `eskiu.compilerPath` setting, else `build22/eskiuc` or `build/eskiuc`
// in workspace folders or above hintPath, else the installed `~/.eskiu/bin/eskiuc`, else
// `eskiuc` on PATH. In a workspace the user has not trusted (Workspace Trust), nothing the
// workspace holds picks the program run: VS Code ignores its compilerPath (a restricted
// setting in package.json) and its build directories are not searched.
function findEskiuc(hintPath) {
    const configured = (config().get('compilerPath') || '').trim();
    if (configured) return configured;
    if (process.env.ESKIUC) {
        try { fs.accessSync(process.env.ESKIUC, fs.constants.X_OK); return process.env.ESKIUC; } catch {}
    }

    const buildNames = ['build22', 'build', 'build-debug', 'build-release'];

    // 1. Check workspace folders
    const wfs = vscode.workspace.isTrusted ? (vscode.workspace.workspaceFolders || []) : [];
    for (const wf of wfs) {
        for (const b of buildNames) {
            const c = path.join(wf.uri.fsPath, b, 'eskiuc');
            try { fs.accessSync(c, fs.constants.X_OK); return c; } catch {}
        }
    }

    // 2. Check hintPath walking up directory tree
    if (hintPath && vscode.workspace.isTrusted) {
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

    // 3. Check relative to extension installation / checkout
    const relCandidates = [
        path.join(__dirname, '..', '..', 'build22', 'eskiuc'),
        path.join(__dirname, '..', '..', 'build', 'eskiuc'),
        path.join(__dirname, '..', '..', '..', 'build22', 'eskiuc'),
        path.join(__dirname, '..', '..', '..', 'build', 'eskiuc'),
    ];
    for (const c of relCandidates) {
        try { fs.accessSync(c, fs.constants.X_OK); return c; } catch {}
    }

    // 4. The installer's location (an editor opened from the Dock lacks the shell PATH)
    const installed = path.join(os.homedir(), '.eskiu', 'bin', 'eskiuc');
    try { fs.accessSync(installed, fs.constants.X_OK); return installed; } catch {}
    return 'eskiuc';
}

function runCompiler(args, timeout, cwd, hintPath) {
    const compiler = findEskiuc(hintPath);
    return new Promise((resolve) => {
        execFile(compiler, args, { timeout, cwd }, (err, stdout, stderr) => {
            if (err && err.code === 'ENOENT') {
                reportMissingCompiler(compiler);
                resolve({ ok: false, stdout: '', stderr: '' });
                return;
            }
            resolve({ ok: !err, stdout: stdout || '', stderr: stderr || '' });
        });
    });
}

let missingReported = false;
function reportMissingCompiler(name) {
    if (missingReported) return;
    missingReported = true;
    vscode.window.showWarningMessage(
        `Eskiu: could not run '${name || 'eskiuc'}'. Install eskiuc or set "eskiu.compilerPath".`,
        'Open Settings'
    ).then((choice) => {
        if (choice) vscode.commands.executeCommand('workbench.action.openSettings', 'eskiu.compilerPath');
    });
}

// Find an entry point (e.g. kernel.esk or main.esk) that imports docPath,
// so multi-file symbols are resolved without spurious 'undefined variable/function' errors.
function findEntryPoint(docPath) {
    if (!docPath) return null;
    const configured = (config().get('entryPoint') || '').trim();
    if (configured) {
        if (path.isAbsolute(configured) && fs.existsSync(configured)) return configured;
        const wfs = vscode.workspace.workspaceFolders || [];
        for (const wf of wfs) {
            const p = path.join(wf.uri.fsPath, configured);
            if (fs.existsSync(p)) return p;
        }
        const rel = path.join(path.dirname(docPath), configured);
        if (fs.existsSync(rel)) return rel;
    }

    const docDir = path.dirname(docPath);
    const docBase = path.basename(docPath);

    // Common root names to prioritize
    const rootNames = ['kernel.esk', 'main.esk', 'app.esk', 'index.esk', 'root.esk'];
    for (const r of rootNames) {
        const rootPath = path.join(docDir, r);
        if (rootPath !== docPath && fs.existsSync(rootPath)) {
            try {
                const text = fs.readFileSync(rootPath, 'utf8');
                if (text.includes(`"${docBase}"`) || text.includes(`<${docBase}>`)) {
                    return rootPath;
                }
            } catch {}
        }
    }

    // Check sibling .esk files in the same directory
    try {
        const files = fs.readdirSync(docDir);
        for (const f of files) {
            if (!f.endsWith('.esk') || f.startsWith('.')) continue;
            const full = path.join(docDir, f);
            if (full === docPath) continue;
            try {
                const text = fs.readFileSync(full, 'utf8');
                if (text.includes(`"${docBase}"`)) {
                    return full;
                }
            } catch {}
        }
    } catch {}

    return null;
}

// Run `fn(filePath, cwd)` on the document's current text.
// When standalone is false, multi-file entry points (e.g. kernel.esk, main.esk)
// are used to resolve imports and prevent false errors.
async function withCurrentText(document, fn, standalone = false) {
    const origPath = document.uri.fsPath;
    const entryPoint = standalone ? null : findEntryPoint(origPath);
    const dir = document.isUntitled ? os.tmpdir() : path.dirname(origPath);
    const base = document.isUntitled ? 'untitled.esk' : path.basename(origPath);

    if (!document.isDirty && !document.isUntitled) {
        if (entryPoint && fs.existsSync(entryPoint)) {
            const res = await fn(entryPoint, path.dirname(entryPoint));
            return { result: res, checked: origPath, orig: origPath };
        }
        const res = await fn(origPath, dir);
        return { result: res, checked: origPath, orig: origPath };
    }

    const tmp = path.join(dir, `.${base}.${process.pid}.${Date.now()}.eskiu-check.esk`);
    try {
        fs.writeFileSync(tmp, document.getText());
    } catch {
        const fallbackTarget = entryPoint || origPath;
        const res = await fn(fallbackTarget, path.dirname(fallbackTarget));
        return { result: res, checked: origPath, orig: origPath };
    }

    let tmpEntry = null;
    try {
        if (entryPoint && fs.existsSync(entryPoint)) {
            try {
                const entryContent = fs.readFileSync(entryPoint, 'utf8');
                const relTmp = path.basename(tmp);
                const replaced = entryContent.replace(
                    new RegExp(`(import\\s+["'][^"']*?)\\b${base.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(["'])`, 'g'),
                    `$1${relTmp}$2`
                );
                if (replaced !== entryContent) {
                    tmpEntry = path.join(path.dirname(entryPoint), `.${path.basename(entryPoint)}.${process.pid}.${Date.now()}.eskiu-check.esk`);
                    fs.writeFileSync(tmpEntry, replaced);
                }
            } catch {}
        }

        const targetFile = tmpEntry || (entryPoint ? null : tmp);
        if (targetFile) {
            const res = await fn(targetFile, path.dirname(targetFile));
            return { result: res, checked: tmp, orig: origPath };
        } else {
            const res = await fn(entryPoint || tmp, dir);
            return { result: res, checked: tmp, orig: origPath };
        }
    } finally {
        fs.rm(tmp, { force: true }, () => {});
        if (tmpEntry) {
            fs.rm(tmpEntry, { force: true }, () => {});
        }
    }
}

// Diagnostics located in the checked file or original file belong to this document.
function parseErrors(text, checkedPath, origPath, document) {
    const diagnostics = [];
    const re = /^(?:error:\s*)?(.+?):(\d+):(\d+):\s*(.+)$/gm;
    const wantChecked = path.resolve(checkedPath);
    const wantOrig = origPath ? path.resolve(origPath) : null;
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

        // Find token end for a precise range squiggle
        let endCh = ch + 1;
        if (document && ln < document.lineCount) {
            const lineText = document.lineAt(ln).text;
            if (ch < lineText.length) {
                while (endCh < lineText.length && /[a-zA-Z0-9_]/.test(lineText[endCh])) {
                    endCh++;
                }
            }
        }

        const severity = msg.startsWith('warning')
            ? vscode.DiagnosticSeverity.Warning
            : vscode.DiagnosticSeverity.Error;
        const cleanMsg = msg.replace(/^(error|warning):\s*/, '');
        const d = new vscode.Diagnostic(new vscode.Range(ln, ch, ln, endCh), cleanMsg, severity);
        d.source = 'eskiuc';
        diagnostics.push(d);
    }
    return diagnostics;
}

async function validate(document) {
    if (document.languageId !== 'eskiu') return;
    const version = document.version;
    const extraFlags = getCompilerFlags();
    const { result, checked, orig } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--test-typechecker'], 10000, cwd, document.uri.fsPath));
    if (document.isClosed || document.version !== version) return;
    diagnosticCollection.set(document.uri, parseErrors(result.stdout + result.stderr, checked, orig, document));
}

function scheduleValidate(document) {
    if (document.languageId !== 'eskiu' || !config().get('checkOnType')) return;
    const key = document.uri.toString();
    clearTimeout(pendingChecks.get(key));
    pendingChecks.set(key, setTimeout(() => {
        pendingChecks.delete(key);
        validate(document);
    }, Math.max(100, config().get('checkDelay') || 400)));
}

async function hover(document, position) {
    const at = `${position.line + 1}:${position.character + 1}`;
    const extraFlags = getCompilerFlags();
    const { result } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--hover-at', at], 5000, cwd, document.uri.fsPath), true);
    const lines = result.stdout.trim().split('\n').map((l) => l.trim()).filter(Boolean);
    const lastLine = lines[lines.length - 1] || '';
    if (!lastLine || lastLine.startsWith('(') || lastLine.startsWith('error:')) return null;
    const md = new vscode.MarkdownString().appendCodeblock(lastLine, 'eskiu');
    // A `Type_method` reached by dot syntax: say how to call it.
    const m = lastLine.match(/^\S.*?\b([A-Z][A-Za-z0-9]*)_([a-z_][A-Za-z0-9_]*)(<[^(]*>)?\(\s*(const\s+)?\*?\1\b[^,)]*\bself\b/);
    if (m) md.appendMarkdown(`\n\nMethod of \`${m[1]}\`: call it as \`x.${m[2]}(...)\`.`);
    return new vscode.Hover(md);
}

async function definition(document, position) {
    const at = `${position.line + 1}:${position.character + 1}`;
    const extraFlags = getCompilerFlags();
    const { result, checked } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--definition-at', at], 5000, cwd, document.uri.fsPath), true);
    const m = result.stdout.trim().match(/^(.+):(\d+):(\d+)$/m);
    if (!m) return null;
    const [, file, line, col] = m;
    const target = path.resolve(file) === path.resolve(checked) ||
                   path.basename(file) === path.basename(document.uri.fsPath)
        ? document.uri
        : vscode.Uri.file(path.resolve(path.dirname(checked), file));
    return new vscode.Location(target, new vscode.Position(
        Math.max(0, parseInt(line, 10) - 1), Math.max(0, parseInt(col, 10) - 1)));
}

// `--references-at`: one FILE:LINE:COL:WIDTH per line, the checked copy of the buffer
// being this document.
async function referenceRanges(document, position) {
    const at = `${position.line + 1}:${position.character + 1}`;
    const { result, checked } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...getCompilerFlags(), '--references-at', at], 5000, cwd, document.uri.fsPath), true);
    const refs = [];
    for (const row of result.stdout.split('\n')) {
        const m = row.trim().match(/^(.+):(\d+):(\d+):(\d+)$/);
        if (!m) continue;
        const [, file, line, col, width] = m;
        const uri = path.resolve(file) === path.resolve(checked) ? document.uri
            : vscode.Uri.file(path.resolve(path.dirname(checked), file));
        const start = new vscode.Position(parseInt(line, 10) - 1, parseInt(col, 10) - 1);
        refs.push(new vscode.Location(uri, new vscode.Range(start, start.translate(0, parseInt(width, 10)))));
    }
    return refs;
}

async function references(document, position) {
    return referenceRanges(document, position);
}

async function prepareRename(document, position) {
    const refs = await referenceRanges(document, position);
    const here = refs.find((r) => r.uri.toString() === document.uri.toString() && r.range.contains(position));
    if (!here) throw new Error('No symbol to rename here.');
    if (refs.some((r) => r.uri.toString() !== document.uri.toString()))
        throw new Error('This symbol is declared in another file.');
    return here.range;
}

async function rename(document, position, newName) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(newName) || KEYWORDS.includes(newName))
        throw new Error(`'${newName}' is not a valid name.`);
    const refs = await referenceRanges(document, position);
    if (!refs.length) return null;
    if (refs.some((r) => r.uri.toString() !== document.uri.toString()))
        throw new Error('This symbol is declared in another file.');
    // A wider declaration (`P_sum` for `p.sum()`) keeps its prefix: only the name is replaced.
    const w = Math.min(...refs.map((r) => r.range.end.character - r.range.start.character));
    const edit = new vscode.WorkspaceEdit();
    for (const r of refs) edit.replace(r.uri, new vscode.Range(r.range.end.translate(0, -w), r.range.end), newName);
    return edit;
}

const KEYWORDS = [
    'if', 'else', 'for', 'in', 'while', 'do', 'switch', 'case', 'default', 'break', 'continue',
    'return', 'match', 'struct', 'union', 'enum', 'interface', 'type', 'let', 'const', 'static',
    'volatile', 'extern', 'intrinsic', 'import', 'async', 'await', 'try', 'catch', 'finally',
    'throw', 'defer', 'errdefer', 'sizeof', 'null', 'true', 'false', 'fn', 'operator', 'packed',
    'escaping', 'must_use', 'alloc_with', 'free_closure', 'thread_create', 'thread_join',
    'int', 'int8', 'int16', 'int32', 'int64', 'uint', 'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'bool', 'char', 'string', 'void',
];

const COMPLETION_KINDS = {
    field: vscode.CompletionItemKind.Field,
    method: vscode.CompletionItemKind.Method,
    function: vscode.CompletionItemKind.Function,
    variable: vscode.CompletionItemKind.Variable,
    global: vscode.CompletionItemKind.Variable,
    struct: vscode.CompletionItemKind.Struct,
    interface: vscode.CompletionItemKind.Interface,
    enum: vscode.CompletionItemKind.Enum,
    enummember: vscode.CompletionItemKind.EnumMember,
    type: vscode.CompletionItemKind.TypeParameter,
};

// `--complete-at` replaces the identifier at the cursor (or the member after `recv.`)
// and prints `kind<TAB>name<TAB>detail` per candidate.
async function complete(document, position) {
    const line = document.lineAt(position.line).text;
    const before = line.slice(0, position.character);
    if (/^\s*(\/\/|#)/.test(before) || (before.split('"').length % 2 === 0)) return [];
    const member = /\.\s*[A-Za-z_0-9]*$/.test(before);
    const at = `${position.line + 1}:${position.character + 1}`;
    const extraFlags = getCompilerFlags();
    const { result } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--complete-at', at], 8000, cwd, document.uri.fsPath), true);
    const items = [];
    for (const row of result.stdout.split('\n')) {
        const [kind, name, detail] = row.split('\t');
        if (!name || !COMPLETION_KINDS[kind]) continue;
        const item = new vscode.CompletionItem(name, COMPLETION_KINDS[kind]);
        item.detail = detail || '';
        // Locals first, then members, then the rest of the program.
        item.sortText = (kind === 'variable' ? '0' : kind === 'field' ? '1' : kind === 'method' ? '2' : '5') + name;
        items.push(item);
    }
    if (!member) {
        for (const k of KEYWORDS) {
            const item = new vscode.CompletionItem(k, vscode.CompletionItemKind.Keyword);
            item.sortText = '9' + k;
            items.push(item);
        }
    }
    return items;
}

// The call around the cursor: the callee's name position and the index of the
// argument being written, found by scanning back over balanced brackets and strings.
function enclosingCall(document, position) {
    const text = document.getText(new vscode.Range(new vscode.Position(Math.max(0, position.line - 30), 0), position));
    let depth = 0, commas = 0, inStr = false;
    for (let i = text.length - 1; i >= 0; i--) {
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
            const offset = document.offsetAt(new vscode.Position(Math.max(0, position.line - 30), 0)) + k;
            return { pos: document.positionAt(offset), active: commas, dot: text[k - 1] === '.' };
        } else if (c === ',' && depth === 0) commas++;
        else if (c === ';' && depth === 0) return null;
    }
    return null;
}

async function signatureHelp(document, position) {
    const call = enclosingCall(document, position);
    if (!call) return null;
    const at = `${call.pos.line + 1}:${call.pos.character + 1}`;
    const extraFlags = getCompilerFlags();
    const { result } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--hover-at', at], 5000, cwd, document.uri.fsPath), true);
    const sig = (result.stdout.trim().split('\n').pop() || '').trim();
    const open = sig.indexOf('(');
    if (open < 0 || !sig.endsWith(')') || sig.startsWith('fn(')) return null;
    // Split the parameter list at top-level commas (a `fn(A,B)->C` parameter has its own).
    const params = [];
    let depth = 0, cur = '';
    for (const c of sig.slice(open + 1, -1)) {
        if (c === '(' || c === '<' || c === '[') depth++;
        if (c === ')' || c === '>' || c === ']') depth--;
        if (c === ',' && depth === 0) { params.push(cur.trim()); cur = ''; } else cur += c;
    }
    if (cur.trim()) params.push(cur.trim());
    // A dot-call passes the receiver as `self`: the first argument written is the second parameter.
    const shown = call.dot && params.length && /\bself$/.test(params[0]) ? params.slice(1) : params;
    const info = new vscode.SignatureInformation(sig);
    info.parameters = shown.map((p) => new vscode.ParameterInformation(p));
    const help = new vscode.SignatureHelp();
    help.signatures = [info];
    help.activeSignature = 0;
    const variadic = shown.length && shown[shown.length - 1] === '...';
    help.activeParameter = variadic ? Math.min(call.active, shown.length - 1) : call.active;
    return help;
}

const SYMBOL_KINDS = {
    function: vscode.SymbolKind.Function,
    method: vscode.SymbolKind.Method,
    struct: vscode.SymbolKind.Struct,
    field: vscode.SymbolKind.Field,
    enum: vscode.SymbolKind.Enum,
    interface: vscode.SymbolKind.Interface,
    type: vscode.SymbolKind.TypeParameter,
    variable: vscode.SymbolKind.Variable,
    constant: vscode.SymbolKind.Constant,
};

// The outline: `--symbols` prints `kind name LINE:COL container detail` per declaration;
// fields and inline methods go under their struct.
async function documentSymbols(document) {
    const extraFlags = getCompilerFlags();
    const { result } = await withCurrentText(document, (file, cwd) =>
        runCompiler([file, ...extraFlags, '--symbols'], 8000, cwd, document.uri.fsPath), true);
    const top = [];
    const byName = new Map();
    for (const row of result.stdout.split('\n')) {
        const [kind, name, at, container, detail] = row.split('\t');
        if (!name || !SYMBOL_KINDS[kind] || !at) continue;
        const [l, c] = at.split(':').map((n) => Math.max(0, parseInt(n, 10) - 1));
        if (l >= document.lineCount) continue;
        const start = new vscode.Position(l, c);
        const full = new vscode.Range(start, document.lineAt(l).range.end);
        const sel = new vscode.Range(start, start.translate(0, name.length));
        const sym = new vscode.DocumentSymbol(name, detail || '', SYMBOL_KINDS[kind], full, sel);
        const parent = container && byName.get(container);
        if (parent) {
            parent.children.push(sym);
            if (parent.range.end.isBefore(full.end)) parent.range = new vscode.Range(parent.range.start, full.end);
        } else {
            top.push(sym);
            if (kind === 'struct') byName.set(name, sym);
        }
    }
    return top;
}

// "undefined variable 'totl' (did you mean 'total'?)": a quick fix that replaces the
// misspelled name, found on the diagnostic's line nearest to its column.
const MISSPELLED = /(?:variable|function|method|member|type) '([^']+)'[^]*\(did you mean '([^']+)'\?\)/;

function quickFixes(document, range, context) {
    const actions = [];
    for (const d of context.diagnostics) {
        if (d.source !== 'eskiuc') continue;
        const m = d.message.match(MISSPELLED);
        if (!m) continue;
        const [, wrong, right] = m;
        const line = document.lineAt(d.range.start.line).text;
        const re = new RegExp(`\\b${wrong.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`, 'g');
        let best = -1, hit;
        while ((hit = re.exec(line)) !== null) {
            if (best < 0 || Math.abs(hit.index - d.range.start.character) < Math.abs(best - d.range.start.character)) best = hit.index;
        }
        if (best < 0) continue;
        const fix = new vscode.CodeAction(`Change to '${right}'`, vscode.CodeActionKind.QuickFix);
        fix.edit = new vscode.WorkspaceEdit();
        fix.edit.replace(document.uri, new vscode.Range(d.range.start.line, best, d.range.start.line, best + wrong.length), right);
        fix.diagnostics = [d];
        fix.isPreferred = true;
        actions.push(fix);
    }
    return actions;
}

// `eskiuc fmt` rewrites a file in place, so format a temporary copy and replace the
// whole document with the result.
async function format(document) {
    const tmp = path.join(os.tmpdir(), `eskiu-fmt-${process.pid}-${Date.now()}.esk`);
    try {
        fs.writeFileSync(tmp, document.getText());
        const r = await runCompiler(['fmt', tmp], 10000, undefined, document.uri.fsPath);
        if (!r.ok) return [];
        const formatted = fs.readFileSync(tmp, 'utf8');
        if (formatted === document.getText()) return [];
        const all = new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length));
        return [vscode.TextEdit.replace(all, formatted)];
    } catch {
        return [];
    } finally {
        fs.rm(tmp, { force: true }, () => {});
    }
}

function quote(s) {
    return process.platform === 'win32' ? `"${s}"` : `'${s.replace(/'/g, `'\\''`)}'`;
}

async function runFile(uri) {
    const editor = vscode.window.activeTextEditor;
    const document = uri
        ? await vscode.workspace.openTextDocument(uri)
        : editor && editor.document;
    if (!document || document.languageId !== 'eskiu') {
        vscode.window.showInformationMessage('Eskiu: open a .esk file to run it.');
        return;
    }
    if (document.isUntitled) {
        vscode.window.showInformationMessage('Eskiu: save the file before running it.');
        return;
    }
    if (document.isDirty) await document.save();
    let terminal = vscode.window.terminals.find((t) => t.name === 'Eskiu');
    if (!terminal) terminal = vscode.window.createTerminal('Eskiu');
    terminal.show(true);
    const compiler = findEskiuc(document.uri.fsPath);
    terminal.sendText(`${quote(compiler)} run ${quote(document.uri.fsPath)}`);
}

function activate(context) {
    diagnosticCollection = vscode.languages.createDiagnosticCollection('eskiu');
    context.subscriptions.push(diagnosticCollection);

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(validate),
        vscode.workspace.onDidSaveTextDocument(validate),
        vscode.workspace.onDidChangeTextDocument((e) => scheduleValidate(e.document)),
        vscode.workspace.onDidCloseTextDocument((doc) => {
            clearTimeout(pendingChecks.get(doc.uri.toString()));
            pendingChecks.delete(doc.uri.toString());
            diagnosticCollection.delete(doc.uri);
        }),
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('eskiu')) {
                missingReported = false;
                vscode.workspace.textDocuments.forEach(validate);
            }
        }),
        vscode.languages.registerHoverProvider('eskiu', { provideHover: hover }),
        vscode.languages.registerDefinitionProvider('eskiu', { provideDefinition: definition }),
        vscode.languages.registerReferenceProvider('eskiu', { provideReferences: references }),
        vscode.languages.registerRenameProvider('eskiu', { provideRenameEdits: rename, prepareRename }),
        vscode.languages.registerCompletionItemProvider('eskiu', { provideCompletionItems: complete }, '.'),
        vscode.languages.registerSignatureHelpProvider('eskiu', { provideSignatureHelp: signatureHelp }, '(', ','),
        vscode.languages.registerDocumentSymbolProvider('eskiu', { provideDocumentSymbols: documentSymbols }),
        vscode.languages.registerCodeActionsProvider('eskiu', { provideCodeActions: quickFixes },
            { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }),
        vscode.languages.registerDocumentFormattingEditProvider('eskiu', { provideDocumentFormattingEdits: format }),
        vscode.commands.registerCommand('eskiu.runFile', runFile),
    );

    vscode.workspace.textDocuments.forEach(validate);
}

function deactivate() {
    for (const t of pendingChecks.values()) clearTimeout(t);
    if (diagnosticCollection) diagnosticCollection.dispose();
}

module.exports = { activate, deactivate, _test: { complete, signatureHelp, documentSymbols, hover, quickFixes } };
