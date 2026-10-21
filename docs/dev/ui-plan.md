# Plan: trailing blocks and a UI framework (0.13)

A cross-platform UI framework for Eskiu: components written once, rendered as native views on
iOS and Android, as the DOM on the web (WebAssembly), and in a webview on the desktop. The
view is ordinary Eskiu code; the only language change is a general one, trailing blocks.
Target release 0.13, no date. Compiler work goes on a branch off `develop` and merges when its
gates are green; the framework is a package, [`eskiu-lang/ui`](https://github.com/eskiu-lang/ui).

The runtime model (reactivity, ownership, identity, renderer protocol) is specified below and
settled in phase 0 against its test cases before the core is written in phase 1.

Phase 0 is done: the prototype lives in [`eskiu-lang/ui`](https://github.com/eskiu-lang/ui).
Shared Eskiu components run on the web, on iOS (UIKit) and Android
(`android.view`), both with Yoga, and in a macOS WKWebView host. The runtime-model tests pass
with the C++ and the self-hosted compiler, at `-O0` and `-O2`, under AddressSanitizer and
with every allocation counted. Its README records the executed matrix, the measurements and
the prototype's limits; "What phase 0 settled" below lists the decisions it changed in this
plan.

## What exists and what is missing

Already in place: wasm32 through emcc, iOS, Android, Windows GUI builds, closures with escape
analysis, struct literals with field defaults, generics with constraints, interfaces, async,
`jni`, the `gpu` and `font` packages, and `[native.NAME]` prebuilt libraries (Yoga, the
WebView2 loader).

Missing:

| Piece | Where it lives |
|---|---|
| Trailing blocks | compiler, both parsers (desugar only) |
| Element tree, diff, mutation list | `ui` package |
| Signals, effects, memos, scheduler | `ui` package |
| Primitives (`View`, `Text`, `Image`, `Pressable`, `ScrollView`, `TextInput`, layout helpers) | `ui` package |
| Native renderers for iOS (UIKit) and Android (`android.view`) with Yoga layout | `ui` package |
| Web renderer (`ui.js`, `--js-library`) | `ui` package |
| Desktop host (webview running the web renderer) | `ui` package, per platform |
| Dev server with reload | `ui` package (tool written in Eskiu) |
| Native renderer on `gpu` for the desktop | later, after 1.0 |

## Trailing blocks

### The rule

A call statement followed by a block passes the block as its last argument, a lambda with no
parameters that returns `void`:

```
f(a, b) { BODY }    =>   f(a, b, void() { BODY });
f() { BODY }        =>   f(void() { BODY });
```

The parser desugars it; sema, codegen and the async lowering see an ordinary lambda, and
escape analysis applies as usual (a callee that keeps the block declares it `escaping`).
Desugared nodes keep the block's positions, so errors, hover and definition point into it.
`Name(args) {` at statement position is a parse error today in both compilers, so the rule
changes the meaning of no valid program.

### Grammar edges

- **Where.** Only an expression statement of a block (`parseStatement`'s call to
  `parseExpressionStatement`). Not the `for` initializer or step (the same function serves
  the initializer, so the rule is gated by a flag the statement path sets), not a
  subexpression, not a `let` initializer, not `return f() { }`.
- **Callee.** The statement's expression must be a call whose callee is a name (a function or
  a closure variable: sema checks the last parameter either way), a member chain ending in a
  dot-call (`a.b.m(x) { }`), or an explicit generic call (`f<int>(x) { }`). The `{` must
  follow the call's own `)`. Rejected with a located error: a parenthesized call
  (`(f(x)) { }`), a call of a call (`f(x)(y) { }`), a cast or any other expression before the
  block.
- **After the block.** No `;` is needed; one `;` right after the block is accepted (lambdas
  are written `};` today and Eskiu has no empty statement, so this avoids a trap). The next
  token starts a new statement.
- **The block is another function.** `return;` returns from the block; `return x;` is an
  error (the block returns `void`); `break` and `continue` do not reach a loop outside the
  block ("outside of a loop or switch"); `await` is an error unless lambdas become async (the
  current diagnostic adds a spurious "await expects a *Future<T>, got *Future<int>" after the
  first error, fixed in phase 2); `?` is an error; a `defer` runs at the end of the block.
  Captured variables are read-only inside it, as in any lambda.
- **Not in the first version:** blocks with parameters, blocks in expression position.

It is not UI-specific: `with_lock(&m) { ... }`, `measure("load") { ... }`,
`test("parses csv") { ... }` read the same way (Ruby blocks, Swift and Kotlin trailing
closures).

### A view with it

```
void TodoApp(*Store store) {
    Column(Style{gap: 8, padding: 16}) {
        String pending;
        defer String_free(&pending);
        String_format(&pending, "%d pending", store.pending());
        Text(pending.data);
        for (item in store.items) {
            Row(Style{key: key(item.id)}) {
                let editing = use_signal<bool>(false);
                Checkbox(item.done, void(bool v) { store.toggle(item.id, v); });
                Text(item.title);
                Button("Edit", void() { editing.set(true); });
            }
        }
        if (store.items.size == 0) {
            Text("Nothing to do");
        }
        Button("Add", void() { store.add("New task"); });
    }
}
```

Components are functions; primitives come from `ui`. Control flow is plain `if`/`for`/`match`.
Props are struct literals with field defaults. Events are lambdas. State lives in
`*Signal<T>` handles, which a captured pointer can read and set. `ui` keeps the current
builder, so no context parameter is threaded through.

`Text(string)` takes finished text and copies it. Formatting goes through `<string>`'s
`String_format`, which `-Wformat` already checks (the checker knows a fixed table of names:
`formatIndexOf` in `sema/typecheck_expr.cpp`, mirrored in `selfhost/sema_assign.esk`). A
printf-style `Text(fmt, ...)` would not be checked, and untagged varargs cannot be verified at
run time; a general `format` attribute that would let a library function join that table
extends the language and is out of scope for 0.13. Small typed helpers (`NumberText(label,
n)`) cover the common cases.

## Runtime model

### Scopes and slots

Every element call with a block creates a **scope**: the element's node, its current block
closure, the state slots created inside it, the signals it read during its last run, and its
children. Running a scope means calling its block with the scope as the current builder; the
calls inside emit children into it.

State belongs to the scope whose block creates it, by call order (the hooks rule): a block
must create the same slots in the same order on every run. Each slot records its kind
(`signal`, `memo`, `effect`) and a **type tag**: the address of a `static` local inside the
generic hook (`use_signal<T>` and friends), which is distinct per instantiation, so
`use_signal<int>` and `use_signal<string>` in the same slot differ (checked with the C++
compiler; the self-host is checked in phase 0). Debug builds compare count, kind and tag on
every run and abort on a mismatch with the diagnostic described under "Diagnostics".

A function that needs its own state wraps its content in an element (or in `Scope(k) { }`,
an element with no view).

### Signals, memos and effects

- **Signal.** `get()` returns the committed value and records a dependency of the running
  scope, memo or effect. `set()` behaves by phase (see the scheduler). `use_signal<T>` belongs
  to a scope; `signal_new<T>` belongs to the app's model (a store), outlives every mount,
  works with no runtime, and is freed by its owner with `Signal_free` after the UI that reads
  it is shut down. Freeing a signal detaches it from every reader.
- **Memo.** `use_memo<T>(fn()->T)` is computed on first read. When a dependency changes,
  subscribed memos (including their dependency chains) are validated before choosing the
  scopes to render; an unobserved memo remains lazy until its next read. Its dependencies are
  re-tracked on every computation. A memo is a dependency like a signal: a scope that reads a
  memo is dirtied only when the memo's recomputed value differs (`==` for primitives, an
  explicit equality function otherwise).
