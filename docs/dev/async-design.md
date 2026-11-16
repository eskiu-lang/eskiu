# Async / Await: Design & Contract

This note is the contract reference for Eskiu's async runtime: the `Future<T>`
ABI, the atomic four-state handshake, and the waker model. These are the parts
that are expensive to change once async functions exist in the wild (see "What is
locked, what is free"), so they are fixed here.

**Implementation.** The runtime model is built on `stdlib/future.esk` (the locked
`Future<T>`/`FutureHdr` contract and the §3 handshake), the atomic intrinsics
(`<atomic>`), and escaping closures (a waker/callback outlives its creating
function, so it needs a heap env: escape analysis + the `escaping` qualifier +
`free_closure`, spec §6.5). On top sit the Executor and leaf futures
(`<executor>`/`<net_async>`), the `async`/`await` frontend, and the
AST→state-machine transform (`sema/async_transform.cpp`). Supported: single and
multiple `await` (fast path + suspend over real reactor reads, values threaded
through frame fields across N+1 states), `return await`, bare `await`, `async
void`, cancellation, and all control flow around `await` (`if`/`while`/C-style
`for`/`do`-`while`/`switch`/`match`/`for-in`/`try`-`catch`, with `break`/`continue`),
with full closure-env ownership, leak-free under the macOS `leaks` tool. An await may sit anywhere in
an expression: a desugar hoists each one, in evaluation order, into a `let` of its own
(§4.5). A `defer` in a block split by an await is kept by the transform and emitted at
each exit of that block (fall-through, `return`, `break`/`continue`); a defer body or a
`finally` may await, and is then lowered into states at each exit (§4.6, §7). A lambda that
captures a frame-hoisted local gets a block-local copy (`T x = __fr.x;`, `__fr` being the frame pointer) at its
creation point, so the capture is still a by-value snapshot. Combinators (`spawn`/`select2`/`join2`,
generic + cast-free) and a `<timer>` leaf future for deadline-based timeouts build
on this shape.

**Audience:** compiler maintainers. Assumes familiarity with the existing closure
model (`fn(T)->R` is a fat pointer `{fn_ptr, env_ptr}` that captures by value),
monomorphic templates, the `<eventloop>` reactor, and `<threading>`.

---

## 1. Goals and non-goals

**Goals**

- `async` function and `await` usable for real network I/O (the stdlib HTTP servers
  need it): an async TCP read/connect that suspends without blocking a thread.
- **Cancellation / drop in v1.** A future can be dropped before it completes;
  doing so releases its resources (loop registrations, memory) and cascades into
  whatever it was awaiting.
- **Multi-threaded execution with thread-affinity in v1.** A future may be
  completed on one thread while its continuation must resume on another. The
  motivating case is a UI framework: background work completes on a worker
  pool, but the continuation that touches UI state must run on the UI thread. This
  dictates how completion and the waker work, so it is part of the locked contract,
  not a later bolt-on.
- A `Future<T>` whose shape and atomic protocol we will *not* break when we later
  add timers, channels, and `select`/`join` combinators.
- Reuse the existing compiler: closures (resume/drop continuations), monomorphic
  templates (`Future<T>`), AST-visitor passes. The state-machine split is an
  **AST→AST transform**: its output is ordinary AST that the type checker re-checks,
  it reuses existing codegen, and it needs no LLVM coroutine intrinsics.
- Manual memory, no GC, no hidden refcounting.

**Prerequisite this design pulls in**

- **Atomic intrinsics** (atomic load/store/CAS/swap with acquire/release ordering).
  Multi-thread completion needs a lock-free state handshake. LLVM exposes these
  directly (`cmpxchg`, atomic `load`/`store`), so codegen is tractable, and
  `<threading>` uses them independently. They underpin the §3 handshake.

**Non-goals (v1: deferred, each forward-compatible)**

- `select` / `join` combinators were deferred from v1. They have since shipped
  (`select2`/`join2`, plus the value-carrying `select2v`/`join2v` in `<futureval>`),
  built on this shape with no contract change.
- Cooperative cancellation (a coroutine that observes a cancel request and keeps
  running its normal code). Drop runs only the pending `defer`/`finally` code (§7),
  which may await; the forward path is a cancellation token, no contract change.
- Work-stealing / load-balancing across executors. The executor abstraction allows
  it later; v1 uses fixed thread affinity.

---

## 2. The runtime model: completion + waker, atomic, with drop

Two classic shapes: **poll-based** (Rust; inert futures, executor polls: payoff is
avoiding per-future allocation) and **completion + waker** (JS Promise / C# Task;
the future holds state + result + continuation).

**We choose completion + waker.** Our reactor already dispatches on readiness (a
completion event), we already heap-allocate frames (so poll's allocation savings
don't apply), and completion is simpler to *generate*: suspend points are explicit.

The shape, an ordinary stdlib template (`stdlib/future.esk`):

```eskiu
struct Future<T> {
    int        state;     // ATOMIC. 0 pending, 1 waiting, 2 ready, 3 cancelled  (§3)
    fn()->void waker;     // completion path: resume (or schedule) the awaiter
    fn()->void on_drop;   // cancel path: release own resources + cascade
    T          value;     // valid only when state == ready; LAST (§2.1)
}
```

A **type-erased header** aliasing the first three fields for any `T`:

```eskiu
struct FutureHdr { int state; fn()->void waker; fn()->void on_drop; }
```

`(FutureHdr*)f` lets cascade-drop, the executor, and any thread touch
`state`/`waker`/`on_drop` without knowing `T`.

### 2.1 Why `value` is last

`Future<int>` and `Future<string>` are distinct monomorphizations of different
sizes. If `value` sat before the other fields, `waker`/`on_drop`/`state` would land
at different offsets per `T` and no `FutureHdr` view would be possible; cascade-drop
and cross-thread dispatch would break. `value` **last** fixes the header offsets for
every `T`. Free today, a hard break later, hence locked.

### 2.2 Why `state` is atomic

In multi-thread, a completer (worker thread) and the awaiter (home thread) touch the
same future concurrently. Without atomicity there is a lost-wakeup race: the awaiter
decides to park and the completer signals readiness in between. The fix is a
lock-free handshake on an **atomic `state`** with four values (§3) and
acquire/release ordering so `value`, written before publish, is visible after the
reader observes `ready`. Atomicity is a property of the *accesses*, not an extra
field, but the four-value encoding and the ordering are part of the locked contract.

Why these four fields and only these (the full walk) is §5.

---

## 3. Protocols (the compiler↔generated-code ABI)

`state` is an atomic int: `PENDING=0`, `WAITING=1`, `READY=2`, `CANCELLED=3`.
`READY` and `CANCELLED` are terminal. All transitions are CAS/swap with
acquire/release; `value` is written before the publish to `READY` and read only
after observing `READY`.

### 3.1 Park (awaiter, on its home thread): `let x = await F;`

```
F.waker = <resume-me>;                       // publish continuation
if (CAS(&F.state, PENDING -> WAITING)) {
    frame.awaiting = (FutureHdr*)F; return;  // parked; executor will resume us
}
// CAS failed -> F is already READY (completer beat us): take the value inline
let x: T = F.value;  frame.awaiting = null;  Future_free(F);
// ...continue with x...
```

### 3.2 Complete (producer, any thread)

```
F.value = <result>;                          // write result first
F.on_drop = <just-free F>;                   // external resources already released
old = SWAP(&F.state, READY);                 // release
if (old == WAITING) F.waker();               // awaiter parked -> schedule its resume
// old == PENDING: awaiter not parked yet; it sees READY in its CAS and takes value inline
// old == CANCELLED: dropped first -> producer releases <result> and frees (§3.4 arbitration)
```

**Where the continuation runs.** `Future_complete` calls the waker on the
completing thread, and the waker the transform generates resumes the coroutine
directly (`fr.st = N; __f_resume(fr);`). Thread affinity is the executor's job: a
producer that completes a future on another thread hands the resume to the
awaiter's executor with `Executor_schedule`, which queues it and wakes that
executor, so the continuation runs on the executor's own thread (§6).

### 3.3 Drop / cancel (any thread): `Future_drop((FutureHdr*)F)`

```
old = SWAP(&F.state, CANCELLED);
if (old == READY) { Future_free(F); }        // completed already: just free memory
else { F.on_drop(); }                        // PENDING/WAITING: release + cascade + free
```

- A **leaf** future's `on_drop` deregisters its fd/timer from the loop, frees itself.
- A **coroutine** future's `on_drop` cascades: `if (frame.awaiting) Future_drop(frame.awaiting);`,
  runs the `defer`/`finally` code pending at the await it is parked at (§7), and the
  frame is freed. The coroutine does not continue past the await. When that cleanup
  awaits, `on_drop` first sets `state` to `DETACHED` (4) and the frame outlives
  `Future_drop` (§7.1).

### 3.4 Arbitration (the one invariant)

> A future is finalized **exactly once**. The atomic swap to a terminal state
> (`READY` or `CANCELLED`) has a single winner; the loser observes the terminal
> state as its `old` and performs only the free, never a second resource release.
> Memory is freed exactly once, by whichever path reached terminal.

This is the heart of the design. The contract locks the *requirements* (atomic
4-state, ordering, single-free); the exact CAS choreography lives in
`stdlib/future.esk`, exercised by a cross-thread test.

---

## 4. Lowering an `async` function to a state machine (AST transform)

```eskiu
async int fetch_len(EventLoop* lp, int listen_fd, *uint8 buf) {
    int fd = await net_accept_async(lp, listen_fd);        // await #1
    int n  = await net_read_async(lp, fd, buf, (int64)64); // await #2
    return n;
}
```

### 4.1 The frame

One struct per async function: embeds the return future (one allocation), resume state,
the `awaiting` back-pointer (cascade-drop), params, and locals
**live across an await**:

```eskiu
struct __fetch_len_frame {
    Future<int> ret;       // &frame.ret is the returned Future<int>*
    int         st;        // resume state
    FutureHdr*  awaiting;  // inner future currently parked on (null otherwise)
    EventLoop*  lp;        // param
    int         listen_fd; // param
    *uint8      buf;       // param
    int         fd;        // local hoisted to the frame
}
```

(Simplified: the transform also hoists await temporaries, and every synthesized
name is made unique against the function's own identifiers.) `awaiting` is a
*frame* field, not a `Future` field, free to adjust, no contract cost. Only one
resume of a frame runs at a time (it is parked between them), so the frame needs no
locking. The only cross-thread object is the `Future` header, synchronized per §3.

### 4.2 Constructor + resume

- **Constructor** `Future<int>* fetch_len(EventLoop* lp, int listen_fd, *uint8 buf)`: allocs the
  frame, stores params, `st=0`, `awaiting=null`, sets
  `frame.ret.on_drop = <cascade-drop awaiting, run pending cleanup>`, calls
  `__fetch_len_resume(frame)` once, returns `&frame.ret`.
- **Resume** `void __fetch_len_resume(__fetch_len_frame* f)`, shown as a `switch` for
  readability (the transform emits a state graph, `while (true) { if (st == N) {...} ... }`,
  so loops and branches that contain awaits can jump between states):

```
switch (f.st) {
case 0:
  Future<int>* g = net_accept_async(f.lp, f.listen_fd);
  f.waker_of_g = <closure capturing f: "f.st=1; __fetch_len_resume(f)">;
  // park via §3.1 against g; if g already ready, fall through with g.value
  ... if parked: f.awaiting=(FutureHdr*)g; f.st=1; return;
  f.fd = g.value; f.awaiting=null; Future_free(g);
case 1:
  Future<int>* h = net_read_async(f.lp, f.fd, f.buf, 64);
  ... (same park/fast-path against h, resume label 2) ...
  int n = h.value; f.awaiting=null; Future_free(h);
  // return n: complete our own future (§3.2), then the awaiter's waker frees f
  f.ret.value = n; f.ret.on_drop = <just-free f>;
  old = SWAP(&f.ret.state, READY); if (old==WAITING) f.ret.waker();
  return;   // do not touch f afterward
}
```

The continuations are existing closures capturing `f` by value: no new
machinery. (The §3.1 park sequence is emitted inline at each await; shown abbreviated.)

### 4.3 Fast path

The park's failed-CAS branch means an already-ready inner future does **not**
suspend: read value, fall through. Zero extra round-trips when nothing blocks.

### 4.4 Generic async functions

`async T f<T>(...)` lowers once, generically: the frame struct, the resume function,
and the constructor are templates over `f`'s type parameters (`__f_frame<T>`,
`__f_resume<T>`, `*Future<T> f<T>(...)`), instantiated per use like any generic. Each
awaited type must be spelled in terms of `T`. The C++ transform recovers it from the
type checker's per-instance records (`AwaitExpr::instanceTypes`: the instance's type
arguments and the awaited type), choosing a spelling that reproduces every checked
instance (`generalizeType`). The self-host pass resolves it from the source directly
(`al_await_type`). The other temporaries are typed the same way in both compilers: the
type checker records, per checked instance of a generic async function, every
expression's type (C++ `TypeChecker::instanceExprTypeMap`, self-host `ExprNode.ainst`)
and each `match` arm's binding types (`Arm::instanceBindingTypes`, self-host
`arm_inst`), and the lowering generalizes them (`generalizeType` / `al_generalize`). A
generic async function that is never instantiated is left as is.

