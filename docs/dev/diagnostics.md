# Diagnostics as JSON

`eskiuc --diagnostics=json` (and the self-hosted `eskiuc-esk`) prints errors and warnings
on stderr as JSON lines: one object per diagnostic, one diagnostic per line. The default,
`--diagnostics=text`, is the text form described in the language spec (section 17). The
flag works in every mode that reports diagnostics: a build (`-o`), `--test-typechecker`,
`--test-parser` and the others; stdout is unchanged.

```
$ eskiuc --diagnostics=json app.esk -o app
{"severity":"error","id":"undefined-variable","message":"undefined variable 'cuont'","file":"app.esk","line":12,"col":20,"end_line":12,"end_col":25,"hints":["did you mean 'count'?"],"notes":[]}
{"severity":"warning","id":"format-expects-string","message":"format '%s' expects a string, but argument 2 has type 'int'","file":"app.esk","line":8,"col":11,"option":"-Wformat","hints":[],"notes":[]}
```

## Format

Each line is an object with these members, always in this order:

| Member | Type | Present | Meaning |
|---|---|---|---|
| `severity` | string | always | `"error"`, `"warning"` or `"note"` |
| `id` | string | always | The kind of diagnostic, a short kebab-case name (see Ids) |
| `message` | string | always | The text form's message, without the hint and the option |
| `file` | string | when located | The file as the text form names it (as given on the command line, or the resolved path of an import) |
| `line`, `col` | number | when located | 1-based line and byte column |
| `end_line`, `end_col` | number | when known | The end of the token at `line:col`; `end_col` is the column just past it |
| `option` | string | warnings with one | The flag that controls the warning (`"-Wformat"`): the text form's ` [-Wformat]` suffix |
| `hints` | array of strings | always | Suggestions: the text form's ` (did you mean 'x'?)` suffix as `"did you mean 'x'?"` |
| `notes` | array of objects | always | Lines that belong to the diagnostic: a `note:` line after it (`{"message", "file", "line", "col"}`, the location when it has one) or a line of continuation text (`{"message"}`) |

The range is the token the position points at: an identifier or keyword, a number, or a
string literal (quotes included). When the position is on punctuation, or past the end of
its line, there is no `end_line`/`end_col`. A location-less diagnostic (a driver error
such as `could not open file 'x.esk'`) has no `file`, `line` or `col`. A line on stderr
that is not a diagnostic and does not follow one becomes
`{"severity":"note","id":"output","message":...}`. Strings are escaped as JSON requires
(`\"`, `\\`, `\n`, `\t`, `\r`, other control characters as `\u00XX`); other bytes are
written as they are (the compiler's messages are UTF-8).

The JSON form has the same diagnostics as the text form, in the same order, and nothing
else: the summary lines of the text form ("Parse failed!", "error: parse failed", "error:
FILE: type checking failed") are left out. With `-Werror` a warning is reported as an
error and its `severity` is `"error"`. The exit code is the same in both forms. Output of
the programs the driver runs (the C linker, a `run` script) is not converted.

## Ids

An id names the kind of a diagnostic and is the same for every instance of it, whatever
names, types or numbers the message carries. It is made from the message with one rule,
so no table of ids has to be kept next to the hundreds of places that raise diagnostics,
and the two compilers give the same id for the same message:

1. A message that starts with a context, `initializing 'x': `, `In struct 'S': ` or
   `In union 'U': `, loses it.
2. The message is read up to its first `:`, `;`, `,` or `(` outside quotes.
3. A quoted part (`'...'`, a quote that does not follow a letter or digit up to a quote
   not followed by one) is dropped: it is the name, type or value of this instance.
   Backquotes are dropped and their text kept (`` `await` `` is the word `await`).
4. Of the words that remain, a trailing `'s` and a leading `#` are removed; a word is kept
   when it is letters, or letters joined by `-` (`read-only`); numbers, types (`*int`,
   `P<int>*`) and symbols are dropped, and so are `a`, `an` and `the`.
5. The first five kept words, in lower case, joined by `-`, are the id. A message with no
   such word has the severity as its id.

So `undefined variable 'cuont' (did you mean 'count'?)` is `undefined-variable`,
`argument 1 type mismatch: expected int, got string` is `argument-type-mismatch`, `struct
'A' has no member 'x'` is `struct-has-no-member` and `#else after #else` is
`else-after-else`. The scheme lives in `diagnosticId` (`diag_json.cpp`) and `diag_id`
(`selfhost/diag_json.esk`).

An id is stable as long as the fixed words of its message are: the compiler's tests pin
both. [`diagnostic-ids.txt`](diagnostic-ids.txt) lists every id the test corpus
(`tests/errors/`, `tests/warnings/`, `tests/diag/`) produces, and
`tests/selfhost/diag_parity.sh` fails when that set changes, so a reworded message that
changes an id is a visible change (`UPDATE=1 tests/selfhost/diag_parity.sh` rewrites the
list). A tool should match on `id`, and show `message`.

## How it works

Every diagnostic is printed in its text form where it is raised (the lexer, the
preprocessor, the parser, the type checker, the driver). The JSON form converts those
lines, which is how it carries the same set:

- **C++** (`diag_json.cpp`): `installJsonDiagnostics`, called by `compilerMain` after
  option parsing, replaces `std::cerr`'s buffer with one that hands each complete line to
  the converter, which writes JSON to the real stderr. A diagnostic is held until the next
  one starts (notes may follow it) and written by `flushJsonDiagnostics` at the end of
  `main` (and at `exit`).
- **Self-host** (`selfhost/diag_json.esk`): the self-hosted compiler prints with
  `dprintf(2, ...)` from many places, so `main` (in `esk_main.esk`) forks when it sees
  `--diagnostics=json`: the child runs the compiler with fd 2 on a pipe, and the parent
  converts what it reads (`dj_line`, the same steps as the C++ converter), then exits with
  the child's code. A program the child runs (clang, a `run` script) gets the real stderr
  back (`diag_json_restore_stderr` in `spawn_wait`).

A line is a diagnostic when it is `error: `, `warning: ` or `note: ` followed by an
optional `FILE:LINE:COL: ` and the message, or `FILE:LINE:COL: warning: message` (the
form of warnings).

## Tests

- `tests/diag/NAME.esk` + `NAME.expected` in `tests/run.sh` ("Diagnostics as JSON"): the
  JSON stderr of a build with the flags on the file's `// DIAG-FLAGS:` line.
- `tests/selfhost/diag_parity.sh`: both compilers print the same JSON, byte for byte,
  over `tests/errors/`, `tests/warnings/` (no flags, `-Wall -Wextra`, `-Werror -Wall`)
  and `tests/diag/`; the JSON has one object per diagnostic of the text form; the ids are
  those of `diagnostic-ids.txt`. Files whose text form already differs between the two
  compilers (a type spelled `int*` for `*int`, an instantiation context as a note) are
  listed in the script's `TEXT_DIFFERS`; a listed file that comes to agree fails the gate
  until it is taken off the list.
