# Eskiu Language: IntelliJ IDEA / JetBrains IDEs

Syntax highlighting for `.esk` files in IntelliJ IDEA (and other JetBrains IDEs:
CLion, GoLand, PyCharm, RustRover, …) by **reusing the same TextMate grammar that
powers the VS Code extension**, so the two never drift apart, and with the plugin
(Option B) the language features of the Eskiu language server.

JetBrains IDEs ship with a built-in TextMate engine, so no custom lexer/parser is
needed: the grammar at `bundle/Syntaxes/eskiu.tmLanguage.json` (a copy of the
canonical `editor/vscode/syntaxes/eskiu.tmLanguage.json`) drives highlighting
directly.

---

## Option A: Import the bundle (works today, recommended)

The fastest path, no build required. In IntelliJ:

1. **Settings/Preferences → Editor → TextMate Bundles**
2. Click **+** and select this folder:
   `…/eskiu/editor/intellij/bundle`
3. **Apply**. Open any `.esk` file: keywords, strings, comments, numbers, types,
   operators, and `#pragma`/preprocessor directives are now highlighted.

To pick a color theme for the scopes: **Settings → Editor → Color Scheme → TextMate**.

This binds `.esk` to the `source.eskiu` scope automatically (the grammar's
`fileTypes` lists `esk`).

---

## Option B: Package as a distributable plugin

For a one-click install (and JetBrains Marketplace), `plugin/` is a Gradle project
that wraps the same bundle as an installable plugin. It depends on the platform
TextMate plugin (`org.jetbrains.plugins.textmate`) and contributes the bundle via a
`TextMateBundleProvider`, so installing the plugin = the grammar is registered with
no manual import.

```bash
cd editor/intellij/plugin
./gradlew buildPlugin        # → build/distributions/eskiu-intellij-*.zip (needs a JDK 17+)
```

Install the resulting zip via **Settings → Plugins → ⚙ → Install Plugin from Disk…**.

To check the plugin against an IDE you have installed (binary compatibility with its
platform and with LSP4IJ), run the JetBrains plugin verifier on it:

```bash
./gradlew verifyPlugin -PverifyIde="/Applications/Android Studio.app"
```

> The Gradle project builds against IntelliJ 2024.1, the first platform with the
> `TextMateBundleProvider` API, and sets no upper bound, so the zip installs in any
> JetBrains IDE from 2024.1 on. Building needs a JDK 17+; the Gradle wrapper fetches
> everything else on the first build.

---

## Keeping the grammar in sync

The **canonical** grammar is `editor/vscode/syntaxes/eskiu.tmLanguage.json`. The copy
here (and the one inside `plugin/`) must be kept in sync whenever the language
gains or loses syntax. Refresh both with:

```bash
make -C editor sync-grammar     # or: cp editor/vscode/syntaxes/eskiu.tmLanguage.json editor/intellij/bundle/Syntaxes/
```

## Language features (Option B)

The plugin also runs the Eskiu language server through
[LSP4IJ](https://plugins.jetbrains.com/plugin/23257-lsp4ij) (Red Hat's LSP client,
installed from the Marketplace as a dependency). It bundles `editor/vscode/server.js`
with the modules it requires, the same server Neovim or Helix use and the same code the
VS Code extension runs, which calls the compiler's editor modes. A file is checked in the
program it belongs to (see `editor/vscode/README.md`, "How a file is checked"):

- errors as you type (`--test-typechecker`), with a quick fix for a misspelled name;
- find usages and rename across the files of the program (`--references-at`);
- completion: after `x.` the fields of `x`'s struct and the `Type_method` functions dot
  syntax reaches, elsewhere the names in scope and the keywords (`--complete-at`);
- hover with the type of a variable or field and the signature of a function or method
  (`--hover-at`), and signature help while writing a call;
- go to definition for variables, functions, methods (also from a dot-call) and fields
  (`--definition-at`);
- the Structure view (`--symbols`) and Reformat Code (`eskiuc fmt`).

The server needs Node.js. It looks for `node` on the IDE's PATH, then in Homebrew, fnm,
nvm and Volta installs, and for `eskiuc` in `build22/` or `build/` of the project, then
`~/.eskiu/bin`, then PATH. Set either path in **Settings | Tools | Eskiu** when it is not
found. The same page holds the server's other settings, the VS Code extension's
`eskiu.*`: compiler flags, the entry point, check while typing and the check delay; they
apply when the server starts (restart it from LSP4IJ's Language Servers view).
Option A (the bundle alone) gives highlighting only.
