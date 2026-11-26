# Patterns and pitfalls

The semantics that shape everyday Eskiu code, each with the form that works and the
mistake people make, and the message the compiler gives for it. [spec.md](spec.md) has the
full rules; the sections below link to them.

Every example here is a test: the working ones are in
[tests/guide_semantics.esk](../../tests/guide_semantics.esk) (its output is checked on every
change, with both compilers), and each mistake is a file in `tests/errors/guide_*.esk` that
must fail with the message shown.

## Captures are copies

A lambda captures the variables it uses **by value**, when the lambda expression runs. A
later change to the variable is not seen by the closure, and the closure cannot change the
variable ([spec 6.5](spec.md#65-lambdas-and-closures)).

```eskiu
int base = 10;
let add: fn(int)->int = int(int x) { return x + base; };
base = 20;
add(1);                          // 11: the closure kept base = 10
```

To share state, capture a pointer and write through it:

```eskiu
int total = 0;
*int t = &total;
let bump: fn(int)->void = void(int x) { *t = *t + x; };
bump(2);
bump(3);                         // total is 5
```

The mistake is assigning to the captured variable itself:

```eskiu
int n = 1;
let f: fn()->int = int() {
    n = 2;                       // error
    return n;
};
```

```
error: cannot assign to captured variable 'n': a closure captures it by value (use a pointer, a global, or a static)
```

Taking the address of a captured variable (`&n`), writing to a field of a captured struct
or calling a method that changes it are rejected for the same reason: the write would reach
only the closure's copy.

## A closure that outlives the call

A closure only called during the call keeps its captures on the stack and costs nothing to
clean up. One that is kept after the call (stored, returned, handed to a thread) needs its
captures on the heap, so the parameter that receives it is marked `escaping`, and the
closure is released with `free_closure` once nothing uses it.

```eskiu
fn()->int saved;
void keep(escaping fn()->int f) { saved = f; }

int n = 41;
keep(int() { return n + 1; });
saved();                         // 42
free_closure(saved);
```

Without `escaping` the compiler stops the closure from outliving its stack frame:

```
error: closure parameter 'f' escapes (used beyond a direct call); mark it `escaping`
```

## Pointers do not keep anything alive

A pointer is an address; it does not extend the life of what it points to.

- A local lives until its function returns. Returning its address is an error:

  ```eskiu
  int* make() { int x = 5; return &x; }
  ```

  ```
  error: returning the address of local 'x' (dangling pointer)
  ```

  Return the value, or allocate it with `alloc<T>` and give the caller the job of freeing it.
- The compiler catches the direct case only. A pointer to a local stored in a struct, a
  global or a closure that outlives the function is not diagnosed: the rule above still
  holds.
- A pointer into a `List`'s elements (`&list.data[i]`) is valid until the list grows:
  `List_push` past its capacity moves every element to new storage and frees the old one.
  Keep an index instead of a pointer when the list may grow.
- A slice (`T[]`) points into the array or buffer it was taken from, with the same lifetime
  as that storage.

## Who frees what

Memory from `alloc<T>` is freed with `free`, once, by whoever owns it; the type system does
not track ownership, so document it at each function that returns or takes allocated
memory ([spec 11](spec.md#11-memory)). Two helpers keep cleanup next to the allocation:

- `defer` runs its statement when the block exits, on every path: normal end, `return`,
  `break`, `continue` ([spec 7.8](spec.md#78-defer)).
- `errdefer` runs it only when the function leaves through `?` (an error propagated from a
  call), not on a plain `return`, `return Err(...)` included; it undoes partial work when a
  fallible step fails, for a resource handed to the caller on success.

```eskiu
Result<int, string> parse_two(string a, string b) {
    *int buf = alloc<int>(2);
    defer free(buf);             // runs on each of the three returns
    if (strlen(a) == 0) return Err<int, string>("empty first");
    buf[0] = atoi(a);
    if (strlen(b) == 0) return Err<int, string>("empty second");
    buf[1] = atoi(b);
    return Ok<int, string>(buf[0] + buf[1]);
}
```

Without `defer`, each early return needs its own `free`, and the first one forgotten leaks.

## Initialization

- `alloc<T>(n)` returns zeroed memory: a pointer field is `null`, a number `0`, a `List`
  empty and usable.
- A struct literal sets the fields it names and gives every other field its declared
  default, or zero when it has none ([spec 8.3](spec.md#83-struct-initialization)).
- A scalar local declared without a value holds zero, but reading it before any assignment
  is an error, because it is almost always a mistake:

  ```eskiu
  int total;
  return total;
  ```

  ```
  error: use of uninitialized variable 'total'
  ```

  A read that only some paths reach without an assignment is not an error; `-Wall` warns
  about it ("may be used uninitialized"). Arrays and structs are not tracked: they start
  zeroed, and setting them element by element is normal.

## Records and arrays are values

Assigning, passing or returning a struct or a fixed-size array copies all of it. A function
that changes its parameter changes its own copy:

```eskiu
struct Big { int[64] v; int len; }

void change_copy(Big b) { b.v[0] = 99; }          // the caller's value is unchanged
void change_through(*Big b) { b.v[0] = 99; }      // changes the caller's
int first(const Big* b) { return b.v[0]; }      // reads without copying

int[3] x = {1, 2, 3};
int[3] y = x;                    // a copy: changing y leaves x as it was
```

The copy costs time proportional to the record's size. A record over 16 bytes passed to an
Eskiu function goes as a pointer to a copy the caller makes, and assigning one copies its
bytes with `memcpy`; neither is free for a large record. To read a large record without
copying it, take a `const T*`; to change the caller's, take a `*T`.

## Crossing into C

`extern` declares a C function and calls it with the platform's C calling convention,
structs passed by value included, as Clang would for the same signature
([spec 13](spec.md#13-extern--c-interop)). A struct's fields are laid out as C lays out the
same fields, so a pointer to it can be handed to C.

What changes at the boundary:

- Between Eskiu functions a record over 16 bytes passes as a pointer to a copy; an
  `extern`, an `export` function and a function used as a C callback keep the C convention.
  Code compiled by an older Eskiu does not link with code compiled after 0.16.2, which
  introduced this: rebuild everything.
- A `string` is a pointer to NUL-terminated bytes, as C's `const char*`. A slice (`T[]`) is
  a pointer and a length: pass `&s[0]` and `s.len` to C, not the slice.
- A closure (`fn(T)->R`) is two pointers and cannot be called from C; pass a top-level
  function cast to a pointer type instead ([spec 13.5](spec.md#135-passing-an-eskiu-function-as-a-c-callback)).
- Memory C allocates is freed with C's `free`; memory from `alloc<T>` with Eskiu's `free`.
  On hosted targets both are the C allocator, but mixing them makes code wrong the day one
  side changes.