### 4.5 Awaits inside expressions

Before the state split, a desugar (`hoist` / self-host `al_hoist`) moves every await,
in evaluation order, into a `let` of its own, so the lowering only ever sees
`let x = await E;`:

```eskiu
x += g() + await f();      // let __sp = g(); let __aw = await f(); x += __sp + __aw;
a[i()] = await f();        // let __sp = i(); let __aw = await f(); a[__sp] = __aw;
ok = c() && await f();     // let __sc = false; if (c()) { let __aw = await f(); if (__aw) { __sc = true; } }
while (await more()) B     // while (true) { let __aw = await more(); if (__aw) {} else break; B }
```

An operand written before an await is held in a temporary only when it has a side
effect (a call, an assignment, `++`/`--`); the temporary's type is the one the type
checker gave the operand (the C++ transform reads the checker's expression types and
spells them as a declaration would, `declType`; the self-host sema stamps
`ExprNode.aty` in async functions). An assignment target keeps its place and only the
parts that compute it (an index, a pointer) are held, so it is evaluated once. `?:`
with an awaiting arm needs its result type the same way. A `do`/`while` or `for` whose
condition or step awaits becomes a `while (true)` whose first pass skips the test or
the step, so `continue` still runs them. A `switch`/`match` subject, a `for-in` iterable
and a range bound are hoisted before their statement (evaluated once). In a generic
async function these types are spelled with the type parameters (§4.4).