- **Hook callbacks.** A scope run replaces its memo and effect callbacks, freeing the old
  environments. Replacement invalidates the memo and schedules the effect conservatively,
  since captured values cannot be compared. Memos are pure; cycles are errors.
- **Effect.** `use_effect(fn()->void)` runs after the commit of every run of its scope (the
  run replaced its callback, whose captures may differ and cannot be compared), and after a
  commit when a signal or memo it read in its last run changed. Its dependencies are
  re-tracked on every run. It may register a cleanup through `on_cleanup(fn()->void)`, run
  before its next run and at unmount. Effects run parents before children, and within a depth in the order they were scheduled. An effect that
  should run only when chosen values change compares them itself, or uses a later
  `use_effect_on(deps, fn)`.
- **Unmount during an effect.** An effect whose write removes its own scope (or an ancestor)
  finishes its run; the removal happens in the next frame, and effects of a scope removed by
  a commit do not run after it (only their cleanups).

### Errors in components

A throw out of a block, handler or effect is not caught by `ui`: the program stops through
Eskiu's uncaught-exception reporter (`error: uncaught exception: T v`). Catching it around
the runtime does not resume it: a throw leaves the runtime mid-phase (builder, tracking and
phase set), and the next `ui_frame` or `ui_event` stops with "runtime left mid-phase". Error
boundaries that unmount a failing subtree and render a fallback are a later addition; they
need the runtime to unwind its own state, which the `try`/`finally` support allows.

