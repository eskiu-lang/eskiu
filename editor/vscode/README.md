# Eskiu Language: VS Code Extension

Eskiu support for Visual Studio Code and editors built on it (Cursor, Antigravity,
VSCodium): syntax highlighting, errors as you type, completion, hover with types and
signatures, signature help, go to definition, references and rename, an outline,
formatting and a Run command.

## Features

- **Errors as you type.** `eskiuc --test-typechecker` runs shortly after you stop typing
  and when a file is opened or saved; errors and `-Wall`-style warnings show as underlines.
  The file is checked in the program it belongs to, with your unsaved text (see
  [How a file is checked](#how-a-file-is-checked)).
- **Completion** (`--complete-at`): after `x.` it lists the fields of `x`'s struct and the
  functions dot syntax reaches (`String_replace_all` is offered as `replace_all`, generic
  ones such as `List_push` on a `List<int>` too); elsewhere it lists the variables in
  scope, the program's functions, types and enum members, and the keywords. Internal
  (`_`-prefixed) stdlib names are left out.
- **Hover** (`--hover-at`) shows the type of a variable or field, and the signature of a
  function at its declaration, at a call and at a dot-call (`s.replace_all` shows
  `int64 String_replace_all(String* self, string from, string to)`).
- **Signature help** shows the parameters of the call being written, with the current one
  highlighted; a dot-call leaves out `self`.
- **Go to definition** (`--definition-at`) jumps to where a variable, function, method
  (also from a dot-call, inline methods included) or struct field is declared.
- **Find references and rename** (`--references-at`): Shift+F12 lists every place in the
  program that names the symbol under the cursor, in every file of it (a local resolves by
  scope, a field also in `Point{x: 1}` literals); F2 renames them all, from the declaring
  file or from any file that uses it. Text in strings and comments, and a different
  symbol of the same name (a local in another function), are left alone. A symbol
  declared outside the program's own files (the stdlib, a package) is not renamed.
- **Quick fix** for a misspelled name: an error such as `undefined variable 'totl' (did you
  mean 'total'?)` offers "Change to 'total'" (Cmd/Ctrl+.). Uses of a `deprecated` function
  show as warnings with the replacement.
- **Outline** (`--symbols`): functions, structs with their fields and inline methods,
  enums, interfaces, type aliases and globals, for the Outline view, breadcrumbs and
  Go to Symbol (Cmd/Ctrl+Shift+O).
- **Format Document** runs `eskiuc fmt` and is the default formatter for `.esk` files.
- **Eskiu: Run File** (also the play button in the editor title) saves the file and runs
  `eskiuc run` in a terminal.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `eskiu.compilerPath` | `""` | Path to `eskiuc`. Empty uses `$ESKIUC`, else `build22/eskiuc` or `build/eskiuc` in a trusted workspace (its folders and the directories above the file), else `~/.eskiu/bin/eskiuc`, else `eskiuc` on `PATH`. Set it when the editor does not inherit your shell `PATH` (common for apps opened from the Dock). |
| `eskiu.compilerFlags` | `[]` | Flags added to every compiler run, e.g. `["-Wall"]`, or `["--target=arm64-apple-ios17.0"]` for a program whose files are imported only for that target. |
| `eskiu.entryPoint` | `""` | The program's root file (absolute, or relative to a workspace folder or to the file). Used for a file it reaches; see below. |
| `eskiu.checkOnType` | `true` | Type-check while typing, not only on open and save. |
| `eskiu.checkDelay` | `400` | Milliseconds after the last edit before checking (at least 100). |

## How a file is checked

`import "x.esk";` includes x where it stands, once per program, so a file sees what the
files before it declared even when it does not import them itself. A file is therefore
checked in its program, from the program's root:

1. `eskiu.entryPoint`, when it is the file or reaches it;
2. else the `main` of the nearest `eskiu.toml` above the file (`src/main.esk` when the
   manifest names none, as `eskiuc build` assumes), when it reaches the file;
3. else the top of the chain of files that import the file (its importer, the importer
   of that, and so on, looking in its directory and up to four above);
4. else the file alone.

"Reaches" means through relative `import "..."` lines, those inside `#if` branches
included. Errors run `eskiuc ROOT --test-typechecker` with `--overlay=FILE=COPY` for each
open file whose text differs from disk, so the compiler reads your unsaved text wherever
the file is imported and reports real paths and columns. Only the document's own
diagnostics are shown on it; another file's are dropped (that file shows them when it is
open and checked). When the program stops at a syntax error in another file before
checking this one, the file shows one note saying where, instead of looking clean.

Hover, go to definition, references, rename, completion, signature help and the outline
take a position in the root file, so they run on the program written as one text (each
relative import replaced by the file, unsaved text included), given to the compiler as
the root's overlay, and every position maps back to its file. A compiler without
`--overlay` (0.16.3 and older) still works: the text goes in a hidden copy next to the
root (`.main.esk.<pid>.<time>.eskiu-check.esk`, removed right after), and you are told
to update it.

## Publishing

`.github/workflows/editors.yml` packages the extension and the IntelliJ plugin on each
release tag and uploads them as artifacts. It publishes to a store only when that store's
token is a repository secret: `OVSX_PAT` for Open VSX (VSCodium, Cursor, Antigravity),
`VSCE_PAT` for the Visual Studio Marketplace, `JETBRAINS_TOKEN` for the JetBrains
Marketplace. The `doranteseduardo` namespace must exist on Open VSX first
(`npx ovsx create-namespace doranteseduardo -p TOKEN`).

## Install

Each release has the extension attached; `releases/latest/download/eskiu-vscode.vsix` is
always the newest:

```bash
curl -fsSLO https://github.com/eskiu-lang/eskiu/releases/latest/download/eskiu-vscode.vsix
code --install-extension eskiu-vscode.vsix    # or cursor / antigravity-ide / codium
```

Or in the editor: the Extensions view's **⋯** menu, **Install from VSIX…**.

To build it from this checkout instead:

```bash
cd editor/vscode
npx @vscode/vsce package
code --install-extension eskiu-language-*.vsix
```

For development, link the folder into the extensions directory instead, so edits apply
after a window reload:

```bash
ln -s "$PWD/editor/vscode" ~/.vscode/extensions/eskiu-language
```

## What it highlights

| Token | Color category |
|---|---|
| `//` and `/* */` comments | comment |
| `"..."` string literals (with escape sequences) | string |
| `'a'` char literals | string |
| `0xFF`, `3.14`, `42` numbers | constant.numeric |
| `true`, `false`, `null`, `__FILE__`, `__LINE__` | constant.language |
| `#define #undef #ifdef #ifndef #else #endif #pragma #error` directives | keyword.control.directive |
| `if else for while do switch case break continue return in await match` | keyword.control |
| Loop labels `outer:` before a `for`/`while`/`do` (for `break outer` / `continue outer`) | entity.name.label |
| `try catch finally throw defer errdefer` | keyword.control.exception |
| `let struct packed union interface enum fn operator extern intrinsic import` | keyword.declaration |
| `const volatile static escaping must_use async` | storage.modifier |
| `sizeof asm alloc_with thread_create thread_join va_start va_arg va_end` | keyword.other |
| `import <mem>;` stdlib imports and `type Alias = …` | namespace / declaration |
| `int uint8 float double bool char string void` … | support.type |
| Function names at declaration and call sites | entity.name.function |
| A name after `.` (a field or method, keywords included: `j.int(5)`) | variable.other.member / entity.name.function |
| Type names (uppercase-starting) | entity.name.type |
| Template parameters `<T, E>`, incl. bounded `<T: Iface>` / `<T: A + B>` | entity.name.type.parameter |
| `+= == && \| ^ << ..` operators (incl. the `..` range) | keyword.operator |

## File association

Files ending in `.esk` are automatically associated with the Eskiu language.
To associate manually, add to VS Code `settings.json`:

```json
"files.associations": {
  "*.esk": "eskiu"
}
```

## How it works

`extension.js` uses only the VS Code API (no npm packages) and shells out to the compiler
for every feature, parsing its `file:line:col: message` output into diagnostics.

`server.js` is a standalone language server (JSON-RPC over stdio, no npm packages) with
the same features. It serves editors that speak LSP natively (Neovim, Helix, Zed) and
JetBrains IDEs through the Eskiu IntelliJ plugin.

Both run the compiler through the same modules, so they behave the same on the same
input: `project.js` (the compiler, the program a file belongs to, checks and the editor
modes on the current text), `features.js` (hover text, completion items, signature help,
the outline, the quick fix, rename edits) and `flatten.js` (the import graph). The only
difference is the editor: the Run command is the extension's own (LSP has no run
request).