### 4.6 `match` and `try`

A `match` whose arms await dispatches in the current state: each arm copies its
payload bindings to frame fields (typed by the checker's stamp, `bindingTypes` /
`arm_types`) and selects the arm's entry state.

A `try` whose body or handlers await is split into states grouped in two regions (the
body's and the handlers'). At the end of lowering, each state of a region is wrapped in
a synthesized `try`:

```
try { <state> }
catch (T x) { <defers pending in the region>; fr.e = x; fr.st = <handler state>; }
finally-on-unwind { <defers pending in the region>; <the try's finally> }   // then rethrow
```

innermost region first, so an exception thrown before or after a suspension reaches
the handler of the try it is thrown in whichever resume runs it. The `finally` of the
synthesized try (`TryStmt::unwindOnly`, self-host SK_TRY `is_err = 1`) runs only when
no catch matched. On the other exits the user's `finally` is kept like a `defer`:
`emitExit` runs it on fall-through, `return`, `break` and `continue`, in a state
outside the try's regions (an exception it throws is not caught by its own handlers),
and publishes a completion outside every region. Each state of a region keeps a fixed
set of pending defers (a `defer` registered inside one starts a new state), so the
handler runs exactly the defers pending where the exception was thrown.

A `finally` or a defer body that awaits is kept as written (not rewritten in place) and
lowered into states of its own at each exit (`runFrames` / `al_run_frames`), with the
cleanups under it still pending, so an await there suspends like any other and a
cancellation while parked in it runs the rest. On the exceptional path, when the pending
cleanups of a region await, the synthesized try gets a capturing catch-all instead of the
unwind-only `finally` (`CatchClause::captureAll`, self-host SK_CATCH `is_err = 1`): it
copies the exception object into a fresh unthrown one (`__cxa_allocate_exception` of the
size stored at offset 0 of every Eskiu exception, then a byte copy), keeps it in a frame
field of that region, and jumps to states of the enclosing region that run the cleanups
and throw the copy again (`ThrowStmt::rethrowCaptured`, SK_THROW `is_err = 1`). A typed
handler whose pending defers await jumps to states that run them before the handler. A
copy that is never thrown again (a cleanup that throws, or a frame dropped while the
cleanup runs) is released with `__cxa_free_exception` when the frame finishes.

