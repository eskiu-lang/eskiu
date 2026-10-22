# Plan: recoverable exceptions in ui

## Goal

An error in a block, a handler or an effect must not close the app. The runtime always stays
consistent, and the app decides what to show. Today any `throw` nobody catches stops the
program, and a caller that catches it from outside finds the next frame stopping with
"runtime left mid-phase" (`ui_require_consistent`, `src/core/diagnostics.esk` in the ui
repository).

## Phase 1: a catch-all in the language (both compilers)

ui cannot know which types an app throws, and Eskiu catches by exact type only
(`catch (int e)`). The compiler already has an internal catch-all: `captureAll` in the async
lowering.

1. **`catch (_) { ... }`** catches any exception without binding it. It may stand alone or
   follow typed catches, always last; one before another catch is a located error.
2. **`exception_text()`**, a builtin valid only inside a catch, gives `"int 7"`,
   `"string boom"` and so on. It reuses the uncaught-exception reporter's logic
   (`emitUncaughtReporter` / `cg_emit_uncaught`). Outside a catch it is a sema error.
3. **One commit per layer:**
   - the C++ parser and the self-hosted mirror;
   - sema (the catch's position and order, uses of `exception_text`);
   - codegen (the clause that compares no type);
   - async: a `catch (_)` in a `try` split by `await`;
   - wasm: check that `-sDISABLE_EXCEPTION_CATCHING=0` is passed when there is a `try`.
4. **Tests:** run and error cases, parse, tc and cg parity, `-O0` against `-O2`, and a wasm run.
5. **Docs:** the spec's exceptions section, chapter 10 of the book, AGENTS and CHANGELOG.
6. **Version:** by the stability policy new syntax goes in a minor release, so **0.14.0**.
   Moving the 0.13.0 tag, as was done with ui, is the other option; it is decided before the
   release, since it is the compiler.

## Phase 2: a transactional ui runtime

**Render (`ui_run` in `src/core/tree.esk`).**
- `ErrorBoundary(fallback) { content }` is a `Scope` that wraps its block in
  `try` / `catch (_)`.
- On entry it keeps `ui.commit.len`. When the block throws:
  1. It cuts the commit buffer back to that length, so nothing of the failed pass is sent.
  2. It frees the nodes made in that pass without emitting them, and their ids go back to
     `free_ids`.
  3. It unmounts the boundary's previous subtree, which the host does have, emitting its
     removal, so no node is left with a state other than the host's.
  4. It restores `ui.builder`, `ui.tracking` and the hook cursors.
  5. It mounts the `fallback`.
- A boundary showing its fallback does not retry on its own. `ui_boundary_reset(id)` or a
  change of its `key` tries again.
- **Without a boundary** the root is an implicit one, with a default fallback the app can
  replace.

**Handlers (`ui_dispatch_event` and the navigation ones).** Catch, report and go on. Writes to
signals made before the `throw` stay, as in JavaScript.

**Effects (`ui_run_effects` in `src/core/runtime.esk`).** Catch per effect and report. An
effect that fails registers no cleanup, and the others run.

**Phases.** Each entry point (`ui_frame`, `ui_complete`, `ui_post`, timers, fetch and native
events) restores `ui.phase` in a `finally`. `ui_require_consistent` stays as the safety net for
internal errors.

**Reporting.**
- `ui_on_error(fn(UiError)->void)`, where `UiError` carries where it happened (render, handler
  or effect), the component's path (the one the diagnostics already build,
  `Column[0] > TodoRow[key 3]`) and the exception's text.
- By default it prints to stderr or to the console.
- Under `ui serve` it shows the overlay over the app, with the component's path, as compile
  errors show today.

## Phase 3: tests and measurements

- **Model (`tests/model`):**
  - a throw in a render under a boundary: the fallback arrives, no mutation of the failed pass
    does, the ids are reused;
  - a throw nested in a grandchild;
  - a throw in a handler and in an effect;
  - a retry with `ui_boundary_reset`;
  - a throw with no boundary, and the root's fallback;
  - a throw halfway through a keyed list.
- **Memory:** `tools/check.py` with allocation counting and ASan, with nothing leaked and
  everything balanced after failing and recovering.
- **`tests/rejections.esk`:** mode 6 goes from "runtime left mid-phase" to recovering. A case
  for an internal error that must still stop is added.
- **Host conformance:** iOS, Android and the web receive only valid commits after an error.
  The kit gallery shows a boundary.
- **Speed:** the benchmark (`tests/benchmark.esk`) natively and in wasm. Under emscripten calls
  inside a `try` go through JavaScript wrappers, so only boundaries, event dispatch and effects
  get a `try`, not every `Scope`. Target: under 2% on the 10,000-row benchmark.

## Phase 4: docs and release

- **ui docs:**
  - `state.md`: rewrite "Errors in handlers";
  - `components.md`: `ErrorBoundary`;
  - `platform.md`: `ui_on_error`;
  - `testing.md`.
- **Site:** a line in `ui.html` and the component in `ui-components.html`.
- **Release:** ui v0.2.0 or a moved v0.1.1 tag, as decided. The packages and the starter are
  updated and the MCP index is rebuilt.

## Risks

- **Cutting the commit back** assumes that a boundary's pass touches only nodes of its
  subtree. A model test confirms it before anything builds on it. Anything else that writes to
  the buffer, such as commands, is kept apart as `ui.commands` is today.
- **Side effects that cannot be undone:** cleanups or `use_mount` callbacks that already ran in
  the failed pass stay run. That is documented, as React does.
- **Exceptions crossing into the host:** catching them in Eskiu before returning to JNI or
  Objective-C is the point. Check that no host path calls the app's code outside the guarded
  entry points.

## Later: SSR and server functions (outside this plan)

1. Prerendering with `ui build web --prerender ROUTES`, on the native core and an HTML
   serializer.
2. SSR with hydration from an Eskiu server.
3. Server functions the tool generates.

Each step gets its own plan when it comes.

**Order:** phase 1, 2, 3, then 4, with commits per layer and CI green at each step. Before
starting: the compiler's version (0.14.0 or moving the 0.13.0 tag).