The server takes the extension's settings, without the `eskiu.` prefix, in
`initializationOptions` or `workspace/didChangeConfiguration` (`{"eskiu": {...}}` or the
keys alone): `compilerPath`, `compilerFlags` (an array or one string), `entryPoint`,
`checkOnType`, `checkDelay`; their defaults come from `$ESKIUC`, `$ESKIU_FLAGS`,
`$ESKIU_ENTRY_POINT`, `$ESKIU_CHECK_ON_TYPE` and `$ESKIU_CHECK_DELAY`. It looks in the
workspace's build directories for the compiler only when the client marks the workspace
as trusted (`searchWorkspace: true`, or `ESKIU_SEARCH_WORKSPACE=1`). For example, in
Neovim:

```lua
vim.lsp.start({
  name = 'eskiu',
  cmd = { 'node', '/path/to/eskiu/editor/vscode/server.js' },
  root_dir = vim.fs.root(0, { 'eskiu.toml', '.git' }),
  init_options = { compilerFlags = { '-Wall' }, searchWorkspace = true },
})
```

## Workspace Trust

A repository you open must not be able to run a program of its choice. In a workspace you
have not trusted, the extension ignores the workspace's `eskiu.compilerPath`,
`eskiu.compilerFlags` and `eskiu.entryPoint` settings and does not use a `build/eskiuc` in
it; it runs the installed compiler. The IntelliJ plugin does the same for projects the IDE
has not trusted.

## Limitations

Hover, go to definition, references and signature help need the program to parse: a
syntax error in the file or in another file of the program leaves them empty until it is
fixed. Completion leaves out a broken line of the file being edited (and still offers the
keywords). They are computed by the C++ `eskiuc`.

A file imported only inside an `#if` branch the host does not take (`#if
defined(TARGET_OS_IPHONE)`) is not part of the host's program: give the target in
`eskiu.compilerFlags` (`--target=arm64-apple-ios17.0`) to check it.