---

## 5. Field walk: why `{state, waker, on_drop, value}` survives everything

| Await source | Completes via | Drops via | New field? |
|---|---|---|---|
| Socket readable (leaf) | loop callback reads, §3.2 | `on_drop`=`EventLoop_del`+free | no |
| Another `async` function | resume §3.2 | `on_drop` cascades `awaiting`, frees frame | no |
| Timer `await timer_after(lp, ms)` | loop timeout, §3.2 | `on_drop` cancels timer (`EventLoop_del_timer`)+free | no |
| Worker thread / pool | worker completes on its thread; the resume is scheduled on the awaiter's executor (§6) | `on_drop` detach+free | no |
| `await Chan_recv(ch)` | sender §3.2 | `on_drop` unlinks+free | no |
| **UI: bg work → UI-thread continuation** | worker §3.2; waker enqueues on UI executor | parent `on_drop` | no |
| `select2`/`join2` | first (or last) child §3.2 | parent drops losers | no |

- **No `error` field**: fallible async returns `Future<Result<T,E>>`; error rides
  in `value`, composes with `?`.
- **No `waker_ctx`/`dropctx`/`executor` field in `Future`**: closures capture by
  value, so the resume thunk (and any executor it targets) live in the waker's env.
- **No lock field**: the atomic `state` handshake (§3) replaces a per-future lock.

