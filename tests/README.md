# Tests

Compiler regression tests. Run the whole suite with:

```bash
cmake --build build          # make sure the compiler is current
tests/run.sh
```

`run.sh` exits non-zero if anything fails, so it is safe to use as a CI gate.
Override the compiler or C linker with `ESKIUC=...` / `CC=...`.

## How a test is judged

The runner classifies every file automatically. There is no list to maintain.

| Kind  | Files | Pass condition |
|-------|-------|----------------|
| **run**   | `NAME.esk` **+** `NAME.expected` | compiles, links, runs, and stdout matches `NAME.expected` **exactly** |
| **smoke** | `NAME.esk` with no `.expected`   | compiles, links, and exits `0` (output not checked) |
| **error** | `errors/NAME.esk`                | `--test-typechecker` exits non-zero **and** the diagnostics contain the `EXPECT-ERROR:` substring from the file's first line |
| **lint**  | `warnings/NAME.esk`              | type-checks under `-Wall` and emits exactly the warnings named by its `// EXPECT-WARNING:` lines (none listed means none allowed) |

A `run` or `smoke` test may have a C companion `NAME.c` next to it. The runner compiles
it with `$CC` and links it into the test binary, which is how calls across the C ABI are
checked (`c_abi_struct.c`, `c_abi_try.c`, `c_abi_callback.c`, `c_abi_fnptr.c`,
`bitfield_c_layout.c`). Every test links through `eskiuc` with no `-l` flags, so the
libraries a program implies (`#pragma link`, the C++ runtime, pthread) are exercised too.

`run.sh` also runs the generated deep-input tests from `tests/deep/gen.sh`: a
100000-operand chain, 10000-deep nesting, programs past the 100000-level nesting
limit that must be rejected, and types just under and past the 1000-level type limit.

These are *honest* tests: a `run` test fails the moment the generated program
prints anything different, and an `error` test fails if the compiler ever starts
**accepting** code it should reject. (Verified by deliberately corrupting an
expected file and by adding a compilable file under `errors/`: both make
`run.sh` exit 1.)

## Coverage

The runner needs no list, but this index keeps the suite legible. Keep it in sync
when you add a test.

### `run` tests (exact-match)

