# IR notes (results of the phase 1 experiments)

What was checked by building the hand-written `.ll` files in `docs/design/ir-experiments/` with clang 18.
They are samples of the shape the compiler emits.

| What was checked | File | Result |
| --- | --- | --- |
| The smallest Hello World (opaque `ptr`) | `hello.ll` | works with `clang hello.ll` |
| Calling the runtime (C) | `rt_call.ll` | `clang a.ll zevic_rt.c` links them. A Panic exits with code 101 |
| `I32` addition and the overflow check | `overflow.ll` | `llvm.sadd.with.overflow.i32`, then `zevic_panic` if it happened |
| `String` = `{ ptr, i64 }` | `string_enum.ll` | constants and `extractvalue` work |
| `enum` = `{ i32 tag, [N x i64] }` | `string_enum.ll` | a variant's struct can be `store`d to and `load`ed from the payload area |
| `interface` value = `{ ptr, ptr }` | `iface_closure.ll` | read a function from the start of the function table and call it indirectly |
| Closure = `{ ptr fn, ptr env }` | `iface_closure.ll` | the environment is the first argument of the function |

## Rules carried into the implementation

- A module has no `target triple`. Passing it to `clang` gives a warning, so the driver adds `-Wno-override-module`.
- The payload of an `enum` is `[N x i64]` (N is the largest variant rounded up to 8-byte units). Without a payload it is `{ i32 }`.
- The `this` passed to a function is always a `ptr` (for a `class` it is the reference itself, for a `struct` / `enum` the place where the value lives).
- Aggregates such as `String` are **passed by pointer** to the runtime. The result comes back through a leading output pointer (to avoid ABI differences).
- All `alloca`s go into the entry block of the function (none grow inside loops).
- No instruction may follow `unreachable` or `ret`: after a terminator, a new (unreachable) block is opened.