### Scheduler

The scheduler keeps two sets:

- `current`: the scopes dirty for the frame being rendered. A scope leaves it when the
  scheduler starts running it, or when an ancestor's run re-runs it.
- `next`: the scopes dirtied while the frame runs. Running a scope never removes anything from
  `next`.

A frame: swap `next` into `current` (empty `next`), run the scopes of `current` in tree order
(the **render phase**), commit the mutation list, wait for the commit to complete (see the
protocol), free what the commit retired, then run effects (the **effect phase**). If `next`
is not empty, schedule another frame.

Writes by phase:

| Phase | `set()` applies | A later `get()` in the same phase sees | Dirties |
|---|---|---|---|
| Event handler (between frames) | at once | the new value | readers into `next` |
| Render phase | at the end of the render phase, in write order | the old value (every scope of a frame renders one snapshot) | readers into `next` |
| Effect phase | at once | the new value | readers into `next` |

So a scope dirtied during the render phase, including by itself or by a descendant while it
is running, renders again in the next frame and never loses the invalidation. Debug builds
abort when one scope is dirtied by render-phase writes in 100 consecutive frames (a render
loop), naming its scope path.

### Re-running

A parent's run emits its children again. Each emitted child is matched to an existing child
(see identity). A matched child keeps its node and state; its block closure is replaced by
the newly emitted one and the child re-runs in the same frame, since its captures are values
taken from this parent run and cannot be compared. An unmatched old child is unmounted; an
unmatched new one is mounted. A `memo` element that skips the re-run when its props compare
equal is a later addition.

Test cases, each with its expected mutation list and per-scope run counts:

1. A `Row` whose block reads signal `a`; `a` changes in a handler: only the `Row` runs (count
   1), one `set_text`, the parent's count stays 0.