### 5.1 `Future<void>`

`async void f()` lowers internally to `Future<Unit>` (`Unit` = 1-byte `uint8`);
`await f()` discards the value. Invisible at source level.

---

## 6. Executors, the event loop, and thread-affinity

An **Executor** is a thread plus a thread-safe ready-queue and a wakeup mechanism
(a self-pipe registered with that thread's `<eventloop>`): `executor_new(lp)`,
`Executor_schedule(ex, waker)`, `Executor_run`, `Executor_stop`, `Executor_free`.

- `Executor_schedule` may be called from any thread: it enqueues a waker on the
  executor's ready-queue and wakes its loop. The executor's run loop pops ready
  entries and calls them on its own thread, so a resume scheduled there runs there.
- **I/O wake source.** `<eventloop>` is one source of completions: when an fd is
  ready, its leaf future completes (§3.2) on the loop's thread and calls the
  awaiter's waker there.
- **UI framework shape.** The UI thread runs an executor; background work runs on
  a worker thread; the worker completes the future and the continuation is marshalled
  back to the UI executor with `Executor_schedule`. This is the JS-main-thread /
  Swift-MainActor / Kotlin-dispatcher model, and it needs *no `Future` change*.

Cross-thread enqueue + wakeup and the ready-queue are **executor machinery, free to
evolve** (single-thread, fixed pool, later work-stealing). What is locked is only
that completion goes through `waker()` and `waker` is responsible for landing the
resume on the right thread.

---

## 7. Memory and cancellation: one invariant

> **Every future is finalized exactly once**: *awaited to completion* (the awaiter
> frees it), or *dropped* (`Future_drop` frees it via `on_drop`). The atomic swap to
> a terminal state picks the single winner (§3.4); the other path only frees.

One allocation per async call (frame + embedded return future), one per leaf future.
A future created but never awaited, handed to a combinator or `spawn`, or dropped
leaks: the caller owns it and must release it (`Future_drop`, or `spawn` for a
detached task). The transform does not insert an implicit drop.

**Closure locals.** Every local is a frame field, so a lambda bound to a local gets a heap
env: the closure must survive a suspension. The frame owns that env when the local is only
ever bound to lambda literals (its initializer, or a statement `f = <lambda>`) and is
otherwise only called, never named inside another lambda (`OwnedClosureScan` /
`al_owned_closures`). Each binding first frees the env the field held (the field starts
zeroed, and `free_closure` of a null env does nothing), so a loop that binds it on every
pass keeps one env alive. The last one is freed when the frame finishes, by the block that
also releases unthrown exception copies (`excFree` / `al_exc_free`): it runs before the
completion is published, at the end of a cancellation's cleanup (detached or not), and
from `on_drop` when the future is parked where no cleanup is pending. A closure local used
any other way (copied, passed on, captured) keeps the escaping rule: whoever ends up
holding it frees it with `free_closure`. A lambda passed straight to a non-`escaping`
parameter of a plain function keeps a stack env, since the call ends within one resume.

**Cascade.** Dropping a suspended coroutine drops `frame.awaiting` first, recursively,
then frees the frame: a whole await-chain torn down by dropping its head. The
combinators take part: an unresolved `select2`/`join2` (and `select2v`/`join2v`) has an
`on_drop` that drops both inputs (a finished `join2` input is just freed), so a
cancelled awaiter never leaves an input whose completion would wake freed memory.

**Cleanup on drop:** dropping a *suspended* coroutine does not continue it past the
await, but the `defer`s and `finally` blocks pending at that await run exactly once,
innermost first, before the frame is freed (matches a scope exit by unwinding: Rust
async-drop runs destructors the same way). The transform records, per await, the
defer frames pending there and lowers them into cleanup states of their own; `on_drop`
cascades to the awaited future, then sets the resume state to that cleanup and calls the
resume function once. A coroutine that must keep running its normal code after a cancel
request uses a cooperative **cancellation token** it checks (a normal threaded value, no
contract change), a deliberate later feature.

### 7.1 A cleanup that awaits

A pending `finally` or defer may await (as Python's asyncio lets a `finally` await during
cancellation). The cleanup then runs **detached**: the dropper's reference is gone and
nobody reads the result, so the frame becomes a task that owns itself until the cleanup
ends. The hand-off uses three more values of `state`, reached only on this path and only
by a coroutine future, so leaf futures and the four-state handshake are unchanged:

```
on_drop:        Future_drop(awaiting); atomic_store(&ret.state, DETACHED /*4*/);
                st = <cleanup>; resume();            // may park on the cleanup's own awaits
Future_drop:    ... f.on_drop(); free_closure(waker); free_closure(on_drop);
                if (CAS(&f.state, DETACHED -> ORPHANED /*5*/)) return;   // the cleanup owns f
                free(f);
cleanup end:    if (CAS(&ret.state, DETACHED -> CLEANED /*6*/)) return;  // still inside Future_drop
                free(frame);                          // ORPHANED: the frame frees itself
```

The awaited future is dropped first (the cascade), then the cleanup starts. A cleanup
whose awaits are all ready finishes inside `Future_drop` (`CLEANED`) and is freed there
as before; one that suspends is freed by its last state. `Future_drop` returns at once
for a state of 4 or more, so dropping the future again while its cleanup runs is a no-op.
A coroutine's own normal path never runs after a drop, so no completion can follow it.
An exception that escapes a detached cleanup propagates to whatever resumed it (the event
loop, or the `Future_drop` caller before the first suspension), the same as one escaping
a task given to `spawn`. The cleanup's awaits record no drop sites of their own: nobody
holds the future any more.

---

## 8. Surface syntax and type rules

- `async` is a function modifier: `async T f(...)`, parsed like `volatile let` (a
  leading qualifier before the return type).
- An `async T f(...)` has *declared* return type `T`; its *call expression* has type
  `Future<T>*`. The type checker performs this rewrite.
- `await E`: `E : Future<T>*`, `await E : T`. Legal **only inside an `async` function**
  (the top level drives a future by hand, see below). New keywords: `async`, `await`.
- Calling an `async` function without `await` yields `Future<T>*` (start now, await later,
  or hand to a combinator or `spawn`). The caller owns a future it neither awaits nor
  hands off and must drop it (§7).
- `Future_drop(f)` is the explicit cancel entry.
- At the top level a future is driven by hand: `Future_poll(f, waker)` installs a
  waker that calls `EventLoop_stop`, `EventLoop_run(lp)` runs the loop until then (skip it when
  `Future_poll` returns 1, the future is already ready), and the caller reads
  `f.value` and releases `f` with `Future_free_polled`, which also frees the waker it
  passed (`Future_free` leaves the waker alone, since an `await` frees its own). There
  is no `future_block` helper.

---

## 9. Components

The async stack decomposes into independently testable layers:

- **Atomic intrinsics** (`<atomic>`): `atomic_load`/`atomic_store`/`atomic_cas`/
  `atomic_swap` with acquire/release, lowering to LLVM atomics. The lock-free
  primitives the §3 handshake stands on; `<threading>` uses them too.
- **`stdlib/future.esk`**: `Future<T>` + `FutureHdr` + `Future_free` /
  `Future_drop`, implementing the §3 atomic protocol. The runtime model is
  validated by a hand-written coroutine over this contract that drives a real
  non-blocking socket read via `<eventloop>`, is cleanly cancelled, and resumes on
  a different thread than it completed on (cross-thread completion: a worker thread
  completes; the resume is scheduled on a different executor).
- **Executor** (`<executor>`): ready-queue + self-pipe wakeup over
  `<eventloop>`; `executor_new`, `Executor_schedule`, `Executor_run`. (`spawn` lives in
  `<future>`.)
- **Leaf futures** (`<net_async>`): `net_accept_async`/`net_read_async`/
  `net_write_async` (plus the readiness-only `net_readable_async`/`net_writable_async`)
  with a real `on_drop`.
- **Lexer/parser**: the `async` modifier, the `await` expression, and their tokens.
- **Type checker**: async return → `Future<T>*`; `await` typing; "await only in
  async". It runs again on the transformed program, so the lowering's output is
  type-checked like user code.
- **AST transform** (`sema/async_transform.cpp`): the state-machine split (§4) and
  the drop-time cleanup states (§7). The bulk of the work. `AsyncTransform::run` lowers
  each async function with an `AsyncLowering` (`sema/async_lower.h`), in phases: `setup`
  and `collectFrame` (`async_lower_setup.cpp`), the await hoisting (`async_lower_desugar.cpp`,
  one `desugar<Kind>` per statement kind), the state machinery (`async_lower_states.cpp`),
  `lowerBody` and `lowerStmt` (`async_lower_stmt.cpp`, one `lower<Kind>` per statement
  kind), and `buildCleanups`/`emitDecls` (`async_lower_frame.cpp`); the small AST builders
  are in `async_lower_util.cpp`.
- **Codegen**. No async-specific path: the transform emits structs/switch/closures/
  casts/atomics that codegen already handles.

The hand-written coroutine over `future.esk` is the behavioural oracle: the same
logic expressed in `async`/`await` must match it, and the suite adds a cancellation
test (the leaf fd is deregistered and the frame freed) and a thread-affinity test
(completion on a worker, resume observed on the home executor's thread).

---

## 10. What is locked, what is free

**Locked now (compiler↔generated-code ABI: breaking these recompiles/rewrites every
async function in existence):**

- `Future<T>` field set **and order**: `{ int state; fn()->void waker; fn()->void
  on_drop; T value; }`, `value` **last** so the header is type-erasable (§2.1).
- `FutureHdr` aliases the first three fields.
- `state` is **atomic**, four values `PENDING/WAITING/READY/CANCELLED`, with
  acquire/release ordering and the single-winner terminal swap (§3). A coroutine future
  dropped with an awaiting cleanup also passes through `DETACHED/ORPHANED/CLEANED`
  (4, 5, 6) after it is cancelled (§7.1); no other future ever holds them.
- The completion, drop, and arbitration protocols (§3) and the finalized-once
  invariant (§7).
- Completion goes through `waker()`; a cross-thread completion lands the resume on
  the awaiter's executor through `Executor_schedule` (the contract that makes
  thread-affinity work).

**Free to change later (touches only stdlib / the executor / the transform's
internals: no recompile of user async code):**

- Await *sources* (timers, joins, channels): new leaf futures obeying §3.
- Frame layout (`awaiting`, state numbering, spilled locals).
- Executor internals: thread count, ready-queue, wakeup mechanism, work-stealing.
- Combinators (`select2`/`join2` and any later ones, built on drop + completion).
- A cooperative cancellation token for cleanup-on-cancel.

`value`-last, the atomic four-state handshake, `on_drop`, and the waker contract
form the locked set: the ABI every async function and every leaf
future is generated against.