| Test | Exercises |
|------|-----------|
| `arithmetic` | integer arithmetic, precedence, `/` `%`, comparison, `&&`/`\|\|` |
| `int_widen` | integer widening honors source signedness at call/assign/init/struct/enum sites |
| `int64_arith` | an `int` literal mixed with an `int64` operand widens to 64-bit |
| `int_width` | integer width, signedness, and variadic-promotion correctness |
| `bitwise_assign` | compound bitwise assignment `&= \|= ^= <<= >>=` |
| `control_flow` | `if`/`else if`/`else`, `while`, `for`, `break`, `continue` |
| `for_in` | `for x in …` over a fixed-size array |
| `range_for` | `for (i in A..B)` half-open integer ranges (nested, variable bounds, empty) |
| `recursion` | self-recursion: factorial, fibonacci, Ackermann |
| `forward_decl` | forward declarations, call-before-define, mutual recursion |
| `pointers` | `&`/`*`, deref, write-through, indexing, typed pointer arithmetic |
| `ptr_member` | member read/write through a local `*T` variable (`p.x`) |
| `ptr_trailing_star` | the trailing-star spelling `T*` dereferences correctly |
| `strings` | `%s`, `string[i] -> char`, char codes |
| `question_op` | `?` postfix error-propagation operator |
| `volatile` | `volatile let` pointer load/store marked volatile in IR |
| `inline_asm` | `asm(...)` simple + extended compiles, links, runs |
| `variadic` | user-defined variadic fn: `...` + `va_list`/`va_start`/`va_arg<T>`/`va_end` (int + double) |
| `http2_frame` | `<http2>` 9-byte frame-header encode/decode round-trip (incl. 31-bit stream id) |
| `http2_conn` | `<http2>` connection codecs: SETTINGS write/apply, ACK, PING/PONG, GOAWAY round-trips |
| `http2_handshake` | `<http2>` async server opening handshake over a socketpair (preface + SETTINGS exchange + ACK) |
| `hpack` | `<hpack>` HPACK (RFC 7541): integer/string codecs, static + dynamic tables, §6 decode/encode, Huffman: RFC vectors |
| `http2_stream` | `<http2>` stream state machine, flow-control accounting, HEADERS/DATA/WINDOW_UPDATE/RST_STREAM codecs |
| `http2_server` | `<http2_server>` the h2c server end-to-end over a socketpair (request → handler → response) |
| `http2_chunking` | response bodies > 16384 split into MAX_FRAME_SIZE DATA frames (last has END_STREAM) |
| `async_elseif` | async `if/else-if/else` with `await` in branches + terminating `else` (transform regression) |
| `async_locals_forms` | in an async fn: a `static` local keeps one cell across calls, `for-in` over a slice, `try`/`catch`, an array-literal local, `match` on a local ADT, bindings named like a hoisted local |
| `http2_multiplex` | interleaved two-stream multiplexing: per-stream request assembly, each routed + answered |
| `const` | immutable bindings, usable as array sizes |
| `param_reassign` | reassigning scalar/pointer parameters; method call through a pointer parameter |
| `c_callback` | passing a top-level Eskiu function to a C API as a raw callback (drives libc `qsort`) |
| `pointer_const` | `const T*` (pointee read-only) vs `T* const` (binding read-only); read/rebind allowed, write-through and const-drop rejected |
| `pack_n` | `#pragma pack(N)` for N > 1: field-alignment cap, padding, size matches the C ABI |
| `sizeof_union_ptr` | `sizeof`, `union` (incl. float member), typed pointer arithmetic |
| `os_macros` | exactly one host-OS macro (`__APPLE__`/`__linux__`) is defined |
| `preprocessor` | object-like and function-like `#define`, `#ifdef` |
| `pp_pack` | backslash-continued function-like macro; `#pragma pack` |
| `pp_loc` | `__LINE__` / `__FILE__` preprocessor expansion |
| `shebang` | a leading `#!` line is ignored by the preprocessor (line numbers preserved) |
| `structs_methods` | named initializers, field access, method calls, `self` mutation |
| `interfaces` | structural interfaces, vtable fat-pointer dispatch |
| `enums` | enum members as int constants, usable in `switch`/comparisons |
| `enum_adt` | algebraic enums + `match`: payload variants, multiple bindings, `_` default, classic enum coexisting |
| `enum_generic` | generic algebraic enums `Option<T>`/`Either<A,B>`: turbofish construction, per-instance monomorphization, `match` |
| `either_stdlib` | `<either>` `Option`/`Either` helpers (`Option_is_some`, `Option_unwrap_or`) |
| `type_alias` | `type u8 = uint8;` resolves to the underlying type |
| `bitfields` | bitfield assignment + masked read; signed fields sign-extend |
| `bitfield_signed_store` | a narrower signed value stored into a wide bitfield sign-extends (assignment, struct literal, `+=`); an unsigned one zero-extends |
| `array_return_large` | a function returning an array wider than 16 bytes (`int64[4]`, an array of interface values) goes through the hidden return pointer |
| `generic_rvalue_instance` | a generic struct or enum instance reached only through a call result (`flip(n).a`, `unbox(bx(7))`, `bx(2.5).get()`, `match wrap<int64>(5)`) |
| `closure_array_ptr` | an array of closures `fn(int)->int[3]` (indexed and called) and a pointer to a closure `*fn(int)->int` (`&f`, `*pf`, store through it) |
| `global_iface_init` | a global, `static` or struct-field interface value initialized with `&global` folds to `{data, vtable}` |
| `struct_lit_array_field` | a struct literal's array field takes `{...}` (nested, zero-filled) in a local, an assignment, a bitfield struct and a global |
| `type_order_fields` | a struct field of a struct, enum, union, interface, alias or generic instance declared later in the file gets that type's real layout |
| `iface_ptr_array` | interface values in an array literal (local, global, `for-in`), a method call through `*I`, and a closure `fn(I)->int` boxing its `&s` argument |
| `unary_plus` | unary `+` yields its (integer-promoted) operand, in code and in constant initializers |
| `const_bitfield_union` | global and `static` initializers of bitfield structs, unions (any member, nested in a struct), `packed` and `pack(2)` structs fold to their C byte image; local union literals |
| `templates_result` | `Result<int,string>` monomorphization, `Ok`/`Err` |
| `template_inference` | `T` inferred when it appears directly as a parameter type |
| `template_inference_composite` | `T` inferred from a composite param type (`List<T>*`) |
| `template_struct_literal` | named template struct literal `Pair<int,float>{…}` |
| `template_nested_close` | a lexed `>>` closes nested brackets (`List_init<List<int>>`) |
| `nested_template` | a template fn calling another with the type param forwarded |
| `template_vs_compare` | `f(a < b, c > (d))` is two comparisons; `Name<...>(` is a call only for a known generic or type-only arguments |
| `list_struct` | `List<StructType>` used through helper functions |
| `list_free_reuse` | `List_free` sets `data` to null: a push after it regrows the list and a second free (also via `String_split_free`) is a no-op, not a double free |
| `member_temp` | member access on a struct-valued temporary (call result) |
| `cast_alias` | casts to struct-pointer / alias / enum; alias used as a type |
| `cast_shadowed_type` | a local or parameter named like a type shadows it, so `(P) - 1` is not a cast |
| `lambdas` | anonymous functions, `fn(T)->R`, higher-order functions |
| `closures` | capturing & non-capturing lambdas through higher-order functions |
| `closure_escape` | escape analysis: non-escaping closure on the stack, escaping one heap + `free_closure` |
| `closure_local_env` | a lambda bound to a local that is only called (also in a loop, nested, or captured by an argument lambda) keeps its env on the stack; a copied local, or one captured by an escaping lambda, stays on the heap |
| `generic_closure` | a capturing closure inside a **generic** function body; the `T`-typed capture's env field is substituted per instantiation (`box<int>` / `box<int64>`) |
| `import_cast` | a cast to a type imported from another file (`(FutureHdr*)p`) parses as a cast |
| `closure_global` | a module global read inside a closure reads the global (not a stale copy) |
| `fn_pointer` | function pointers as values and parameters |
| `fn_more` | fn-pointer as a return type; calling a fn-pointer struct field |
| `exceptions` | `try`/`catch`/`finally`, `throw`, exception from a nested call |
| `sret` | a struct larger than 16 bytes is returned via sret (arm64) |
| `alloc` | `<mem>` `alloc<T>(n)`/`free`; `<alloc>` Bump/Arena/Pool/FirstFit |
| `alloc_with` | `alloc_with(&a, T, n)` over a caller-provided buffer |
| `sysheap` | `<sysheap>` mmap-backed `FirstFit` heap: allocate/free/reuse with no libc `malloc` |
| `atomic` | `<atomic>` `atomic_load`/`store`/`swap`/`cas` → LLVM atomics |
| `base64` | `<base64>` encode/decode over buffers |
| `env` | `<env>` `env_get`/`has`/`get_or`/`get_int` |
| `fs` | `<fs>` file I/O |
| `http` | `<http>` request parser + response builder |
| `math` | `<math>` libm wrappers: sqrt/fabs/pow/floor/ceil/fmod/abs |
| `json` | `<json>` builder + recursive-descent parser |
| `net_echo` | `<net>` loopback TCP echo (server thread passed as a bare fn) |
| `path` | `<path>` `join`/`basename`/`dirname`/`extension`/`is_absolute` |
| `string_methods` | `<string>` builder ops (`push`, `char_at`, `len`) |
| `string_ops` | `<string>` `split` (e.g. `"a,b,,c"` on `','`) |
| `threading` | `<threading>` `Mutex`/`Cond`/`Sem`; shared state captured by pointer |
| `time` | `<time>` wall clock / monotonic / `time_sleep_ms` |
| `executor` | async `Executor`: scheduled wakers run on the loop thread in FIFO order |
| `net_async` | leaf future: a coroutine awaits `net_read_async` over the reactor |
| `async_basic` | `async`/`await` transform: one await of an already-ready future (fast path) |
| `async_io` | suspending await over a real reactor read; params live across the await |
| `async_multi` | multiple awaits (fast path); values thread through frame states |
| `async_multi_io` | two suspending awaits over the reactor (multi-state chain) |
| `async_return_await` | `return await E;` desugaring and `async void` (uint8 unit) |
| `async_cancel` | `Future_drop` on a suspended async function: cascade-drop + free, no UAF |
| `async_loop` | `while` loop containing an await (read-until-EOF; loop back-edge) |
| `async_if` | `if`/`else` with an await in each branch (branch-join states) |
| `async_for` | C-style `for` loop containing an await |
| `async_break` | `break`/`continue` inside an awaiting `while`/`for` (state transitions; `for` continue runs the step) |
| `async_switch` | `switch` containing an await: fall-through + suspending case + `default` + `break` |
| `async_try` | `await` in a `try` body and a `catch`: exceptions before/after a suspension reach the right handler, nested try, `finally` once on every exit (early return, break/continue, unmatched exception), a body `defer` before the handler |
| `async_try_cancel` | a future dropped while suspended in try/finally (by `select2` + timer, or by hand, also inside a handler) runs its pending finally blocks and defers once |
| `async_finally_await` | an `await` inside a `finally` or a `defer` suspends: fall-through, early `return` (value kept), `break`/`continue`, a caught exception, an exception unwinding through the try, LIFO defers, defers in loops, nested cleanups, a lambda in a finally |
| `async_cancel_await_cleanup` | a dropped future whose pending cleanup awaits runs it detached (a `select2` loser, a hand drop, an already-ready await, a second drop, a drop inside an awaiting finally or one running while an exception unwinds), then frees its frame |
| `async_await_positions` | `await` in a match arm, a switch subject, a for-in iterable, a range bound, a compound assignment (target once), larger expressions and call arguments (side-effect order), `&&`/`||`/`?:`, and while/do/for conditions and steps |
| `async_await_edges` | awaits in a return and a loop condition inside try, a plain try returning inside a split one, break from a match arm, lambdas in a try and its handler, a catch variable, nested awaits, struct/array literals, a static local, a switch in a try |
| `async_generic_try` | a generic async function awaiting in a try (with a compound assignment) and a switch subject; a match on a computed subject whose arms await |
| `async_generic_positions` | a generic async function awaiting in a payload-binding `match` arm, after a side-effecting operand and in a `?:` arm, each with an `int` and a struct instance (also parked, leak-free) |
| `async_for_in` | `for-in` containing an await, over a fixed-size array and a `List`-like struct (suspending) |
| `async_timer` | `<timer>` `timer_after` leaf future: a delayed await + read-with-timeout via `select2(read, timer)` |
| `async_frame_expr` | frame-hoisted locals used in a struct literal / index / call after an await are renamed to `fr.x` (shared child-enumeration) |
| `select_value` | `<futureval>` `select2v`: winner's value as `Either<A,B>` over the reactor (timer-wins / data-wins) |
| `join_value` | `<futureval>` `join2v`: both values as a `Pair<A,B>` |
| `async_channel` | `<channel>` `Chan_send`/`Chan_recv`: buffered fast path + parked handoff; also guards the cast-after-deduplicated-import parser fix |
| `async_spawn` | detached generic `spawn<T>` of async tasks (ready + suspending), leak-free |
| `async_select` | generic `select2<A,B>`: await the first of two futures; loser dropped (A-wins + B-wins) |
| `combinator_drop` | dropping an unresolved `select2`/`join2`/`select2v`/`join2v` drops its inputs (producers cancelled, wakers unhooked); nested timeout cancels an inner select |
| `net_no_sigpipe` | a send to a closed peer (`net_send`, `net_send_str`, `http_reply`, `net_write_async`) returns an error instead of raising SIGPIPE |
| `async_join` | generic `join2<A,B>`: await both futures, then read both values |
| `const_method` | A method declaring `const T* self` may be called on a const value. |
| `decl_repeats_ok` | Repeated top-level declarations that stay legal: a prototype before its definition, an `extern` declaration beside the variable's definition, and... |
| `enum_dup_value` | Classic enum members may share a value (as in C). A match is on the value, so one arm covers every member with that value. |
| `generic_ptr_leading` | A leading-star pointer to a generic struct instance (`*List<int>`, `*B<int>`) is a valid declared type, including one used only as an explicit... |
| `method_free_fn` | Method-call syntax on free-function methods (`Type_method(self, ...)`), including stdlib ones: `c.get()`, `s.trim()`, `n.push('!')`. |
| `nullable_narrow` | Null-narrowing forms for `?*T`: each dereference below is proven safe. |
| `return_flow` | Definite-return analysis: forms that always return are accepted. |
| `sema_accepts` | Valid forms next to the stricter checks: a lambda whose return type is reconciled to the declared one, an enum-member case label, a struct named... |
| `sizeof_var` | sizeof(variable) measures the variable's type (C semantics). |
| `uninit_chain_assign` | A chained or nested assignment initializes its targets; they are not read first. |
| `adt_layout` | ADT enum payload sizing: array fields, nested enums and generic instances must get enough payload slots, and a struct holding an enum by value must be sized... |
| `alloc_overflow` | Bump/Arena/Pool/FirstFit and the <sysheap> Heap reject huge or negative sizes instead of wrapping the size arithmetic, and a FirstFit buffer smaller than one region header holds nothing (no write past it). |
| `async_dowhile_defer` | do/while, defer and capturing lambdas inside async functions, around awaits. |
| `async_closure_env` | a lambda bound to a local of an async function is freed by the frame: on completion, on rebinding (a loop, a conditional or repeated binding), after an exception is caught, and on a drop with no cleanup, a defer, or an awaiting (detached) cleanup; generic too. An argument lambda keeps a stack env. Leak-free under `leaks --atExit` |
| `async_expr_rewrite` | Frame-hoisted locals used after an await inside every expression form: ++/--, a ternary, an array index, a struct literal, a slice, and a cast. |
| `async_local_named_fr` | A user local named `fr` in an async function must not collide with the transform's internal frame pointer. |
| `base64_strict` | base64_decode rejects impossible lengths and misplaced padding (it used to decode "Z" and "Z=g=" to something) while still accepting padded, unpadded, and... |
| `bitfield_incdec` | `++`/`--` on a bitfield is a masked read-modify-write of its storage word: it wraps within the field's width and never spills into the neighbouring fields. |
| `bitfield_layout` | Bitfields pack into storage words of their declared type (like C): uint8 fields share a byte, a uint64 field keeps all 64 bits, and a new word opens when... |
| `block_shadow` | A `let` in a nested block (or a for-init) shadows, not overwrites, an outer variable of the same name; the outer binding is visible again after the scope. |
| `bool_conversion` | Conversion to bool is `!= 0` (C _Bool semantics), never a truncation to the low bit. |
| `c_abi_struct` | Structs passed and returned BY VALUE across `extern` C functions follow the target C ABI (C side: tests/c_abi_struct.c): register-sized aggregates, HFAs,... (C companion `c_abi_struct.c`) |
| `c_abi_try` | C-ABI-lowered extern calls inside a `try` body (lowered to `invoke`), with the externs declared before the structs they take by value. (C companion `c_abi_try.c`) |
| `call_forms` | Generic inference from a struct-literal argument, and calling a returned closure directly (`pick()(3, 4)`). |
| `chained_assign` | A chained assignment passes on the value converted to the inner target's type. |
| `chan_free_waiter` | Chan_free detaches a still-parked receiver, so dropping that Chan_recv future afterwards no longer reads the freed channel. |
| `chan_recv_drop` | A parked Chan_recv dropped by select2 (the timeout pattern) must unpark: the next Chan_send buffers its value instead of completing the freed future. |
| `compound_assign_once` | `lv op= v` evaluates the lvalue `lv` exactly once (C semantics), even when it has side effects: an index call, a post-increment, or a call returning a pointer. |
| `defer_unbraced` | A statement body that is not a block (an unbraced if/else/loop body, a switch case, a match arm) is its own scope: a `defer` in it runs when that body ends,... |
| `dup_struct_decl` | The same struct declared twice (as when two inputs of a multi-file build share a header-style declaration): the duplicate is merged, not emitted a second time. |
| `env_get_int` | env_get_int returns the fallback for a set but non-numeric (or out-of-range) value, as documented, instead of atoi's 0. |
| `eventloop_edges` | el_new(0) grows its fd table instead of looping forever, replacing an fd's registration from outside its callback frees the old env, and EventLoop_free frees still-registered fds' envs. |
| `eventloop_resources` | EventLoop_free closes the kqueue/epoll descriptor (and Executor_free its self-pipe), so creating and freeing loops does not leak fds; and a full timer table... |
| `executor_many` | Executor_schedule coalesces wakes into one pending self-pipe byte (write end non-blocking), so 100000 wakers scheduled before the loop runs no longer block the scheduler. |
| `expr_types` | Static types of compound expressions: pointer arithmetic keeps the pointee type, numeric binaries promote, and a ternary takes its arms' common type. |
| `finally_catch_exit` | `finally` runs when a catch handler leaves early (return / break / continue), just as it does on fall-through and on an early exit from the try body. |
| `float_literal_range` | Float literals follow C: a denormal keeps its (tiny, nonzero) value, and a literal too large for double is infinity. |
| `forin_multidim` | for-in over a multidimensional array binds each row (an `int[3]` for `int[2][3]`). |
| `fs_read_pipe` | fs_read_all on a stream that cannot seek (a pipe, like stdin) must read to end of file instead of trusting ftell (-1 there), growing past its first guess. |
| `future_polled` | Future_free_polled frees the resume closure a hand-driven Future_poll installed along with the future (Future_free leaves it, so a capturing resume leaked). |
| `generic_operators` | Operator overloads resolve inside a generic body, per instantiation. |
| `global_const_expr` | Global initializers are folded at compile time, operators included (C semantics). |
| `global_neg_const` | Negative constants in global initializers keep their sign, and a doubly negated literal folds to the positive value. |
| `heap_coalesce` | FirstFit (and the <sysheap> Heap over it) coalesces adjacent free regions: after 64 x 900-byte blocks are freed in any order, a 30000-byte allocation fits... |
| `hpack_evict_name` | RFC 7541 §4.4: a literal with incremental indexing may name a dynamic entry that its own insertion evicts. |
| `hpack_limits` | HPACK: the Huffman scratch arena is sized from the block (a valid 20000-byte value decodes), a table size update after a field is a COMPRESSION_ERROR, and more fields than the caller's array is -2 with the table kept in sync. |
| `hpack_size_update` | A §6.3 dynamic table size update above SETTINGS_HEADER_TABLE_SIZE is a COMPRESSION_ERROR (the decode fails) instead of growing the table past the entry... |
| `hpack_truncated` | Malformed HPACK blocks must fail cleanly (-1) without reading past the block: a string literal longer than the remaining bytes, a prefix integer whose... |
| `http2_big_headers` | Response headers larger than the old fixed 8 KB block are sized from the headers, and a block over MAX_FRAME_SIZE (16384) is split into a HEADERS frame plus... |
| `http2_flow_control` | h2c flow control with a window-honoring client thread: a 100000-byte upload gets WINDOW_UPDATE credit, a response waiting for window is not credited by another stream's update, and a request arriving meanwhile is decoded (HPACK in sync) and answered. |
| `http2_field_rules` | RFC 9113 §8.2.1: a request field with CR/LF/NUL or edge whitespace in its value, or an uppercase/space/colon in its name, is answered 400 without the handler (an LF used to forge a header line). |
| `http2_fill_request_leak` | `_h2_fill_request` sets the version by clear + append; `String_from` on the allocated String leaked 16 bytes per HTTP/2 request (checked with `leaks --atExit` / LeakSanitizer). |
| `http2_frame_rules` | h2c frame rules: PADDED DATA/HEADERS unpadded, CONTINUATION reassembled (interrupted = PROTOCOL_ERROR), PING answered, window overflow and bad SETTINGS are GOAWAY, over-long padding rejected, >64 header fields get 431 without the handler. |
| `http2_head_trailers` | An HTTP/2 HEAD response has the content-length but no body and a 304 neither (both sent the body as DATA); a trailer with an uppercase name, a connection-specific field, `te`, a CR or a pseudo-header resets the stream without the handler. |
| `http2_settings_values` | SETTINGS values are validated before any is applied (ENABLE_PUSH 0/1, MAX_FRAME_SIZE range, INITIAL_WINDOW_SIZE at most 2^31 - 1), with the RFC 9113 error code from h2_settings_error. |
| `http2_malformed` | RFC 9113 §8.1.1, §8.2.2, §8.3: a malformed request (unknown, response, repeated or late pseudo-header, missing `:method`/`:scheme`/`:path`, CONNECT rules, connection-specific field, `te` other than trailers, content-length not matching the DATA, pseudo-header in trailers) is reset with PROTOCOL_ERROR without the handler; late frames on the reset stream are ignored and HPACK stays in sync. |
| `http2_response_fields` | An HTTP/2 response drops connection-specific fields and the handler's content-length (it went out twice), and a 204/1xx response has no content-length and no body; engine and `_h2_build_response`. |
| `http2_stream_errors` | RST_STREAM on an idle stream is a PROTOCOL_ERROR GOAWAY, a GOAWAY under 8 bytes a FRAME_SIZE_ERROR, and a HEADERS/PRIORITY self-dependency a PROTOCOL_ERROR reset (its block still decoded, HPACK in sync). |
| `http2_tls_engine` | Both h2-over-TLS servers run the H2Server engine: padding, CONTINUATION, PING (async), >64 headers (431) and window overflow, over an OpenSSL stand-in (C companion `http2_tls_engine.c`). |
| `http_async_accept_retry` | The async servers retry a failed accept (EMFILE) after a backoff via net_accept_retry_async instead of spawning a handler on fd -1 that never completes the wait-group. |
| `http_header_edges` | HTTP/1.1 edge cases: header values lose surrounding whitespace (OWS), an overflowing or malformed Content-Length is a 400 (it used to wrap to 0),... |
| `http_host_value` | A Host value must be uri-host [":" port] (RFC 9110 §7.2): whitespace, a list, a bad port, a bad `%` escape or an unclosed IP-literal is 400 in `http_recv` and `HttpRequest_parse_status`. |
| `http_reply_null_body` | `http_reply` with a null body sends Content-Length: 0 (it used to read body.len through the null pointer). |
| `http_nul_header` | RFC 9110 §5.5: a NUL in a header value or the request target makes `HttpRequest_parse` and `HttpReq_parse_head` fail (a lookup used to stop at the NUL). |
| `http_parse_framing` | `HttpRequest_parse` frames by RFC 9112 like `http_recv`: the body is exactly Content-Length bytes, an invalid or conflicting Content-Length is 400, a request cut short (no blank line, a short body) is incomplete (-1 from `HttpRequest_parse_status`); `HttpConnBuf_feed` waits for a request fed a few bytes at a time. |
| `http_no_content_length` | RFC 9110 §8.6: `HttpResponse_render` and `http_reply` send no Content-Length and no body for a 1xx or 204 status (they used to add `Content-Length: 0`). |
| `http_strict` | HTTP/1.1 strictness: a body cut short by the peer is 400, conflicting Content-Length values are 400, a bare CR/LF in the head or an empty method/path is malformed (the servers answer 400 without the handler), and render sends a body past an embedded NUL. |
| `http_rfc9112` | RFC 9112 framing: a chunked body is decoded (extensions, trailers), TE with CL is 400, a non-chunked coding 501, whitespace before a colon, obs-fold, a missing or repeated Host in HTTP/1.1 and an invalid version 400, HTTP/2.0 505; same rules in `HttpRequest_parse` |
| `http_serve_split` | `http_serve` and `http_serve_async` read until the request is complete (a head and body split over TCP segments reach the handler whole), a peer that closes early gets 400, a HEAD response has Content-Length but no body, a 304 neither. |
| `http_timeouts` | `http_serve` (one worker) and `http_serve_async` under short `HttpLimits`: a silent peer is closed, a trickled head (its deadline not extended by the bytes) and a stalled body get 408, then a good client is served; past `max_open` a connection is closed at once. |
| `http_serve_slow_reader` | `http_serve` bounds a whole answer by `write_ms` from its first byte, retrying partial sends: a slow reader cannot hold the only worker |
| `http2_timeouts` | h2c deadlines over a socketpair kept open: no preface or no SETTINGS closes after `header_ms`, an idle connection gets GOAWAY(NO_ERROR) after `idle_ms` even with PINGs coming in, an unfinished header block and a response with no window are closed. |
| `http2_tls_timeouts` | The blocking (poll) and async (timer) h2-over-TLS servers close a silent connection after `header_ms` and send GOAWAY(NO_ERROR) after `idle_ms`, over an OpenSSL stand-in (C companion `http2_tls_timeouts.c`). |
| `int_promotion` | C integer promotions: operands narrower than `int` (bool, char, int8/16, uint8/16) become `int` before arithmetic, bitwise, shift, and comparison. |
| `interface_values` | An interface value is a {data, vtable} fat pointer held by value: it can be a local, a struct field, a return value, or an argument, and it refers to a... |
| `json_builder_escape` | The JSON builder escapes control bytes (so its output parses back) and writes a full int64 instead of truncating it to 32 bits. |
| `json_depth` | json_parse rejects nesting past JSON_MAX_DEPTH (512) with null instead of overflowing the stack, and JsonValue_free frees a deep tree iteratively. |
| `json_strict` | json_parse is strict (RFC 8259) and never reads past the input: truncated escapes and literals fail cleanly, malformed literals / numbers and trailing... |
| `logical_not` | `!` on a pointer tests for null, on a float tests for 0.0, and a user `operator !(V)` resolves for a struct operand. |
| `loop_temps` | Expression temporaries (short-circuit slots, ternary slots, struct/ADT/closure temps) inside a long loop must not allocate stack space per iteration. |
| `map_hash_intmin` | A key whose djb2 hash is exactly 0x80000000 ("ovuga,m") must still land in a valid bucket: the signed `h = 0 - h` fold left INT_MIN negative, a negative... |
| `multipart_delimiters` | multipart: the boundary is parsed as a real (case-insensitive, whole-name, quotable) Content-Type parameter, and a delimiter counts only at the body start or after CRLF with its line ending in -- or CRLF. |
| `multipart_name_match` | multipart_part matches the `name` parameter only (not the tail of `filename="..."`), and a present-but-empty field is found with length 0. |
| `multipart_disposition` | `multipart_part` matches only the Content-Disposition `name` parameter (not a `name=` in Content-Type), case-insensitive header and parameter names, token or quoted values, exact value match |
| `nested_body_context` | A lambda body, or a generic first instantiated inside a `try`, is its own function: it never runs the enclosing function's defers, and never unwinds into... |
| `net_write_async` | net_write_async completes with the byte count and frees its progress counter on completion (it used to leak one per call; only the cancel path freed it). |
| `path_posix_edges` | POSIX dirname/basename edge cases: the root, empty paths, and repeated separators between the directory and the final component. |
| `ptr_diff` | Pointer difference counts elements (C), not bytes. |
| `range_bound_once` | A range loop's upper bound is evaluated once, before the first iteration. |
| `regex_class_range` | An invalid bracket range (`[z-a]`, `[a-\d]`) is a compile error as in RE2, and a `-` after a shorthand is a literal (`[\d-z]` is digits, `-` and `z`, as in RE2); `\D` `\W` `\S` inside a class are the complemented shorthands (they used to be the literal letters). |
| `regex_escapes` | Escapes follow RE2 (checked against Go regexp): `\b` `\B` word boundaries, `\A` `\z`, `\xHH` `\x{..}`, octal, `\a`, escaped punctuation; any other escaped letter or digit (`\q`, `\1`, `\Z`, `[\b]`) is a compile error (they used to be the literal letter). |
| `regex_depth` | regex_compile rejects group nesting past RE_MAX_DEPTH (512) with an error instead of overflowing the stack. |
| `regex_empty_iter` | A starred group that can match empty takes one empty iteration and exits, as in RE2 (`(a?\|b)*` on "b" is (0,0) with group 1 = (0,0)). |
| `regex_repeat_count` | A `{m,n}` count above 1000 is a compile error (RE2 limit); the digits used to wrap, so `a{4294967298}` compiled as `a{2}`. |
| `regex_repeat_groups` | Counted or repeated groups duplicate SAVE instructions; the Pike VM must still find the match (capture storage used to run out and report a silent no-match). |
| `regex_posix_class` | RE2 POSIX classes in brackets (`[[:alpha:]]`, `[[:^space:]]`, all 14 names; an unknown name is an error, `[:` without `:]` a literal `[`), and a `{` that starts no `{n}`/`{n,}`/`{n,m}` is a literal (`a{,2}`); checked against Go regexp. |
| `regex_space_class` | `\s` is `[\t\n\f\r ]` as in RE2: it matched `\v` too (and `\S`, `[^\s]` missed it); `[[:space:]]` keeps `\v`. |
| `regex_unicode` | UTF-8 as in Go regexp: `.` and classes match whole code points (offsets stay byte offsets, an invalid byte reads as U+FFFD), `\p{..}`/`\P{..}` categories and scripts, `(?i)` by simple case folding (`k`, `K`, U+212A), `(?m)` `(?s)` `(?U)` for the rest of a group or scoped `(?flags:..)`, `(?:..)`, named groups, `\Q..\E`; each case checked against Go. |
| `rvalue_member` | Member access, indexing and method calls work on rvalue aggregates (call results, operator results, ternaries, fields of temporaries), not just on variables. |
| `self_append` | Appending a String or Bytes to itself must copy from the live buffer, not from the one freed when the append grows it. |
| `static_local_closure` | A `static` local has static storage: a closure refers to that one cell (like a global) rather than capturing a copy, and an uninitialized static starts at zero. |
| `stdlib_after_free` | A freed String or Map/HashMap is empty rather than a trap (clear, trim, at, get), String_substring clamps a negative start, and DateTime_to_epoch sums the time of day in 64 bits. |
| `string_int_edges` | String integer edge cases: INT_MIN renders fully (it used to print "-"), a zero-capacity String_init still has room for its NUL, String_to_int accepts a... |
| `struct_lit_zero_fill` | Fields a struct literal omits are zero-initialized (C semantics), at -O0 and -O2. |
| `switch_case_fold` | Case values fold to integer constants: literals, chars, enum members, casts and arithmetic over them. |
| `switch_const_case` | A top-level `const int` (or a constant expression over one) is a valid case label. |
| `ternary_null` | A `null` ternary arm takes the other arm's pointer type. |
| `ternary_wide` | A ternary arm that is an integer literal too wide for `int` makes the result 64-bit, and equal-width mixed signedness is unsigned (C usual arithmetic... |
| `time_negative_year` | DateTime_format_iso prints a negative (proleptic) year in ISO 8601 expanded form, "-0001", instead of zero-padding the digits around the sign ("00-1"). |
| `tls_alpn_bounds` | `_tls_alpn_pick_h2` stops at an ALPN entry whose length runs past the list instead of reading beyond the buffer. |
| `tls_frame_limit` | A peer-controlled 24-bit frame length larger than the 16384-byte frame buffer must be rejected (FRAME_SIZE_ERROR), not read past the buffer. |
| `union_layout` | A union takes the alignment of its most-aligned member, so it lands at the C offset inside a struct and the struct is padded like C (u at 8, size 24). |
| `async_name_collisions` | Locals and parameters named like the async transform's synthesized names (`__fr`, `st`, `ret`, `awaiting`, the await temporaries, the resume function) do not collide with them. |
| `async_range_wide` | An await inside a range loop over `int64` bounds keeps the hoisted loop variable `int64`. |
| `bitfield_unnamed` | unnamed bitfields `T : N` and `T : 0` in structs, a generic struct and a union: positional literals skip them, their bits stay zero, `sizeof` folds |
| `bitfield_unnamed_c` | unnamed bitfields follow the target's C layout and C ABI (C side: `bitfield_unnamed_c.c`): sizes, alignments, fields written on either side, by-value args and results |
| `bitfield_c_layout` | Bitfields follow the target's C layout (C side: `bitfield_c_layout.c`): mixed declared types share a storage unit when they fit (SysV/AAPCS), packed structs pack bit by bit; C writes, Eskiu reads, and back. |
| `c_abi_callback` | An Eskiu function passed to C as `(*void)f` that takes or returns a struct by value is reached through a C-ABI thunk (C side: `c_abi_callback.c`). |
| `c_abi_fnptr` | An `extern` fn-typed parameter is a C function pointer: a top-level function is passed by its C address (through a thunk for by-value structs), `null` as a null pointer, and libc `qsort` takes its comparator that way (C side: `c_abi_fnptr.c`). |
| `const_fold_c` | Constant initializers compute what the same expression computes at run time (promotions, usual arithmetic conversions, sign-extending casts, wrapping). |
| `const_ptr_method` | A method on `const P* self` is callable through a pointer to const. |
| `const_sizeof_struct` | A top-level `const` using `sizeof(struct)` folds to the real size. |
| `defer_block_body` | A braced defer body run on an early exit does not invalidate the frame being unwound. |
| `defer_shadow` | A deferred statement names the variables visible where it was written, even when it runs where a later declaration shadows one. |
| `defer_switch_continue` | `continue` of a loop inside a `switch` case runs only the loop body's defers. |
| `exceptions_nolib` | A program that throws links with no `-l` flag (the driver adds the C++ runtime). |
| `extern_defined_struct` | An `extern` prototype next to the program's own definition keeps the Eskiu convention. |
| `fn_type_local` | A C-style local of function type, `fn(int32)->int32 h = f;`, parses. |
| `generic_arith_type` | Built-in operators in a generic body get the instance's types (unsigned division and comparison stay unsigned). |
| `generic_const_self` | A generic `Type_method` taking `const Box<T>* self` infers `T` through a const receiver. |
| `generic_dot_call` | `x.m(args)` on a generic instance calls the generic `S_m<T..>` with the receiver's type arguments (`List`, `Map`, `HashMap`, `Chan`, and inside generic bodies). |
| `generic_infer_multi` | One parameter carrying several type parameters binds them all (`Pair<A, B>`, `HashMap_get(&m, k, &out)`). |
| `generic_struct_methods` | Inline methods on a generic struct, instantiated per struct instance. |
| `http_int64_lengths` | `Content-Length` digits are written from an `int64`, so values above `INT_MAX` come out in full. |
| `int_conversions_c` | C's usual arithmetic conversions: same-width signed/unsigned is unsigned, a shift takes its left operand's type, unary `-`/`~` promote. |
| `literal_unsigned_mix` | A literal mixed with an unsigned operand follows C's conversions; a char literal is unsigned. |
| `main_int32` | `int32 main()` is a valid entry point. |
| `math_nolib` | `import <math>` links with no `-l` flag (`#pragma link("m")` on glibc). |
| `pragma_link` | `#pragma link("name")` links `-lname`: repeated pragmas link once, an import's pragma counts, one in a skipped `#ifdef` branch does not. |
| `range_bound_outer` | Range bounds are read in the enclosing scope: a bound naming the loop variable means the outer one. |
| `range_end_name` | The range loop's synthesized bound local does not capture a user variable spelled `__end_i`. |
| `range_for_wide` | The range loop variable takes the bounds' common integer type, so `int64`/`uint64` bounds are not truncated. |
| `shadow_init` | A shadowing declaration's initializer reads the outer variable. |
| `sizeof_mixed_sign` | `sizeof` is an `int64` in mixed-sign arithmetic. |
| `ternary_narrow_arms` | Two different narrow ternary arms meet as `int`. |
| `wide_literal_type` | An integer literal too wide for `int` is an `int64` and widens its operation. |
| `array_2d` | multidimensional `int[N][M]` in C order: the leftmost bracket is the outer dimension |
| `async_await_var` | awaiting a future held in a variable takes its type from the `let` declaration |
| `bytes` | `<bytes>`: embedded NUL survives, append/eq/slice, base64 round trip |
| `closure_coerce` | a closure call widens an argument narrower than its parameter |
| `comment_backslash_continuation` | a `//` comment ending in `\` does not splice the next line into the comment |
| `crlf_source` | CRLF line endings: backslash continuation, directives and `__LINE__` keep the right line numbers |
| `datetime` | `<time>` UTC civil calendar: split, format and round trip |
| `dead_code_return` | code after a `return` is dropped instead of emitted into a terminated block |
| `defer` | `defer` runs LIFO at block exit on fall-through, `return`, `break`, `continue` and `?` |
| `enum_match` | `match` on a payload-less enum: exhaustive dispatch, explicit `= N` values, `_` default |
| `errdefer` | `errdefer` runs only on a `?`-propagation exit; plain `defer` always runs |
| `escapes` | string and char escapes share one set; `\0` is NUL and char literals accept `\r` |
| `escapes_hex` | `\xNN` hex escapes in string and char literals decode to the exact bytes |
| `float_arg_coerce` | a `double` literal passed to a `float` parameter narrows at the call site |
| `float_exponent` | scientific-notation float literals (`3.4e38`, `2.5e-3`, `1e6`, `6.022e+23`) |
| `global_array_init` | global and static-local initializers, including array literals, survive to load time |
| `global_const_init` | global and static initializers fold every constant form (64-bit, struct, const/enum/sizeof, `~`/`!`, string, hex) |
| `global_lambda` | a non-capturing lambda assigned to a global folds to a constant closure |
| `iface_arg_coerce` | interface (vtable) dispatch coerces each argument to the method's parameter type |
| `import_dedup` | imports are deduplicated by canonical path, and an import cycle does not re-parse the root |
| `labeled_break` | `break label` / `continue label` target a named loop and unwind the defers they leave |
| `long_chain` | precedence climbing: equal-precedence operators fold left, long mixed chains follow C precedence |
| `loop_locals` | a long loop with several locals does not grow the stack (allocas live in the entry block) |
| `map` | `Map<V>` string-keyed hash map with growth and struct values |
| `map_generic` | `HashMap<K,V>` with fn-pointer hash/eq, int keys (with growth) and struct keys |
| `method_call_args` | a dot-call converts each argument, returns a large struct through sret, and throws through a `try` |
| `multipart` | `multipart_part` extracts the "photo" part from a form-data body |
| `numeric_signedness` | C signed/unsigned rules: `fptoui`, `>>` follows the left operand, mixed-rank ops |
| `octal_literal` | leading-zero integer literals are octal (C rule) |
| `operators` | operator overloading: binary, overloads by operand type, comparison, unary, subscript, compound assign |
| `paren_deref` | `(*p)` is a dereference expression, not a cast; `(*T)x` stays a cast when T is a type |
| `local_times_stmt` | `a * expr;` with `a` a local or parameter is an expression statement, also after `case` |
| `sizeof_case_layouts` | `case sizeof(T):` over a generic instance, a sum type, a bitfield struct and an interface-holding struct reach their own cases |
| `pointer_array` | `*T[N]` is an array of N pointers, at module scope and as a local |
| `pp_macro_args` | function-like macro arguments: literals stay whole, nested calls expand first, block-comment apostrophes are ignored |
| `random` | `<random>` xoshiro256** stream from a fixed seed (regression golden) |
| `regex` | `<regex>` Thompson NFA / Pike VM syntax and capture groups |
| `short_circuit` | `&&` / `\|\|` evaluate the right operand only when needed |
| `slice` | slice `T[]` fat pointer: carries its length and aliases the backing array |
| `slice_empty_end` | an empty slice `s[len..len]` at the end is valid; a mid slice reads the right element |
| `slice_ptr` | `ptr[lo..hi]` builds a slice over heap memory |
| `slice_elem_spelling` | a slice element compares by type (`int*[]` is `*int[]`); `T[]` infers from a slice of `Box<int>` or `int*` |
| `variant_name_shadow` | a fn-typed local or parameter named like an ADT variant shadows it in calls |
| `sort` | `<sort>` generic heapsort and binary search |
| `static_const_init` | `static` locals accept array, cast and negative constant initializers and persist across calls |
| `static_local` | a `static` local keeps one instance across calls |
| `switch_defer_bitfield` | a `break` in a `switch` runs only the switch's cleanups; a bitfield write through `*Struct` reaches the pointee |
| `template_rshift_backtrack` | `x < y >> 1` parses as `x < (y >> 1)` after the template-call attempt backtracks |
| `ternary` | `?:` evaluates one arm and coexists with postfix `?` propagation |
| `test_modes_macros` | the single-file `--test-*` modes predefine the same macros as a build |
| `traits_multi` | multiple constraints `<T: Show + Eq>` |
| `traits_ok` | a fn and a struct constrained to `Ord`, instantiated with a struct that satisfies it |
| `traits_primitive` | a primitive satisfies a constraint through a free function (`int cmp(int,int)`) |
| `try_finally` | a catch-less `try`/`finally` runs the finally block and propagates to an outer catch |
| `url` | `<url>` percent-encoding and query parsing |
| `url_query_keys` | `url_query_get` decodes each key (`+` as space, %XX) before comparing it with the wanted key |
| `uuid` | `<uuid>` RFC 4122 v4 from a fixed seed |
| `finally_catch_throw` | `finally` runs when a `catch` handler throws (directly, from a call, nested) |
| `bitfield_bool` | `bool` bitfields at nonzero bit offsets, set, cleared and brace-initialized |
| `bitfield_enum_c` | `bool` and enum bitfields agree with the C layout and signedness (C companion) |
| `for_in_once` | `for (x in E)` evaluates a call, a variable-index element or a pointer-returning call once (sync and async) |
| `alias_operands` | operators on alias-typed fields, elements, returns and pointees |
| `thread_handles` | `thread_create` of a lambda, a function and a closure value; `thread_join` of a field or `?*void` |
| `async_await_struct` | awaiting futures of a struct, a generic instance and a `join2v` pair |
| `alloc_with_method` | `alloc_with` over an inline `alloc` method, through a pointer, with an alias count; a string slices to `char[]` |
| `generic_sizeof_var` | `sizeof(var)` in a generic body (and a lambda in one) measures the variable's instance type (`T`, `T[4]`, `B<T>`) |
| `generic_catch` | `catch (T e)` / `catch (Err<T> e)` in a generic body matches the instance's thrown type |
| `slice_op_index` | indexing a slice or array of a struct with an `operator []` reaches the element, not the operator; `for (v in s)` over it |
| `generic_lambda_tparam` | a lambda typed by a type parameter (`T() {...}`, `fn(T)->T`) in a generic function or inline method |
| `generic_variadic` | a generic variadic function is called variadic (`va<int>(1, 7)`, `sum<double>(2, 1.5, 2.0)`) |
| `closure_array_global` | arrays of closures as a global, a `static` and a struct field |
| `union_aggregate_init` | global and `static` union constants whose member is a struct or an array |
| `void_expr_stmt` | `return f();` of a `void` call in a `void` function or lambda (with a `defer`), `void` ternaries as statements |
| `bitfield_promote` | a bitfield whose values fit an `int` reads as `int` (C, clang output); a postfix `f++` keeps the declared type |
| `ptr_sub_same` | `p - q` across `const`, `?` and alias spellings of the same pointer type |
| `array_of_pointers` | `*T[N]` is an array of N pointers: brace init, `*ps[i]`, element stores, as a parameter |
| `alias_array_codegen` | an alias of an array as a field, through `*Alias` (`(*p)[2]`, `p[0][1]`) and as an array of structs |
| `thread_handle_ptr` | the `*void` handle of `thread_create` converts to another pointer and joins back |
| `iface_ternary_box` | `?:` arms of different conforming structs boxed into an interface (declaration, assignment, return, argument, nested) |
| `array_dim_const_expr` | array dimensions as constant expressions: C-truncating casts, `const` and enum members, hex and shifts |
| `array_dim_sizeof` | `sizeof(T)` in array dimensions (scalars, structs, a generic instance, pointers, a field) |
| `array_dim_compare_ops` | `<`, `<<`, `<=`, `>` and `?:` in array dimensions of globals, fields, parameters and locals |
| `array_alias_of_alias` | an array of an array alias is outer-dimension-first (`Arr[2]`, `Mat[4]`, `Arr[2] n`) |
| `array_alias_nested_init` | nested brace initializers whose rows are an array alias (local and global) |
| `alloc_with_generic` | `alloc_with` over a generic allocator instance: inline `alloc` method and a top-level `G_int_alloc` |
| `target_sizes` | type sizes per target (pointer width, i386 `int64`/`double` alignment), compared by `cabi_parity.sh` |
| `forin_array_alias_deref` | `for (x in *p)` with `p: *A4`, and over a field of array-alias type |
| `closure_address_ok` | addresses a lambda may take: its locals, through a captured pointer, a slice of a captured slice |
| `finally_control` | `break`/`continue` inside a `finally`, and a lambda with a `return` written there |
| `global_fn_value` | a function name as a global, `static`, array-element and struct-field initializer (a constant closure) |
| `alias_shapes` | aliases of an interface, a pointer, a generic instance, a fn type, a classic enum, a struct (operators, `*VV self`), a slice (`IS[2]`, `*IS`) and an array, used as their targets |
| `iface_payload_lambda` | a struct pointer boxed into an interface as a variant payload (generic or not) and as a lambda's return value |
| `index_result_member` | a member of an overloaded `[]` result (`w[2].v`), `.len` of an element of an array of slices |
| `c_abi_union` | unions by value across `extern` (+ `.c`): x86-64 SysV eightbytes as wide as the widest member |
| `http_chunk_size_max` | `http_chunked_step` with a chunk size near `INT64_MAX` under a limit that allows it waits for more input (the `size + 2` room check overflowed and the decoder spun) |
| `http_response_header_inject` | `HttpResponse_header` refuses (returns 0) a non-token name or a value with CR/LF (it forged a second header line), a handler's Content-Length is not sent next to the automatic one (a 304 keeps it), and the HTTP/2 encoder drops a hand-written line with a bad name or an LF |
| `http_conn_feed_incremental` | `HttpConnBuf_feed` parses the head once and decodes a chunked body as it arrives (it reparsed the whole request on every read: quadratic CPU for small reads); a chunk-size or trailer line over `HTTP_CHUNK_LINE_MAX` is 400 |
| `http_async_slow_reader` | `http_serve_async` writes answers through `net_write_async` on a non-blocking socket, so a client that never reads an 8 MB answer no longer stalls the loop and the other client (`net_set_nonblocking` was also a no-op on arm64 macOS: `fcntl` is variadic) |
| `http2_body_limit` | The HTTP/2 engine buffers at most `s.max_body` bytes per request body and `s.max_buffered` per connection: a request past either (or with a content-length past `max_body`) is answered 413 without the handler and reset with NO_ERROR (DATA used to be buffered without limit while the window credit came back) |
| `c_abi_narrow` | narrow integer params and results across `extern` (+ `.c`): `signext`/`zeroext`, callbacks and C calling Eskiu |
| `va_list_c` | a `va_list` handed to `vprintf`/`vsnprintf`, also through an Eskiu `va_list` param |
| `volatile_access` | volatile loads/stores through a volatile local or global (`*p`, `p[i]`, `p.f`, `++`, `+=`); IR count checked |
| `run_cmd/await_unplaced` | an `await` in an `asm` input of an async function: an error located at the await (`run.sh`, `cg_parity.sh`) |
| `run_cmd/net_timeval` | `<net>`'s socket timeouts pass a `struct timeval` of two C `long`s with its own size: `{i32, i32}` and optlen 8 on 32-bit ARM, `{i64, i64}` and 16 on x86-64 (it was always 16 bytes); IR checked by `run.sh` per target |
| `inline_asm_ext` | extended inline asm with inputs and a clobber (AArch64 and x86-64 spellings) |
| `async_arm_locals` | an async fn declaring locals in `match` arms and `try`/`catch`/`finally` bodies |
| `async_closure_param` | an async fn's closure param outlives the caller (heap env), awaited after the caller returned |
| `generic_enum_alias_arg` | a generic enum instantiated with an alias type argument (`Opt<F>`, a fn-type alias) |
| `alias_ptr_dotcall` | a dot-call through a pointer to an alias of a generic instance and of a struct |
| `http_chunk_line_split` | `HTTP_CHUNK_LINE_MAX` also holds for a complete chunk-size or trailer line: a 9000-byte chunk extension is 400 in one read, in small reads and in `HttpRequest_parse_status` (only an unfinished line was checked, so the verdict depended on the segmentation) |
| `http_recv_trailers` | `http_recv` drops the chunked trailer lines it has read (it rescanned the whole trailer section from the last chunk on every read: quadratic CPU); a trailer section over `HTTP_CHUNK_TRAILER_MAX` is 400 |
| `http_chunk_tiny_chunks` | `HttpConnBuf_feed` drops decoded chunked input, so a 1 MiB body in 1-byte chunks is accepted (the raw-byte limit made it 413) and 1-byte chunks with long extensions keep the buffer small; a trailer section over `HTTP_CHUNK_TRAILER_MAX` is 400 |
| `http2_header_list_limit` | The HTTP/2 engine answers 431 to a decoded header list over `H2_MAX_HEADER_LIST` (advertised as `SETTINGS_MAX_HEADER_LIST_SIZE`) or past what `s.max_buffered` leaves, without keeping its fields (only the field count was limited: a large dynamic-table entry referenced many times turned 13 KB of HPACK into 34 MB) |
| `case_decl` | a declaration directly after `case N:`/`default:` is visible in the later cases (one switch scope, C), shadows an outer variable, and may be `static` or generic |
| `async_case_decl` | case declarations in an async fn, with and without an await in the switch, shadowing and `static` |
| `generic_slice_infer` | a `T[]` parameter infers `T` from a slice argument |
| `sizeof_expr` | `sizeof(expr)` measures the expression's type without evaluating it (`*p`, `a[0]`, `p.x`, a call, in a generic and a lambda) |
| `enum_value_expr` | enum member values as constant expressions over literals, earlier members, `const` ints and `sizeof`, used as array dims, case labels and in `match` |
| `volatile_bitfield` | a bitfield through a `volatile` variable and a volatile variable's initializing store are volatile (IR count checked in `run.sh` and `cg_parity.sh`) |
| `macro_call_newline` | a function-like macro whose `(` starts a later line (past blank lines and comments) is expanded |
| `http2_host_rules` | HTTP/2 requests follow the HTTP/1.1 Host rules: two host fields or a host that differs from `:authority` is RST_STREAM PROTOCOL_ERROR, an invalid host or `:authority` 400, and without a host field the handler sees `:authority` as Host (it was dropped, and any host fields were accepted) |
| `http2_write_total` | An HTTP/2 response must be all sent within `HttpLimits.write_total_ms` of when it was ready: a peer granting one byte of window every 150 ms (simulated clock through the `H2Server` engine) gets the stream reset with CANCEL and its slot freed, and the connection answers the next request (`write_ms` counts from the last data sent, so the response stayed open forever); with `write_total_ms` 0 the deadline keeps moving |
| `tls_trickle_deadline` | The blocking TLS server's handshake and preface deadlines hold against a peer trickling one byte every 25 ms inside a record: both end after `header_ms` (SO_RCVTIMEO restarted with every byte OpenSSL's record loop read, so they never ended); a normal request still gets a 60000-byte answer through the non-blocking writes. Over an OpenSSL stand-in with a record layer (C companion `tls_trickle_deadline.c`) |

### `smoke` tests (compile + link + exit 0)

| Test | Exercises |
|------|-----------|
| `eventloop` | single-threaded echo server over the `<eventloop>` reactor |
| `http_roundtrip` | worker-pool server + client round trip |
| `http_async` | non-blocking async HTTP server (`<http_async>`) + client round trip |
| `http_async_concurrent` | concurrent async HTTP server: 3 simultaneous clients, channel wait-group shutdown |
| `threads` | `thread_create`/`thread_join`: output order is non-deterministic |
| `test_struct` | minimal struct field access |
| `arrays_md` | multidimensional array lowering, nested initializer, per-dimension index |
| `pp_if` | `#if` / `#elif` with integer constant expressions and `defined(X)` |
| `ternary_ir` | ternary lowering: branch plus result slot, arms coerced to the common type |
| `target_macros` | `_WIN64` beside `_WIN32` for every 64-bit Windows triple (dumps compared by `driver_parity.sh`) |

### `error` tests (must be rejected)

| Test | Diagnostic asserted |
|------|---------------------|
| `errors/undefined_var` | reference to an undeclared variable |
| `errors/undefined_type` | use of an undefined type |
| `errors/type_nesting_too_deep` | a type nested past the 1000-level type limit (here 1000 pointer levels) |
| `errors/undefined_field` | access to an undefined struct member |
| `errors/const_no_init` | `const` declared without an initializer |
| `errors/const_reassign` | reassigning a `const` |
| `errors/arg_count` | calling a function with the wrong argument count |
| `errors/async_no_await` | an `async` function with no `await` is rejected |
| `errors/match_duplicate` | two `match` arms for the same variant |
| `errors/const_field` | assigning to a field of a `const` value |
| `errors/const_ptr_write` | writing through a pointer-to-const (`*r = …`) |
| `errors/const_ptr_drop` | a conversion that discards a const qualifier (`int* = const int*`) |
| `errors/question_bad_return` | `?` used in a function not returning `Result` |
| `errors/unknown_intrinsic` | an `intrinsic` declared with a name the compiler can't lower |
| `errors/pp_error` | a `#error` directive aborts compilation |
| `errors/match_nonexhaustive` | a `match` missing a variant (no `_`) is rejected |
| `errors/escaping_param` | a non-`escaping` closure parameter used beyond a direct call |
| `errors/await_outside_async` | `await` used outside an `async` function |
| `errors/parse_error` | malformed syntax |
| `errors/unterminated_string` | unterminated string literal |
| `errors/unterminated_char` | malformed/unterminated char literal |
| `errors/unterminated_comment` | unterminated block comment |
| `errors/addr_of_bitfield` | rejected with "cannot take the address of bitfield 'a'" |
| `errors/addr_of_call` | rejected with "cannot take the address of this expression" |
| `errors/addr_of_literal` | rejected with "cannot take the address of this expression" |
| `errors/alias_cycle` | rejected with "type alias 'A1' refers to itself" |
| `errors/arg_type_mismatch` | rejected with "argument 1 type mismatch" |
| `errors/assign_float_to_int` | rejected with "cannot assign a floating-point value" |
| `errors/assign_rvalue` | rejected with "cannot assign to this expression" |
| `errors/assign_ternary` | rejected with "cannot assign to this expression" |
| `errors/await_in_lambda` | rejected with "await is only allowed inside an async function" |
| `errors/bitfield_float` | rejected with "must have an integer type" |
| `errors/bitfield_too_wide` | rejected with "is 40 bits wide, more than its type 'uint32' holds" |
| `errors/global_forward_ref` | a global initializer naming a later global: "undefined variable 'gsz'" (C declares a global at its definition) |
| `errors/fn_forward_global` | a function body naming a later global: "undefined variable 'gz'" |
| `errors/global_shift_range` | a global initializer is checked like any expression: "shift count 40 is out of range" |
| `errors/generic_void_field` | `Box<void>` makes a field a `void` value: "its field 'v' would be 'void'" |
| `errors/void_slice` | a `void[]` slice has `void` elements: "variable 's' cannot have type 'void'" |
| `errors/array_size_nonconst` | no variable-length arrays: "array size must be a compile-time constant, got 'n'" |
| `errors/question_float_ok` | `?` tests `ok` as a flag: "needs an integer or bool `ok` field, got 'float'" |
| `errors/union_two_members` | rejected with "a union literal initializes one member" |
| `errors/break_outside` | rejected with "'break' outside of a loop or switch" |
| `errors/call_non_fn` | rejected with "undefined function 'x'" |
| `errors/case_nonconst` | rejected with "switch case value must be a constant integer" |
| `errors/cast_ptr_float` | rejected with "cannot cast 'string' to 'float'" |
| `errors/cast_struct` | rejected with "cannot cast" |
| `errors/compound_lit_range` | rejected with "integer literal 300 is out of range for 'uint8'" |
| `errors/const_addr_arg` | rejected with "discards a const qualifier" |
| `errors/const_method_call` | rejected with "cannot call method 'set' on a read-only value" |
| `errors/const_struct_addr` | rejected with "discards a const qualifier" |
| `errors/continue_in_switch` | rejected with "'continue' outside of a loop" |
| `errors/dup_default` | rejected with "multiple 'default' labels" |
| `errors/dup_enum` | rejected with "redefinition of enum 'C'" |
| `errors/dup_enum_member` | rejected with "redefinition of enum member 'R'" |
| `errors/dup_field` | rejected with "duplicate field 'x' in struct 'A'" |
| `errors/dup_field_method` | rejected with "has the same name as a field" |
| `errors/dup_fn_global` | rejected with "is declared as both a function and a variable" |
| `errors/dup_global` | rejected with "redefinition of global variable 'G'" |
| `errors/dup_local` | rejected with "redefinition of 'x' in the same scope" |
| `errors/dup_method_fn` | rejected with "'S_get' is declared as both a method and a function" |
| `errors/dup_param` | rejected with "duplicate parameter 'a'" |
| `errors/dup_struct` | rejected with "redefinition of struct 'A' with different fields" |
| `errors/dup_struct_fn` | rejected with "is declared as both a struct and a function" |
| `errors/field_unknown_type` | rejected with "unknown type 'Nope'" |
| `errors/float_expr_to_int` | rejected with "cannot assign a floating-point value" |
| `errors/fn_chained_arg_count` | rejected with "expects 1 argument(s), got 2" |
| `errors/fn_field_arg_count` | rejected with "'cb' expects 1 argument(s), got 0" |
| `errors/fn_value_arg_count` | rejected with "'f' expects 1 argument(s), got 2" |
| `errors/fn_value_arg_type` | rejected with "argument 1 type mismatch" |
| `errors/generic_arg_conflict` | rejected with "argument 2 type mismatch" |
| `errors/generic_arg_count` | rejected with "function 'id' expects 1 argument(s), got 2" |
| `errors/generic_explicit_arg_count` | rejected with "function 'id' expects 1 argument(s), got 2" |
| `errors/generic_type_arg_count` | rejected with "generic function 'id' expects 1 type argument(s), got 2" |
| `errors/generic_uninferable` | rejected with "cannot infer type argument(s) T of generic function 'mk'" |
| `errors/iface_arity_mismatch` | rejected with "does not satisfy interface 'Shape'" |
| `errors/iface_call_arg_count` | rejected with "method 'area' expects 0 argument(s), got 2" |
| `errors/iface_return_mismatch` | rejected with "does not satisfy interface 'Shape'" |
| `errors/index_scalar` | rejected with "cannot index into a value of type 'int'" |
| `errors/index_struct` | rejected with "cannot index into a value of type 'S'" |
| `errors/init_ptr_from_int` | rejected with "cannot convert 'int' to '*int'" |
| `errors/init_string_from_int` | rejected with "cannot convert 'int' to 'string'" |
| `errors/lambda_sig_mismatch` | rejected with "incompatible function type" |
| `errors/match_dup_value` | rejected with "duplicate match value" |
| `errors/match_non_enum` | rejected with "match subject must be an enum" |
| `errors/member_of_int` | rejected with "cannot access member 'y' on non-struct type 'int'" |
| `errors/missing_return_dowhile_break` | rejected with "missing return in non-void function 'f'" |
| `errors/missing_return_labeled_break` | rejected with "missing return in non-void function 'f'" |
| `errors/missing_return_switch_break` | rejected with "missing return in non-void function 'f'" |
| `errors/must_use_method` | rejected with "result of 'C_get' must be used" |
| `errors/nullable_loop_reassign` | rejected with "cannot dereference a possibly-null pointer" |
| `errors/nullable_reassign` | rejected with "cannot dereference a possibly-null pointer" |
| `errors/nullable_shadow` | rejected with "cannot dereference a possibly-null pointer" |
| `errors/operator_redefinition` | rejected with "redefinition of function 'operator +(V, V)'" |
| `errors/param_unknown_type` | rejected with "unknown type 'Nope'" |
| `errors/proto_conflict` | rejected with "conflicting declaration of function 'f'" |
| `errors/question_non_result` | rejected with "requires a Result-like value" |
| `errors/return_in_void` | rejected with "return type mismatch: expected void" |
| `errors/shift_by_float` | rejected with "invalid operands for operator" |
| `errors/sizeof_unknown` | rejected with "unknown type 'Nope'" |
| `errors/slice_bounds_oob` | rejected with "slice bound 9 is out of bounds for array of size 4" |
| `errors/slice_bounds_order` | rejected with "slice bounds out of order" |
| `errors/string_plus_string` | rejected with "invalid operands for operator" |
| `errors/struct_init_dup_field` | rejected with "field 'v' is initialized more than once" |
| `errors/struct_init_field_type` | rejected with "field 'v': cannot convert 'string' to 'int'" |
| `errors/struct_init_generic_field` | rejected with "field 'v': cannot convert 'string' to 'int'" |
| `errors/struct_init_lit_range` | rejected with "integer literal 999 is out of range for 'uint8'" |
| `errors/struct_init_too_many` | rejected with "too many initializers for struct 'S'" |
| `errors/struct_value_cycle` | rejected with "contains itself by value" |
| `errors/template_arg_count` | rejected with "'Pair' expects 2 type argument(s), got 1" |
| `errors/template_unknown_arg` | rejected with "unknown type 'Nope'" |
| `errors/ternary_lit_range` | rejected with "integer literal 300 is out of range for 'uint8'" |
| `errors/undefined_template_fn` | rejected with "undefined template function 'nope'" |
| `errors/unknown_sig_type` | rejected with "unknown type 'Nope'" |
| `errors/unknown_var_type` | rejected with "unknown type 'Nope'" |
| `errors/void_field` | rejected with "field 'v' of 'S' cannot have type 'void'" |
| `errors/void_param` | rejected with "parameter 'x' cannot have type 'void'" |
| `errors/void_var` | rejected with "variable 'x' cannot have type 'void'" |
| `errors/c_array_global` | rejected with "Expected declaration" |
| `errors/c_array_local` | rejected with "Expected declaration" |
| `errors/global_init_call` | rejected with "is not a compile-time constant" |
| `errors/global_init_var` | rejected with "is not a compile-time constant" |
| `errors/incdec_float` | rejected with "'++'/'--' requires an integer or pointer" |
| `errors/interface_by_value` | rejected with "by value; pass a pointer" |
| `errors/lambda_break_label` | rejected with "has no enclosing loop labeled 'outer'" |
| `errors/lambda_break_outer` | rejected with "'break' outside of a loop or switch" |
| `errors/question_type_args` | rejected with "`?` can only be used in a function returning a Result with the same error type" |
| `errors/assign_struct_to_int` | rejected with "cannot convert" |
| `errors/call_global_nonfn` | rejected with "undefined function 'g'" |
| `errors/const_ptr_method_call` | rejected with "cannot call method 'set' on a read-only value" |
| `errors/deref_non_pointer` | rejected with "invalid operand for unary operator" |
| `errors/dup_param_local` | rejected with "redefinition of 'a' in the same scope" (a local reusing a parameter name) |
| `errors/extern_fnptr_closure` | rejected with "is a C function pointer" (a closure passed to an extern's fn parameter) |
| `errors/generic_dot_call_arg_count` | rejected with "method 'set' expects 1 argument(s), got 2" |
| `errors/generic_dot_call_arg_type` | rejected with "argument 1 type mismatch" |
| `errors/generic_dot_call_const` | rejected with "cannot call method 'set' on a read-only value" |
| `errors/generic_dot_call_in_generic` | rejected with "argument 1 type mismatch" |
| `errors/generic_infer_shape` | rejected with "cannot infer type argument(s) A, B" |
| `errors/generic_inst_method_arg` | rejected with "in instantiation of pick<Box>" |
| `errors/generic_inst_nested` | rejected with "in instantiation of neg<Q>" |
| `errors/generic_inst_operator` | rejected with "in instantiation of add<P>" |
| `errors/generic_struct_method_inst` | rejected with "in instantiation of Box<string>.bump" |
| `errors/iface_call_arg_type` | rejected with "argument 1 type mismatch" |
| `errors/method_arg_count` | rejected with "method 'add' expects 1 argument(s), got 2" |
| `errors/method_arg_type` | rejected with "argument 1 type mismatch" |
| `errors/method_free_fn_arg_type` | rejected with "argument 1 type mismatch" |
| `errors/pragma_link_malformed` | rejected with "malformed #pragma link" |
| `errors/pragma_pack_invalid` | `#pragma pack(3)`: an alignment other than 1, 2, 4, 8 or 16 is rejected |
| `errors/pragma_pack_push_invalid` | the same check for `#pragma pack(push, N)` |
| `errors/range_bound_float` | rejected with "range bound must be an integer" |
| `errors/range_bound_ptr` | rejected with "range bound must be an integer" |
| `errors/return_no_value` | rejected with "return type mismatch" |
| `errors/string_arith` | rejected with "invalid operands for operator" |
| `errors/struct_arith` | rejected with "invalid operands for operator" |
| `errors/switch_case_location` | rejected with ":8:14: duplicate case value" |
| `errors/switch_dup_const` | rejected with "duplicate case value in switch" (a `const` label) |
| `errors/switch_dup_folded` | rejected with "duplicate case value in switch" (folded labels) |
| `errors/unary_struct` | rejected with "invalid operand for unary operator" |
| `errors/unknown_global_type` | rejected with "unknown type 'NoType'" |
| `errors/array_2d_init_overflow` | a nested initializer row longer than its inner dimension ("elements but") |
| `errors/array_2d_oob` | an inner index past its own dimension ("out of bounds") |
| `errors/array_overflow` | an array initializer with more elements than the array holds ("holds") |
| `errors/compare_incompatible` | comparing a pointer to an integer ("invalid operands") |
| `errors/compare_struct` | comparing structs with `==` ("invalid operands") |
| `errors/const_addr_of` | writing through a plain pointer taken from the address of a const ("const") |
| `errors/crlf_line` | a CRLF file with a line continuation reports the right line ("crlf_line.esk:6:") |
| `errors/dangling_local` | returning the address of a local ("dangling") |
| `errors/defer_break` | a `break`/`continue` escaping a defer body ("may not escape") |
| `errors/defer_return` | a `return` inside a defer body |
| `errors/diag_imported_file` | an error inside an imported file names that file ("diag_file/bad.esk:4:") |
| `errors/div_by_zero` | division by a literal zero |
| `errors/empty_char` | an empty character literal `''` |
| `errors/float_to_int` | implicit floating-point to integer conversion ("floating-point") |
| `errors/fn_return_mismatch` | assigning an int-returning function to a float-returning fn type ("incompatible function type") |
| `errors/hex_no_digits` | a `0x` literal with no digits |
| `errors/import_missing` | importing a file that does not exist ("Cannot open import") |
| `errors/import_missing_std` | importing a stdlib module that does not exist ("Cannot open import") |
| `errors/incdec_nonlvalue` | `5++` on a non-lvalue ("modifiable") |
| `errors/index_oob` | a constant index past a fixed array's length ("out of bounds") |
| `errors/init_incompatible` | a string initializing an int ("cannot convert") |
| `errors/init_void` | a void call initializing an int ("cannot convert") |
| `errors/keyword_as_name` | a reserved keyword as a variable name, reported at the cause ("expected a name, found keyword 'fn'") |
| `errors/keyword_field` | a reserved word as a struct field or method name ("keyword") |
| `errors/labeled_break_defer` | a labeled `break`/`continue` escaping a defer body ("may not escape it") |
| `errors/labeled_break_unknown` | a label with no enclosing loop of that name ("has no enclosing loop labeled") |
| `errors/literal_out_of_range` | `int8 x = 300` ("out of range") |
| `errors/main_void` | `void main()` ("must return int") |
| `errors/match_plain_nonexhaustive` | a `match` on a plain enum that misses variants ("non-exhaustive match") |
| `errors/missing_return` | a non-void function that falls off the end |
| `errors/missing_return_if` | a non-void function ending in an `if` without `else` |
| `errors/must_use_discarded` | discarding the result of a `must_use` call (`alloc<T>`) ("must be used") |
| `errors/number_suffix` | `0b101`: binary literals are not supported ("invalid suffix 'b101'") |
| `errors/number_underscore` | `1_000`: digit separators are not supported ("invalid suffix '_000'") |
| `errors/octal_bad_digit` | `08` ("invalid digit '8' in octal literal") |
| `errors/octal_bad_digit2` | `019` ("invalid digit '9' in octal literal") |
| `errors/operator_as_name` | the `operator` keyword as a variable name ("expected a name, found keyword 'operator'") |
| `errors/parse_array_field` | a C-style `int arr[4];` field ("Expected ';' after field") |
| `errors/parse_catch_colon` | `catch (e: string)` ("Expected variable name in catch") |
| `errors/parse_error_located` | a syntax error is located and recovery resumes at the next declaration ("parse_error_located.esk:7:5: Expected ';'") |
| `errors/parse_import_no_semi` | an `import` without `;` |
| `errors/parse_leading_dot` | a float literal with a leading dot, `.5` ("Expected expression, got DOT") |
| `errors/parse_missing_operand` | `3 +;` ("2:25: Expected expression, got SEMICOLON") |
| `errors/parse_postdec_literal` | `x--1` ("Expected ';'") |
| `errors/parse_return_no_semi` | `return 0 }` ("2:23: Expected ';'") |
| `errors/pp_if_bad_expr` | a malformed `#if` expression ("in #if expression") |
| `errors/pp_if_zero_div` | division by zero in an `#if` expression |
| `errors/pp_include` | `#include` ("#include is not supported") |
| `errors/pp_missing_endif` | an `#if` with no `#endif`, located at the `#if` ("unterminated conditional directive") |
| `errors/pp_stray_else` | a second `#else` in one conditional ("#else after #else") |
| `errors/pp_stray_endif` | an `#endif` with no `#if` |
| `errors/pp_stringify` | the `#x` stringification operator ("stringification") |
| `errors/pp_unknown_directive` | an unknown directive `#frobnicate`, located ("unknown preprocessor directive") |
| `errors/redefinition` | defining the same function twice ("redefinition") |
| `errors/static_nonconst_init` | a `static` local with a runtime initializer ("constant") |
| `errors/static_on_global` | `static` on a global ("static") |
| `errors/switch_dup_case` | two `case 1:` labels in one switch ("duplicate case value") |
| `errors/ternary_incompatible` | ternary arms of type string and int ("incompatible") |
| `errors/trait_primitive_unsat` | `float` has no `cmp` free function, so it does not satisfy `Ord` ("does not satisfy constraint") |
| `errors/trait_unsatisfied` | `int` with no `cmp` free function used for a `<T: Ord>` param ("does not satisfy constraint") |
| `errors/unexpected_char` | a stray `@` in an expression, located ("unexpected character '@'") |
| `errors/uninitialized` | reading a local before it is assigned ("uninitialized") |
| `errors/va_start_not_variadic` | `va_start` in a function without `...` |
| `errors/va_start_not_va_list` | `va_start` on an `int8` local ("needs a 'va_list'") |
| `errors/va_arg_not_va_list` | `va_arg<int>(5)` ("needs a 'va_list'") |
| `errors/va_arg_struct` | `va_arg<P>` of a struct ("cannot read a 'P'") |
| `errors/thread_create_int` | `thread_create(5)` |
| `errors/thread_create_wrong_fn` | `thread_create` of a `fn(int)->int` |
| `errors/thread_join_int` | `thread_join(5)` ("expects a thread handle") |
| `errors/free_closure_int` | `free_closure(5)` |
| `errors/bitfield_adt_enum` | a sum type as a bitfield type |
| `errors/bitfield_zero_width` | a named zero-width bitfield `int x : 0` |
| `errors/bitfield_unnamed_too_wide` | an unnamed bitfield wider than its type |
| `errors/bitfield_unnamed_float` | an unnamed bitfield of a non-integer type (in a union) |
| `errors/bitfield_unnamed_negative` | a negative unnamed bitfield width is a parse error |
| `errors/bitfield_unnamed_member` | an unnamed bitfield is no member |
| `errors/bitfield_unnamed_literal` | a positional literal has no value for an unnamed bitfield |
| `errors/method_as_field_write` | assigning an inline method as a field (`p.sum = 3`) |
| `errors/method_as_field_read` | reading an inline method as a field (`int s = p.sum`) |
| `errors/string_slice_elem` | a string slice into an `int[]` ("cannot convert 'char[]'") |
| `errors/alloc_with_count_string` | `alloc_with` with a string count |
| `errors/alloc_with_no_alloc` | `alloc_with` on an `int` allocator ("has no alloc method") |
| `errors/ptr_sub_mismatch` | `*int - *char` ("pointer subtraction needs pointers to the same type") |
| `errors/nullable_global_operator` | a user operator call ends a global's `?*T` narrowing |
| `errors/nullable_global_operator_cond` | an overloaded `==` after the null test in the same condition |
| `errors/nullable_global_lambda` | a global narrowed where a lambda is written is not narrowed in its body |
| `errors/nullable_static_call` | a call ends a `static` local's narrowing (recursive call nulls it) |
| `errors/nullable_static_lambda` | a closure call ends a `static` local's narrowing |
| `errors/alloc_with_by_value` | `alloc_with` of a by-value allocator |
| `errors/alloc_with_unknown_elem` | `alloc_with` of an unknown element type |
| `errors/alloc_with_void_elem` | `alloc_with` of `void` |
| `errors/alloc_with_bad_ret` | an `_alloc` returning `int` |
| `errors/alloc_with_bad_params` | an `_alloc` without the size parameter |
| `errors/ptr_array_from_addr` | `*int[3] p = &arr` (an array of pointers from an array's address) |
| `errors/ptr_array_deref` | `*p` of a `*int[3]` |
| `errors/struct_cycle_array_alias` | a struct holding itself through an alias of an array of it |
| `errors/thread_handle_to_int` | `int t = thread_create(...)` |
| `errors/ternary_unrelated_ptrs` | `?:` of pointers to two unrelated structs into a `*A` |
| `errors/iface_ternary_nonconforming` | `?:` into an interface with an arm that does not satisfy it |
| `errors/va_arg_float` | `va_arg<float>` (promoted to `double`) |
| `errors/va_arg_narrow_int` | `va_arg` of an alias of `int8` (promoted to `int`) |
| `errors/array_dim_cast_zero` | `int[(uint8)256]` folds to 0 |
| `errors/array_dim_sizeof_zero` | `int[sizeof(int) - 4]` folds to 0 in the type checker |
| `errors/nullable_global_alloc_with` | `alloc_with` ends a global's narrowing (the alloc method nulls it) |
| `errors/nullable_global_thread_join` | `thread_join` ends a global's narrowing |
| `errors/nullable_global_thread_create` | `thread_create` ends a global's narrowing |
| `errors/nullable_guard_else_assign` | an early-exit guard whose `else if` branch assigns the pointer |
| `errors/nullable_guard_then_assign` | an early-exit guard (else exits) whose then branch assigns the pointer |
| `errors/not_void_call` | `!i.m()` of a void interface call |
| `errors/throw_void` | `throw v()` of a void call |
| `errors/deref_void_ptr` | `*x;` of a `*void` |
| `errors/cast_void_call` | `(int)v()` |
| `errors/cast_int_to_struct_generic` | `(T)0` with T = a struct, in a generic instance |
| `errors/cast_array_literal` | `((S){1}).a` |
| `errors/generic_void_param` | `id<void>(v())` makes a parameter `void` |
| `errors/closure_captured_address` | `*(&n) = 5` of a captured `n` in a lambda |
| `errors/closure_captured_slice` | a slice of a captured array in a lambda |
| `errors/finally_return` | a `return` inside a `finally` block |
| `errors/finally_question` | a `?` inside a `finally` block (it would swallow the exception being unwound) |
| `errors/match_alias_nonexhaustive` | a `match` on an alias of a classic enum missing a member |
| `errors/void_logical_operand` | a `void` call as an operand of `&&` |
| `errors/void_compare` | comparing two `void` calls |
| `errors/void_variadic_arg` | a `void` call passed through `...` (`printf("%d", hi())`) |
| `errors/case_decl_redefined` | two cases of one switch declaring the same name (one scope) |
| `errors/case_decl_out_of_scope` | a case's declaration used after the switch |
| `errors/index_oob_sizeof_struct` | an index `sizeof(P)` out of bounds (a struct's size folds) |
| `errors/switch_dup_sizeof_ptr` | two case labels equal once `sizeof` of a pointer and of a struct fold |
| `errors/switch_dup_sizeof_generic` | `sizeof` of a generic struct instance and of a generic sum type fold, so equal case labels are seen |
| `errors/switch_dup_sizeof_adt` | `sizeof` of a sum type folds for the duplicate `case` check |
| `errors/switch_dup_sizeof_bitfield` | `sizeof` of a bitfield struct folds with the target C layout |
| `errors/switch_dup_sizeof_iface` | `sizeof` of a struct holding an interface value folds |
| `errors/const_div_zero_sizeof_expr` | a divisor `sizeof(*p) - 2` that folds to zero |
| `errors/enum_value_not_const` | an enum member value that reads a non-`const` global |
| `errors/enum_value_expr_range` | a folded enum member value that does not fit an `int` |
| `errors/variant_nullary_parens` | a payload-free variant written with parentheses (`B()`) |

### `lint` tests (-Wall)

| Test | Warnings asserted |
|------|-------------------|
| `warnings/wall_uses` | -Wall uses: dot-call, interface and operator functions, prototype and lambda parameters |

## Previously known issues (now fixed, and guarded by tests)

Two real compiler bugs were found while building this suite. Both are now fixed and
covered by exact-match `run` tests, so a regression would fail `run.sh`.

1. **`union` float member store**: *fixed.* `v.f = 3.14` stored the full 8-byte
   double into the union's offset-0 storage without truncating to `float`, so a
   later read got garbage (`126443839488.0`). The assignment path now coerces the
   RHS to the LHS member's declared scalar type. Guarded by `sizeof_union_ptr.esk`.

2. **No forward function resolution**: *fixed.* Codegen emitted functions in source
   order, so a call to a function defined later (or a `int b(int n);` forward
   declaration) failed with `Undefined variable or function`. Codegen now declares
   all prototypes in a pre-pass before emitting bodies, enabling call-before-define
   and mutual recursion. Guarded by `forward_decl.esk`.

## Other test scripts

Besides `run.sh`, these run in CI:

- `tests/opt_differential.sh`: every `tests/*.esk` must give the same exit code and
  stdout at `-O0` and `-O2`.
- `tests/safe_mode.sh`: `--safe` bounds checks trap on an out-of-range index and are off
  by default.
- `tests/nullable.sh`: the `?*T` checks and narrowing.
- `tests/type_zoo/snapshot.sh check`: the golden-IR oracle (see `docs/dev/debugging.md`).
- `tests/selfhost/*.sh`: parity between the self-hosted compiler and the C++ one
  (lexer, parser, preprocessor, type checker, codegen, C ABI, driver) and the bootstrap
  fixpoint; see `docs/dev/self-hosting.md`.

## Fuzzers

The fuzzers live in `tests/fuzz/`. Each run is deterministic for a given `--seed`, writes
what it finds to `tests/fuzz/findings/` (ignored by git), and exits non-zero when it finds
anything. Every compiler and program invocation runs under a timeout and a 4 GB resident
memory cap (`fuzz_util.py`), and a timeout or blowup counts as a finding. Parallel work is
limited to 4 jobs by default (`--jobs`).

**`eskiu_fuzz.py`** has two parts.

- The mutation and generation loop (`--iterations N`) feeds programs to `eskiuc` and
  reports crashes, IR verifier failures, hangs, and programs whose output differs between
  `-O0` and `-O2`.
- The C oracle (`--oracle N`, module `c_oracle.py`) generates programs inside a
  C-translatable subset of Eskiu: integers of every width and signedness, `bool` and
  `char`, casts, arithmetic, bitwise, shift and comparison operators, the ternary,
  `if`/`while`/`for`/`do`/`switch`/`break`/`continue`, nested blocks that shadow outer
  names, arrays, structs, pointers and pointer difference, pure and side-effecting
  functions, globals with constant initializers, `static` locals and `defer`. Each
  program is also emitted as C, built with `clang -O0 -fwrapv`, and the C++ `eskiuc` (at
  `-O0` and `-O2`) and the self-hosted `eskiuc-esk` must print exactly what it prints.
  This is the only check outside the Eskiu compilers, so it catches a bug both compilers
  share. Undefined behavior is avoided the same way on both sides (divisors and shift
  counts are guarded, indices masked, side effects kept where C fixes the order); `defer`
  has no C counterpart, so the C side runs the deferred statements at every exit.
- The second generator (`--oracle-ext N`, module `c_oracle_ext.py`) covers what the first
  does not: `float` and `double` arithmetic and conversions, structs passed and returned
  by value (small ones in registers, large ones through sret), arrays of structs, unions
  read through another member, and user variadic functions with mixed arguments. It is
  checked the same way (`c_oracle.check_program`).

```bash
python3 tests/fuzz/eskiu_fuzz.py --iterations 300 --seed 1           # mutation loop
python3 tests/fuzz/eskiu_fuzz.py --oracle-only --oracle 200 --oracle-ext 100 --seed 1   # the CI oracle gate
python3 tests/fuzz/eskiu_fuzz.py --oracle-only --oracle 20000 --seed 5   # a long run
python3 tests/fuzz/eskiu_fuzz.py --oracle-repro 5:1234    # print one program (Eskiu + C)
python3 tests/fuzz/eskiu_fuzz.py --oracle-reduce 5:1234   # shrink a finding
```

A finding is named `oracle_<build>-<kind>_<seed>_<index>` (for example
`oracle_self-diff_5_1234.esk` with its `.c` twin); `--oracle-reduce` deletes lines and
whole blocks while the finding still reproduces and writes `reduced_<seed>_<index>.*`.
Set `ESKIU_CLANG` to the clang to use, and `ESKIUC_ESK` if the self-hosted compiler is
not at `build/eskiuc-esk` (it is skipped when missing).

**`neg_fuzz.py`** is the negative corpus. It takes a program from the C-oracle generator
and injects exactly one error (about sixty kinds: undefined names, members of scalars,
bad calls and argument types, operators on structs or strings, bad casts, assigning to
an rvalue or a const, stray `break`/`continue`, duplicate locals and parameters,
out-of-range literals, division by a literal zero, unknown types, bad struct and array
literals, duplicate or non-constant `case` labels, non-constant globals and statics,
missing returns, unchecked `?*T` derefs, and more) at a random point of the program. Both
compilers must reject it without crashing, with an `error: file:line:col:` diagnostic on
the line of the injected error. Findings are `neg_<build>-<kind>_<seed>_<index>.esk`
(ACCEPT, CRASH, HANG, UNLOCATED, LOCATION).

```bash
python3 tests/fuzz/neg_fuzz.py --programs 300 --seed 1     # the CI gate
python3 tests/fuzz/neg_fuzz.py --programs 20000 --seed 7   # a long run
python3 tests/fuzz/neg_fuzz.py --repro 7:123               # print one program
```

**`stdlib_fuzz.py`** fuzzes the stdlib parsers under AddressSanitizer. Each target in
`tests/fuzz/stdlib/` (`json`, `base64`, `url`, `regex`, `hpack`, `http`, `fetch`, `multipart`,
`uuid_time`, `csv`, `dotenv`, `args`, `time_parse`) is an Eskiu program built with `eskiuc --asan`
that reads a batch of inputs and runs them through the parser, checking round trips where there
is one (base64 and url encode/decode and url_parse, hpack encode/decode, epoch/calendar
conversion, the fetch response parser fed whole and in small pieces, csv and dotenv write/parse,
and time_format/time_parse). The `args` target splits an input at NUL bytes into an argv. The driver mutates a
seed corpus (hand-written seeds plus string literals from the matching tests) and fails
on a crash, an ASan report, a broken invariant, a hang or a memory blowup, saving the
input to `stdlib_<target>_<seed>_<n>.bin`.

```bash
python3 tests/fuzz/stdlib_fuzz.py --inputs 1500 --seed 1   # the CI gate (a few seconds a target)
python3 tests/fuzz/stdlib_fuzz.py --seconds 300 --seed 7   # five minutes per target
python3 tests/fuzz/stdlib_fuzz.py --targets hpack --replay tests/fuzz/findings/x.bin
```

## Linux smoke from a Mac (pre-release)

`tests/linux_docker.sh` checks a release on Linux without a Linux build of `eskiuc`. It is
a manual pre-release check, not a CI gate (it needs Docker). The compiler cross-compiles
every run and smoke test and the self-hosted drivers for `$TRIPLE` (default
`aarch64-unknown-linux-gnu`), and a container (`ubuntu:24.04` plus gcc, built once and
cached) links and runs them:

- each run test must print exactly its `.expected`, each smoke test must exit 0;
- `esk_main --test-typechecker` (over `tests/*.esk` and `tests/errors/*.esk`) and
  `--test-codegen` (over `tests/*.esk`) must not crash or time out;
- `cg_main` compiles `cg_main.esk` on Linux, the host clang assembles that IR, and the
  result must emit the same IR again (the bootstrap fixpoint, on Linux).

A test that is a known failure on the target (listed in the script, with its CHANGELOG
entry) is reported as XFAIL and does not fail the run.

```bash
CLANG=$(brew --prefix llvm@22)/bin/clang ESKIUC=build/eskiuc tests/linux_docker.sh
TRIPLE=x86_64-unknown-linux-gnu tests/linux_docker.sh    # x86-64, emulated on Apple silicon
TESTS="regex http2_server" tests/linux_docker.sh         # only these tests
```

## Adding a test

- **Positive, deterministic output:** add `NAME.esk` and `NAME.expected` (the exact
  stdout). Generate the expected file from a *known-correct* run, then read it to
  confirm it is right before committing.
- **Positive, non-deterministic / smoke only:** add `NAME.esk` with no `.expected`.
- **Negative:** add `errors/NAME.esk` whose first line is
  `// EXPECT-ERROR: <substring of the diagnostic>`.
- **Lint:** add `warnings/NAME.esk` with one `// EXPECT-WARNING: <substring>` line per
  expected `-Wall` warning (no such line means the file must produce no warning).