2. Parent reads `a`, its child `Row` reads `b`; a handler changes both: the parent runs once,
   the child once (as part of the parent's run).
3. Parent re-runs with a new value captured by the child's block: the child's node id is the
   same, its `use_signal` value is kept, its text shows the new capture.
4. A block writes a signal it does not read: the write is visible in the next frame, one run
   per frame, `get()` after the `set()` in the same run returns the old value.
5. A child writes, during its run, a signal its parent read: the parent is in `next` and runs
   in the following frame, once; the child runs once in each of the two frames.
6. A block writes a signal it reads, unconditionally: the debug build aborts after 100 frames
   with the scope path.
7. A memo read by two scopes changes to an equal value: neither scope runs.
8. An effect writes a signal that unmounts its own scope: its cleanup runs once, it does not
   run again.
9. Two events arrive while a commit is in flight: both are queued, then run in order after
   completion with the handler current then; a queued event with a stale generation is
   dropped.
10. A model signal (`signal_new`) is set with no runtime, read by a mount, survives its
    shutdown and a second mount, and is freed by its owner with no allocation left.
11. A block throws: the program stops through the uncaught-exception reporter; a caller that
    catches the throw and calls `ui_frame` again stops with "runtime left mid-phase".

### Ownership

| Resource | Owner | Freed |
|---|---|---|
| Node | its scope | when the commit that removes it completes |
| Block closure (heap env) | its scope | when replaced, after the replacing run; at unmount |
| Event handler closure | the node's handler slot | when replaced, after the commit that installs the new one completes; at unmount |
| Strings passed to `Text` and props | a copy made only when the text changed | the old copy when the commit that replaces it completes (the diff compares both) |
| Event payloads (text of an input, key) | the core | when the handler returns |
| Owned string state (`use_string`) | the slot and explicitly retained snapshots | each reference released on replacement/unmount or by the snapshot owner; storage freed at the last release |
| Generic `use_signal<T>` values | the slot holds T by value; pointers are borrowed | slot freed at unmount; no implicit recursive destructor for T |
| Model signals (`signal_new`) | the app's model | by its owner with `Signal_free`, after the UI that reads it is shut down |
| Signal subscriptions | the signal, as (scope id, generation) | re-tracked on every run; stale entries are skipped and dropped |
| Effect cleanups | the scope | after running, before the effect's next run; at unmount |

The mechanism behind "no callback reads freed memory": the `ui` API never hands arena memory
to user code. `Text` and props copy their strings into the arena, and no `ui` function
returns a pointer into it. Event payloads are valid while the handler runs; a handler that
keeps one copies it (`String_from`) or stores it in owned state (`use_string`). A string the
user creates during a run is the user's, as in any Eskiu code: a handler that captures it
needs it to outlive the run, so it lives in owned state or in the user's model (`*Store`),
not in a local freed by a `defer`. Keeping a string in owned state alone does not keep an old
value alive after `set()`: an escaping callback retains a snapshot and releases it with the
handler's cleanup, or captures the state handle and reads the current value when called.

Checks:

- Leaks: a counting allocator returns to its baseline after mounting and unmounting the TODO
  app 10 000 times, and after 10 000 renders that replace every handler.
- Freed memory: the model tests also run under AddressSanitizer (as the stdlib fuzzers do),
  every generated function instrumented. A test renders a row whose handler captures a string
  kept in `use_string`, replaces the row's strings by re-rendering, then invokes the handler: it reads the right text and ASan reports
  nothing. A second test makes the handler read an event payload kept past its handler without
  a copy; under ASan it must report, which proves the check sees that mistake.

### Identity and keys

- **Representation:** the props' `key` field is a `Key`, a struct `{bool set; int64 value}`
  built by `key(v)`; the zero `Key` (an omitted field) means "no key". So `key(0)` is a key and
  omitted keys never collide. String keys are a later addition.
- **Scope of a key:** the siblings emitted by one run of one parent. The same key under two
  parents names two different nodes; moving an item between parents remounts it.
- **Unkeyed children** match by position among the unkeyed siblings of their own kind, so an
  element that comes and goes does not move its siblings of other kinds (found when a modal
  opening before a list remounted it).
- **Keyed children** match by key among keyed siblings; a moved keyed child keeps its node and
  state and produces a `move` mutation.
- **Same key, different kind:** the old child is unmounted (its state freed) and a new one
  mounted.
- **Duplicate keys:** a debug build aborts naming both scope paths; a release build treats the
  later one as unkeyed.

Tests: a list with `key(0)` and other keys reorders correctly; a list of unkeyed elements
next to keyed ones reports no duplicate; in the TODO, reordering rows keeps each row's
`editing` signal and node id, and deleting a row unmounts it and frees its handlers, block
env, slots and subscriptions (counting allocator).

### Diagnostics

The desugared lambda carries no source position to the `ui` package, and default parameter
values are folded where they are declared, so `__LINE__` there would name the declaration.
For 0.13 the runtime reports a **scope path**: each step is the element kind (a primitive's
name, or the component name given by `Scope("Name", k, block)`), its key or sibling index;
a hook error adds the slot index and the expected and found hook, for example
`hook kind/type changed at slot 0: expected use_signal, found use_memo` /
`in Column[0] > TodoRow[key 3]`. The type of a hook is compared through its tag but cannot be
named (there are no type names at run time), so a type change reads "use_signal with another
type". Source positions (call-site defaults such as
`__LINE__` evaluated at the call, or the compiler stamping blocks) are a later language
decision, not part of 0.13.

## Renderer contract

### Mutation protocol

