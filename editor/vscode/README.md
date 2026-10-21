# Eskiu Language: VS Code Extension

Eskiu support for Visual Studio Code and editors built on it (Cursor, Antigravity,
VSCodium): syntax highlighting, errors as you type, completion, hover with types and
signatures, signature help, go to definition, references and rename, an outline,
formatting and a Run command.

## Features

- **Errors as you type.** `eskiuc --test-typechecker` runs shortly after you stop typing
  and when a file is opened or saved; errors and `-Wall`-style warnings show as underlines.
  An unsaved file is checked through a hidden copy next to it
  (`.name.esk.<pid>.eskiu-check.esk`, removed right after), so relative imports resolve.
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
  file that names the symbol under the cursor (a local resolves by scope, a field also in
  `Point{x: 1}` literals); F2 renames them all. A symbol declared in another file (an
  import) is not renamed.
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
| `eskiu.compilerPath` | `""` | Path to `eskiuc`. Empty uses `build/eskiuc` in a checkout of this repository, else `eskiuc` on `PATH`. Set it when the editor does not inherit your shell `PATH` (common for apps opened from the Dock). |
| `eskiu.checkOnType` | `true` | Type-check while typing, not only on open and save. |
| `eskiu.checkDelay` | `400` | Milliseconds after the last edit before checking. |

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
the same features: diagnostics, hover, go to definition, completion, signature help,
document symbols and formatting. It serves editors that speak LSP natively (Neovim, Helix,
Zed) and JetBrains IDEs through the Eskiu IntelliJ plugin. It runs the compiler given as
`initializationOptions.compilerPath`, else `$ESKIUC`, else the same lookup as the
extension, then `~/.eskiu/bin/eskiuc`.

## Limitations

Completion, hover and signature help need the file to parse: a line with a syntax error
elsewhere in the file leaves them empty until it is fixed (completion still offers the
keywords). They are computed by the C++ `eskiuc`.