- **Commits.** Each frame's mutations form one commit with a sequence number. At most one
  commit is in flight; the next frame waits until the previous commit completes.
- **Completion.** `apply(commit)` either completes before it returns (iOS, Android and the web
  renderer, which run on the UI thread and call into JavaScript synchronously) or returns
  after copying the buffer and later reports `complete(seq)` (desktop webviews, whose bridges
  are asynchronous). The buffer may be reused as soon as `apply` returns, in both cases.
- **What waits for completion:** reuse of the ids a commit removed, freeing of the nodes,
  handler closures and arenas it retired, and the effect phase. A commit not completed within
  a timeout (5 s in debug) is reported through the error callback.
- **Ids.** Node ids are `uint32` with a generation; an id is reused only after the commit
  that removed it completes.
- **Operations:** create (kind), set property, set text, insert before, move, remove, listen,
  unlisten. Each carries the target id.
- **Events** come back as (id, generation, event kind, payload). The core owns the queue: an
  event that arrives while a commit is in flight, or during a render or effect phase, is
  queued (its payload copied on receipt) and dispatched in arrival order once the commit
  completes and the effects have run; otherwise it is dispatched at once. Hosts deliver
  events as they come and never hold them. The core drops an event whose generation is stale;
  for a live node it calls the handler current at dispatch time, so an event queued before a
  handler was replaced calls the new one.
- **Errors.** A renderer reports an error (unknown kind, unknown id) through a callback; debug
  builds abort, release builds log and skip the operation.

### Platform guarantees

**Root insets (configurable host policy).** The production host should respect safe content
insets by default, with an explicit `edge-to-edge` option for apps that draw behind system
bars. Android must distinguish system bars and display cutouts from IME insets; keyboard
avoidance is a separate option. Report the insets to the root so native and webview hosts can
apply the same policy. The current Android PoC draws behind the status bar; this is accepted
for phase 0 and remains a renderer-phase task, not a blocker for the prototype. Test the
default and edge-to-edge modes, rotation, cutouts, gesture/button navigation and keyboard.
See Android's [edge-to-edge guidance](https://developer.android.com/develop/ui/views/layout/edge-to-edge).

| Area | iOS | Android | Web | Desktop (webview) |
|---|---|---|---|---|
| Layout | Yoga | Yoga | CSS flexbox with Yoga's defaults (as react-native-web) | as web |
| Text measurement | Yoga measure callback over UIKit text layout | over `android.text` layout | browser | browser |
| Focus | first responder, `focus()`/`blur()` | `requestFocus` | DOM focus | DOM focus |
| Text editing and selection | `UITextField`/`UITextView` | `EditText` | `input`/`textarea` | as web |
| Keyboard | soft keyboard insets reported to the root | `WindowInsets` | none | none |
| Accessibility | role and label to UIAccessibility | to `AccessibilityNodeInfo` | to ARIA | as web |

Measured criteria: a conformance suite runs the same scripted scenarios on every renderer and
compares (a) a layout dump (each node's frame) within a pixel tolerance, (b) the event
sequence for a script of taps, typing, focus changes and selection, (c) the accessibility tree
(role, label, order). Frame-time budgets come from phase 0 measurements on a reference device
per platform and are checked from then on.

## Phases

### Phase 0: prototype and model (done)

- `ui` repo; examples written with explicit lambdas (`Column(s, void() { ... });`), the form
  trailing blocks will produce.
- Counter and TODO on the web with a minimal `ui.js`, and the counter on iOS with UIKit and
  Yoga, to check native views from Eskiu early.
- Write the runtime-model test cases above (scheduler cases 1 to 11, ownership and
  freed-memory checks, identity tests, the slot tag check in the self-host) against a
  recording renderer; adjust the model until they pass.
- Exit: both examples work, the model tests pass, frame budgets measured.

Result: met, with Android and a macOS WKWebView host beyond the plan. Baselines on an arm64
Mac: 1,000 rows re-rendered in 2.3 ms (p95, recording renderer), 100 taps batched into one
frame on the iOS simulator (0.13 ms) and the Android emulator (1.4 ms), a 48 KB wasm module.

#### What phase 0 settled

- **Effects** run after every run of their scope, not only on dependency changes (above).
- **Model signals** (`signal_new`): a store's signal must not belong to a scope, or a handler
  that outlives the scope writes freed memory.
- **Event queue** in the core, not in each host.
- **Native hosts** are thin platform shims (Objective-C++ on iOS, Java plus a JNI shim on
  Android) with a C API toward the Eskiu core; components and state stay in Eskiu. Calling
  UIKit through `objc_msgSend` from Eskiu would need the exact prototype of every message on
  arm64, which is fragile; the shims stay small.
- **Errors in components** stop the program (above).
- **Text** takes finished text; formatting goes through `String_format` (above).

#### Left for phase 1

The items phase 0 left are listed under phase 1 with their state.

### Phase 1: the core (done)

- Element tree, keyed diff, mutation buffer, scheduler, signals, memos and effects, as
  specified above.
- Host tests with the recording renderer; the leak gates in CI.

Done (`eskiu-lang/ui`, checked on the host at `-O0`/`-O2`, under ASan and with the
self-host, and on web, the iOS simulator, the Android emulator and macOS):

- **Linear core.** An id-to-node table and work lists (dirty scopes, memos, effects), so a
  frame touches only what changed; unmount walks the node's own slots. Children match
  through a key table (up to 8 children by scanning, which keeps small lists as fast as
  before); a reorder moves only the children outside a longest increasing subsequence of
  their old positions; subscriptions are edges indexed on both sides, removed in O(1). On
  10,000 rows: unmount 4,133 to 6.8 ms, reverse 256 to 5.9 ms, an event and its frame 230 to
  5.4 ms, mount 124 to 8.1 ms; a signal read by every row changes in 1.2 ms (was 92).
- **Packed commits.** One byte buffer per frame (format in `src/ui.esk`), decoded by
  `hosts/ui_commit.h` on iOS and Android and from wasm memory in one call on the web;
  properties are generic `prop` records.
- **Id reuse** after the removing commit completes, with a new generation per reuse.
- **Diagnostics** with scope paths, and `Scope(name, key, block)` for component names.
- **Release fallback** for duplicate keys (`ui.lenient`).
- **No run arena.** A string is copied only when it changes and retired after the commit,
  which allocates less than an arena per run; freed-memory checks are the ASan gates.

Found on the way, two compiler bugs, worked around in `ui` while the compiler fix is
under way: a lambda inside a generic function body always got a heap environment, even
when passed to a non-escaping parameter, and nothing freed it, so every `sort<T>` call
leaked 16 bytes (the core sorts with its own counting sort); and the self-hosted compiler
emitted invalid IR for `free_closure` of a variable captured by a lambda (the core frees
closures through a parameter).

Then, on every host (web, iOS, Android, and the web renderer in macOS WKWebView):

- **Typed events** (press, change with text, focus) through a core queue that copies the
  text of an event received during a commit.
- **TextInput** (controlled, `Input{value, placeholder, label, focused}`), **Scroll**,
  **Image** (bundled file or http(s) URL), and layout props `width`, `height`, `grow`,
  `wrap`; `label` is the accessible name on every element.
- **Insets and lifecycle**: hosts report system bars, cutouts and the keyboard
  (`ui_insets()`) and the foreground state (`ui_is_active()`); by default the root sits in
  the safe area and above the keyboard, `ui_edge_to_edge` hands the whole screen to the app.
- **Conformance suite** (`tools/conformance.py`): one scene, its frames (within a point),
  roles and event log compared on iOS, Android and the web; all three pass. The web
  renderer sets Yoga's defaults (border-box, no shrink, zero minimum size) for it.

Still basic, for later phases: IME composition is left to the platform controls (no test
of its own), accessibility covers names and roles but not reading order or actions, the
lifecycle reports foreground state but does not restore state after an Android activity is
recreated, and there is no Windows or Linux host yet (phase 4).

### Components (React Native's core set)

Taken in groups after phase 1, each on every renderer with conformance cases:

1. **Done.** Visual style props (StyleSheet's role: background, color, font size and weight,
   border, radius, opacity, align, justify), `Pressable`, `Switch`, `ActivityIndicator`,
   window size and scale (Dimensions, PixelRatio). Controlled inputs: a refused edit or
   toggle is sent back.
2. **Done.** Lists and overlays: a virtualized `FlatList` (mounts only what is near the
   viewport; fixed row heights, as `getItemLayout`) and `SectionList` over it,
   `RefreshControl` (native on iOS; on Android, whose framework has no pull to refresh, a
   drag at the top with an overlapping indicator; on the web, wheel or drag), `Modal` (a
   layer over the screen with its own layout; back and Escape ask it to close), `Alert`
   and `Linking` (commands in the commit, op 10), `StatusBar`, `use_back`.
3. **Done.** A component kit, `<ui/kit>` (Dioxus's components as a reference): a theme,
   `Card`, `Badge`, `Avatar`, `Separator`, `ProgressBar`, `KitButton`, `TextField`,
   `Checkbox`, `RadioGroup`, `SegmentedControl`, `TabBar`, `Toolbar`, `NavBar`, `ListItem`,
   `Collapsible`, `Accordion`; `ui_own` frees the closures a component keeps.
4. **Done.** Native modules and views: code per platform in `native/{web,ios,android}` of the
   app and of every package in the lock (Objective-C, Swift, Java, Kotlin, C, JavaScript);
   `use_native_event` and `ui_native_emit` (any thread) for modules, `NativeView` with named
   props (op 11) and events for views, a registry in each host. `ui.toml` (kept apart from
   `eskiu.toml`, whose reader rejects unknown tables) gives the app's name, id and URL scheme,
   frameworks, permissions and plist entries; Maven dependencies or Kotlin build through a
   generated Gradle project, Swift packages or Swift through a generated Xcode project.
   Checked by `tests/web.mjs` and `tools/check_native.py` on iOS and Android.
5. **Done.** Navigation: a `Router` (a stack of routes owned by the model) and `Navigator`,
   whose screens (kind 14) stay mounted under the top. On iOS and Android pushes and pops
   slide, with the back button and the iOS edge swipe; on the web the first navigator follows
   the URL and the history (command 3, event 10), under a configurable `<base>`. Links open
   their route. A removal is now sent before its descendants, so a leaving screen keeps its
   content while it slides out.
Next, in this order (phase 2 goes last: trailing blocks change no API, as long as a block
stays the last parameter of everything added meanwhile):

6. **Done.** Core primitives: absolute positioning and `aspect_ratio` (Style), `ui_on_layout`
   (the frame after layout), timers (`use_interval`, `use_timeout` over the host's clock),
   `ui_on_hover`, `Slider` (kind 15, controlled). In the conformance scene on every host.
7. **Done.** Core APIs (React Native's APIs list), in `src/apis/` (part of `<ui>`), through
   the hosts: the platform; appearance (`ui_color_scheme`, a dark kit theme and
   `kit_follow_system`; Android no longer recreates the activity on configuration changes);
   accessibility (screen reader, reduced motion, font scale, `ui_announce`); language and
   RTL (layout does not mirror yet); keyboard dismiss, share, clipboard, haptics (commands
   4 to 9); key-value storage and the data directory; `ui_fetch` through `NSURLSession`,
   `HttpURLConnection` and the browser's `fetch` (the system's TLS). In the conformance scene
   on every host, the fetch against a local server.
8. **Done.** Motion: transforms (`translate_x`, `translate_y`, `scale`, `rotate`) and
   transitions (`transition_ms`, `easing`) in `Style`, animated by each host
   (`UIView` animations, `ViewPropertyAnimator`, CSS transitions); `use_animated` for values
   the host cannot animate; `ui_on_pan` (a drag taken from a pressable inside past the slop);
   the kit's `SwipeToDelete`; the interactive iOS back swipe. Layout changes are not animated
   yet.
9. **Done.** The kit's overlays: `Popover` (placed from the trigger's window position, below
   or above, inside the window), `DropdownMenu`, `Select`, `MenuItem`, `Tooltip`, toasts
   (`kit_toast`, `ToastHost`), `Calendar`, `DatePicker`, `ColorPicker`; in the core,
   `Overlay` (an undimmed layer), a tap on a modal's backdrop, window positions in `Frame`
   and min and max sizes in `Style`.
10. **Done.** `ui serve` (phase 5): it watches what a build reads, builds again in a child
    process and reloads the page (web, the macOS window) through a long poll, showing a failed
    build's output over the app; on iOS and Android it installs and launches the app again.
11. Device packages, as Expo's modules, each in its own repository with its `native/` and
    `ui.toml`. **Done:** `eskiu-lang/ui-location` v0.1.0 (`use_location`; the core's
    `use_mount` for services that live as long as a component). Next `ui-notifications` and
    `ui-camera`, then sensors, secure storage and biometrics. Single-platform APIs (Android
    toasts and permissions, iOS action sheets) too.
12. Phase 2 and the 0.13 release.

The repository has its documentation in `docs/` (getting started, architecture, state,
components, style and layout, the platform, navigation, native code, protocol, hosts,
testing), and the site a page for it (`site/ui.html`).

### Phase 2: trailing blocks in the compiler (branch `trailing-blocks`)

Lockstep, one layer per commit:

1. C++ parser: parse and desugar at statement position (`parse_stmt.cpp`), gated off the `for`
   initializer and step, positions kept, the optional `;`.
2. Self-host parser: the same in `selfhost/parser.esk`; `parse_parity.sh` compares the printed
   AST byte for byte.
3. Located errors for each rejected form (block after a non-call, a parenthesized call, a call
   of a call, a block in a `for` header or an expression), `tests/errors/trailing_block_*`,
   matched in `tc_parity.sh`; the spurious second `await` error removed in both compilers.
4. Run tests (`tests/trailing_block*.esk`): a function, a closure variable, an `escaping`
   callee, a dot-call, a generic call, nesting, `defer` inside a block, `return;` from the
   block; error tests for `return x;`, `break`, `continue`, `await` and `?` inside it;
   `cg_parity.sh` and the `-O0`/`-O2` check.
5. Formatter (`fmt_parity.sh`), editor grammars and version, LSP cases in `tests/lsp/`.
6. Docs: spec section, book chapter, stability note, AGENTS row, CHANGELOG.

Then the `ui` examples move to trailing blocks.

### Phase 3: native renderers

- Yoga as `[native.yoga]` (prebuilt per platform), bound with `extern`.
- iOS: UIKit views through a thin Objective-C++ shim with a C API toward the core (settled in
  phase 0), events back through the same API.
- Android: `android.view` through a Java activity and a small JNI shim, in a template
  project; the `jni` package where Eskiu calls into the VM.
- Exit: the conformance suite passes on both.

### Phase 4: web and desktop

- Web: the mutation buffer applied by `ui.js` once per frame; one delegated listener back into
  Eskiu; primitives map to `div`/`span`/`img`/`input`; a wasm size budget.
- Desktop: the web renderer inside a webview (WKWebView on macOS, WebView2 on Windows,
  WebKitGTK on Linux), the buffer sent through each bridge.
- Exit: the conformance suite passes on web and macOS. A demo page on the site.

### Phase 5: developer loop

- `ui serve`: builds, serves, watches files and reloads over a websocket (`<http_async>`,
  `<fs>`); on mobile, rebuild and relaunch.
- Later: reload of a changed component without restarting the app.

### After 1.0

- Desktop renderer on `gpu` + `font` with Yoga layout, without a webview.
- Server functions for full-stack apps.

## Release criteria for 0.13

- Trailing blocks in both compilers with every gate green (parse, tc, cg, fmt, lsp parity).
- `ui` v0.1.0 with iOS, Android and web renderers and the macOS desktop host, each passing the
  conformance suite; the runtime-model tests and leak gates green.
- The TODO example on the site.

## Risks

- The native shims grow with every primitive (text input, scroll, images, accessibility);
  they must stay a translation layer, with behavior in the Eskiu core.
- Re-running every matched child on a parent re-run may cost too much in large lists before
  `memo` exists; phase 0 measured 2.3 ms for 1,000 rows without the layout cost.
- WebView2 over COM by hand on Windows.
- wasm size with exceptions on emscripten's JavaScript runtime; native wasm exceptions are
  already on the 0.13 list.
