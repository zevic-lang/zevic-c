# Zevic implementation plan

Based on the specification ("Zevic language specification"), this plan implements things in order, **from Hello World to a compiler written in Zevic that can build itself (self-hosting)**.

- The name is final (see phase 0).
- The first compiler, `zevic0`, is written in **C (C11)**. Its output is **textual LLVM IR**. Building and linking are done by **clang**.
- At first there is no GC (`malloc` and leave it). It is replaced by the Boehm GC in stage 5.
- Each stage is a unit in which "more things work". Do not start on the next feature until the stage is finished.

---

## 0. Overall flow

| Phase | Content | Done when | Release |
| --- | --- | --- | --- |
| Phase 0 | Preparation (name, repositories, environment) | the repositories exist and CI is green | |
| Phase 1 | Experiments with hand-written LLVM IR | Hello World runs from a `.ll` | |
| Stage 1 | Hello World | `zevic hello.zev -o hello` works | v0.0.1 |
| Stage 2 | Computation and control | FizzBuzz and factorial work | v0.0.2 |
| Stage 3 | Data and minimal generics | `struct`, `enum`, `match`, minimal generics, the standard library's `Option`, `Result`, `Array` and `String` | v0.0.3 |
| Stage 4 | Classes and C interop | `class`, `interface`, generic constraints, the standard library's `Map`, `extern` and `export` | v0.1.0 (public) |
| Stage 5 | Abstractions | closures, `defer`, `params`, the Boehm GC | v0.2.0 |
| Stage 6 | Tools | `zevpm`, `test`, `zevic.toml`, the second batch of the standard library (including `Process`) | v0.3.0 |
| Stage 7 | Self-hosting | the compiler written in Zevic, `zevfmt`, bootstrapping | v0.4.0 |
| Polish | Preparing for 1.0 | freeze the specification, documentation, distribution | v1.0.0 |

**Working principles**

1. At the start of each stage, first write the **samples and tests** that the stage has to pass (`examples/` and `tests/`).
2. When the specification and the implementation disagree, **fix the specification first**. The implementation does not get ahead.
3. At the end of each stage, tag it and add one line to `CHANGELOG.md`.
4. Leave "speed" and "helpful error messages" for after stage 6. **Working correctly** comes first.

---

## Phase 0: Preparation

### 0-1. Name

- [x] The name of the language is "Zevic" (the GitHub organization is `zevic-lang`)

### 0-2. Repositories

- [ ] Create a GitHub organization (private)
- [ ] Create the four repositories `.github`, `docs`, `zevic` and `examples`
- [ ] Put the specification (`spec/zevic-spec.md`) and `proposals/0000-template.md` in `docs`
- [ ] Put samples 01 to 10 in `examples` (marked as drafts)
- [ ] Add `LICENSE` (Apache-2.0) and `README.md`

### 0-3. Development environment

- [ ] Install clang 15 or newer (`clang --version`). On Windows, the Visual Studio Build Tools too.
- [ ] Install `make`, `git` and a C compiler (clang 15 or newer)
- [ ] Concentrate on one OS first (the one you know best). Other OSes are added to CI in stage 6.
- [ ] Get the Boehm GC sources and check the license (it is bundled in stage 5)

### 0-4. The first CI

- [ ] Make a small CI on GitHub Actions in which the Linux build and tests pass (the content can be almost empty for now)

**Done when**: there are four private repositories and CI is green. The name is final.

---

## Phase 1: Experiments with hand-written LLVM IR

Before writing the compiler, write by hand the **shape the compiler should emit** and check it. This becomes the model for the design from then on.

### 1-1. The smallest `.ll`

```
; hello.ll
@.str = private constant [14 x i8] c"Hello, World!\00"
declare i32 @puts(ptr)
define i32 @main() {
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}
```

- [ ] It works with `clang hello.ll -o hello` (clang 15 or newer, opaque pointers, `ptr`)
- [ ] Check that it works on Windows too (linking an `.exe`)

### 1-2. Write the runtime in C and call it from `.ll`

- [ ] `runtime/zevic_rt.c`: `zevic_alloc`, `zevic_panic` and `zevic_print_str`
- [ ] `declare` and call them from `.ll`, and link with `clang a.ll zevic_rt.c -o a`

### 1-3. Try the representation of types

Check the table "Rules for generated code" in the specification by hand.

- [ ] `String` = `{ ptr, i64 }` (with a trailing NUL)
- [ ] `I32` addition and the overflow check (`llvm.sadd.with.overflow.i32`), then `zevic_panic` if it happened
- [ ] `struct` = an LLVM struct. `class` = a `ptr` to a struct on the GC heap
- [ ] `enum` = a tag + the area of the largest variant
- [ ] `interface` value = `{ ptr, ptr }` (the object and the function table)
- [ ] Function value (closure) = `{ ptr function, ptr environment }`

**Deliverable**: `docs/design/ir-notes.md` (the results of the checks above). **Done when**: everything works by hand.

---

## Stage 1: Hello World

**Goal**: build and run the following program.

```
module Main

import Zevic.Console

fn Main() {
    Console.Println("Hello, World!")
}
```

### What to do

| Part | Content |
| --- | --- |
| `lexer.c` | UTF-8, identifiers, reserved words, integer and string literals, comments, handling of newlines |
| `parser.c` | `module`, `import`, `fn`, call expressions, string literals. A recursive descent parser |
| `ast.c` | The AST definition and a dump (`--dump-ast`) |
| `resolve.c` | Resolving `module` / `import`, including the omission of `Zevic.`. Searching the standard library |
| `check.c` (skeleton) | The representation of types, checking function declarations and calls |
| `emit_ll.c` | Emits functions, string constants and calls into `.ll` |
| `driver.c` | Finds and calls `clang`. `-o` and `--emit-ll` |
| `runtime/` | `zevic_alloc` (`malloc`), `zevic_panic`, `Console.Print` / `Println` |
| Generating `main` | Generates C's `main(int, char**)`, which calls `Main` and returns the exit code |

### Error messages

- The file name, line and column, the offending line, and one sentence about the cause. Put them in from the start (cheaper than adding them later).

### Tests

- [ ] Golden tests: `tests/NN-name/input.zev`, `expected_output.txt` and `expected_exit_code`
- [ ] `tests/run.py` runs all the tests and shows the differences
- [ ] Error cases: no `module`, no `Main`, several of them, an undefined name, a syntax error

**Done when**: `zevic hello.zev -o hello` works and passes in CI (Linux). **v0.0.1**.

---

## Stage 2: Computation and control

**Goal**: programs with only numbers and control, such as FizzBuzz, factorial and primality tests.

### Language features

- [ ] Integer types `I8` to `I64` and `U8` to `U64`, `F32` and `F64`, `Bool`, `Char` and `Unit`
- [ ] Literals (`1_000`, `0xFF`, `0b1010`, `1e3`, `255_U8`, characters, escapes)
- [ ] `let` / `mut`, assignment, `+=` and so on
- [ ] Operators and precedence (as in the table of the specification), comparison, logic, bit operations
- [ ] **Overflow checks** (add, subtract, multiply, shifts, casts), division by zero, `Panic` (exit code 101)
- [ ] `if` / `else` (an expression), `while`, `loop`, `break`, `continue`, `for (i in 0..n)`
- [ ] Functions (parameters, return values, `return`, recursion)
- [ ] `Panic` and `Assert`
- [ ] String interpolation `"${x}"` (basic types only), `Console.Printf` (types and count checked at compile time)
- [ ] Type conversions (`ToI64`, `ToF64`; the narrowing ones that return a `Result` come in stage 3)

### Checks

- [ ] No implicit conversions, type mismatches, reassigning a `let`, uninitialized variables, the return type, unreachable code
- [ ] Whether every branch returns a value (`if` as an expression)

### Implementation notes

- Separate type checking from IR generation (`check.c` builds the typed AST, and `emit_ll.c` looks only at that).
- Use the typed AST itself as the intermediate representation. Do not build a separate IR until it is needed.

**Done when**: the FizzBuzz, factorial and prime tests pass. **v0.0.2**.

---

## Stage 3: Data

**Goal**: programs that handle data structures (a simple calculator, the first half of a word count).

### Language features

- [ ] **Minimal generics** (type parameters, inference, **monomorphization**; no constraints yet). They are put in this stage to write the standard library's `Option`, `Result` and `Array`.
- [ ] Tuples, and destructuring tuples `let (a, b): ...`
- [ ] `struct` (`Init`, `New`, `this`, methods, `==`, copying values)
- [ ] `enum` (three kinds of variants, methods) and `match` (exhaustiveness check, guards, ranges, tuple and struct patterns)
- [ ] `Option` and `Result`: **`enum`s of the standard library** (`Zevic.Core`). `?`, `Context`, `MapErr`, `OkOr`, `Error` and `ErrorKind`
- [ ] `String`: UTF-8 validation, `Len`, `CharCount`, `+`, comparison, `ToI32` and so on
- [ ] `Array<T>`: **a type of the standard library** (`Zevic.Array`). Literals, indexing, a Panic when out of range, the main methods such as `Push` and `Pop`
- [ ] `for (x in arr)` and `for (i, x in arr)`
- [ ] `mut fn` and the check of deep immutability of `let` (you cannot `Push` to a `let` array)
- [ ] The check of public and private (capital and small letters)

### The foundation of the standard library (placed in the standard library from the start)

`Array` and `Map` are written as **types of the standard library** from the start (not built into the compiler). But Zevic has no reading and writing of pointers, so the compiler provides, as a foundation, **a module only for the standard library, `Zevic.Intrinsics`**.

- [ ] `Intrinsics.Buf<T>`: an area of fixed length. The compiler expands `New(cap)`, `Get(i)`, `Set(i, v)` and `Copy(from, to, n)` directly into IR (ordinary users cannot `import` it).
- [ ] `Zevic.Array<T>` is written in Zevic as an ordinary `class` that holds a `Buf<T>` (growth by doubling, range checks on the `Array` side)
- [ ] `String` is not generic, so it is written as a thin type that calls the runtime's C (`zevic_str_*`) through `extern "C"`
- [ ] The only things treated specially as built-ins are the **syntax** of the literal `[1, 2, 3]` (expanded into calls of `Array.New` and `Push`) and the expansion of `for`

### Implementation notes

- The `match` on an `enum` can be a **simple `switch` + matching in order** instead of a decision tree (optimize later).
- The `?` of a `Result` expands into an early `return` (recheck the order of the expansion in stage 5, when `defer` arrives).

**Done when**: `examples/03`, part of `06` and a sample that uses `Array<T>` work. **v0.0.3**.

---

## Stage 4: Classes and C interop

**Goal**: object-oriented programs that call external C libraries. **It becomes public here (v0.1.0)**.

### Language features

- [ ] `class` (reference types, initial values of fields, `Init`, `New`, `static`)
- [ ] `interface` (methods, default implementations, checking implementations, the function table for dynamic dispatch)
- [ ] Standard `interface`s: `Equatable`, `Hashable`, `Comparable`, `Iterable` and `Iterator`
- [ ] Looping over your own `Iterable` with `for`
- [ ] **Generic constraints** (`<K: Hashable>`, joined with `+`) and checking that they are satisfied. For built-in types (integers, `String` and so on) the compiler provides `Hashable`, `Equatable` and `Comparable`.
- [ ] `Map<K, V>`: written in Zevic as a **type of the standard library** (`Zevic.Map`) (a hash table that keeps the insertion order, on top of `Intrinsics.Buf`). `Set<T>` likewise.
- [ ] **C interop**: `extern "C"`, `export "C"`, the table of C types, conversion methods, pointers, variadic arguments `...`, `extern struct`
- [ ] `Handle<T>`, `NewString` and `Free` (`Zevic.C`)
- [ ] `native/` and linking `.c`, `.cpp`, `.o`, `.a` and so on (the table of extensions)
- [ ] Generating `zevic.h` when there is an `export "C"`

### Tools

- [ ] The full set of `zevic` options (`-l`, `-L`, `-I`, `-D`, `--cc`, `--cxx`)
- [ ] The search order for clang, guidance when it is not found, and the version check

### Preparing to go public

- [x] The name is final: "Zevic"
- [ ] `README.md` (features, how to build, Hello World), `CONTRIBUTING.md`, a code of conduct, and the contact details
- [ ] Say clearly which of the `examples` work. Mark those that do not work as "planned".
- [ ] Make the repositories and the organization **public**

**Done when**: a sample that calls a C library (for example SQLite or zlib) works. **v0.1.0 is published**.

---

## Stage 5: Abstractions

**Goal**: enough expressive power to write the standard library in Zevic itself, and the GC.

### Language features

- [ ] **Stronger monomorphization** (generate each set of concrete types only once, assemble names, measure the amount generated). The generics themselves came in stages 3 and 4.
- [ ] **Closures** (capture, sharing, allocating the environment), function values, lambdas, higher-order methods (`Map`, `Filter`, `Reduce`, ...)
- [ ] `defer` (runs on every path of `return`, `?` and `break`; not run on a Panic)
- [ ] `params` (variadic arguments)
- [ ] Deciding array and `Map` literals by the type on the left-hand side

### GC

- [ ] Bundle the Boehm GC sources in `runtime/third_party/bgc/` and add them to the build
- [ ] Replace `zevic_alloc` / `zevic_alloc_atomic` with the GC (the callers do not change)
- [ ] `GC_init` at the start of `main`
- [ ] With load tests (lots of allocation, long loops), check that memory does not keep growing
- [ ] Tests of the rules at the boundary with C (`Handle`, pointers that are valid only during a call)

### Implementation notes

- `Array` and `Map` have been in the standard library since stages 3 and 4, so check that replacing the GC (only changing the content of `zevic_alloc`) does not break the existing tests.
- For monomorphization the problems are **name collisions** and **growth of the code**. Put a hash in the names of generated functions, and generate each set of concrete types only once.
- The rule that a recursive type (an `enum` that contains itself) must go through a `class` or an `Array` is enforced by a check.

**Done when**: `Map`, `Filter` and `Reduce` with closures, and `Cache<K, V>`, work. It runs for a long time with the GC. **v0.2.0**.

---

## Stage 6: Tools

**Goal**: everyday development is complete with `zevpm`. Up to the second batch of the standard library.

### `zevpm`

- [x] `new`, `init`, `build`, `run`, `test` and `clean`
- [x] Reading `zevic.toml` (`[package]`, `[deps]` and `[native]`)
- [x] Output into `build/`, and for `kind = "lib"` a static library and `include/<name>.h`
- [x] `get` / `remove` / `update` (`git clone --depth 1`, the cache in `~/.zevpm`, and `zevic.lock`)
- [x] Fetching the dependencies of dependencies, and building `native/` automatically
- [x] The implementation of `zevpm` itself: first written in Python or C, and replaced by Zevic at self-hosting (stage 7)

### Test features

- [x] `test "name" { ... }` blocks and `_test.zev`
- [x] `Zevic.Test` (`Equal`, `NotEqual`, `True`, `False`, `Some`, `None`, `Ok`, `Err`, `Panics` and `Fail`)
- [x] The runner (with `setjmp` / `longjmp`, a Panic inside a test becomes the failure of that test)
- [x] Showing the results, the exit code, and filtering by name

### The second batch of the standard library

- [x] `Zevic.Fs`: `ReadAll`, `WriteAll`, `Open`, `Exists`, `List` and `Remove` (all return a `Result`; `errno` becomes an `ErrorKind`)
- [x] `Zevic.Text`: `Split`, `Join`, `Trim`, `Replace`, `StartsWith`, `EndsWith` and more
- [x] `Zevic.Math`: math functions and constants
- [x] `Zevic.Time`: `Now`, `Duration` and `Sleep`
- [x] `Zevic.Env`: `Get`, `Set`, `ProgramName`, `Exit` and `RawArgs`
- [x] `Zevic.Process`: starting external commands (`Run(cmd, args)` returns the exit code and the output as a `Result`). It is needed to call `clang` and `git` for self-hosting (stage 7), so it was **moved forward from the third batch to the second**.
- [x] Differences between OSes (Windows / POSIX) are absorbed by bundled `.c` files

### The three OSes

- [ ] Pass CI on Linux, macOS and Windows (only Linux has been checked)
- [ ] Path separators, newline codes, handling of `.exe`, and the clang search (in the order of the specification) (Windows and macOS are not checked yet)

**Done when**: `zevpm new`, `zevpm run` and `zevpm test` work on the three OSes. Samples 01 to 10 all work. **v0.3.0**.

---

## Stage 7: Self-hosting

**Goal**: the compiler written in Zevic (`zevic1`) can build itself.

### 7-1. Checking the prerequisites

- [ ] Can the compiler be written with the features of stages 1 to 6? (The standard library needed: strings, arrays, `Map`, files and starting processes with `Zevic.Process`.)
- [ ] Add what is missing to the standard library first (`Zevic.Process` came in stage 6).
- [ ] If a feature missing from the specification is found, **write a proposal (`proposals/`) first**, then add it.

### 7-2. The order of rewriting

Move the structure of `zevic0` (C) as it is to Zevic, in the same order.

1. `lexer` -> 2. `ast` -> 3. `parser` -> 4. `resolve` -> 5. `check` -> 6. `mono` -> 7. `lower` -> 8. `emit_ll` -> 9. `driver`

- Each time a part is moved, **compile the Zevic version** with `zevic0` and check that it gives **the same output (AST dump, `.ll`)** as the C version (differential tests).
- Finally, join everything.

### 7-3. Bootstrapping

```
zevic0 (written in C) -compile-> zevic1 (the compiler written in Zevic)
zevic1                -compile-> zevic2
zevic2                -compile-> zevic3
```

- [x] The `.ll` files emitted by `zevic1` and `zevic2` are **exactly the same** (when they match, self-hosting has succeeded)
- [x] **Commit** the final `.ll` as `bootstrap/zevic.ll`. Even without C, the compiler can be built with just `clang bootstrap/zevic.ll runtime/*.c`.
- [x] Check the bootstrap in CI every time

### 7-4. The remaining tools

- [x] `zevfmt` (`tools/zevfmt/`. Keeps comments and newlines. Idempotence and an unchanged AST are tested. The syntax check reuses the compiler's parser through `zevic --dump-ast`.)
- [x] Rewrite `zevpm` in Zevic (`tools/zevpm/`. The C version `zevpm/` is kept as a frozen version, and the same integration tests run on both.)
- [x] `zevic0` (the C version) was split into a separate repository (zevic-lang/zevic-c) in v0.4.1. This repository builds with only `bootstrap/zevic.ll` and clang.

### 7-5. Additions to the language and the standard library (proposal first)

- [x] Loop labels and `break@label` / `continue@label` (proposal 0001)
- [x] Asynchronous code (`async`, `await` and `Zevic.Async`) (proposal 0002)
- [x] HTTP (`Zevic.Http`) (proposal 0003. HTTPS is not supported. Windows (Winsock) is unverified.)

**Done when**: the `.ll` of `zevic2` matches and CI passes. **v0.4.0**.

---

## Polish (toward v1.0)

### Language

- [ ] Freeze the specification. Decide the necessary ones among the open questions (concurrency, `Integer` and so on).
- [ ] Write the backward compatibility policy (no changes that break existing code in 1.x)
- [ ] The third batch of the standard library (`Net`, `Thread`, `Async`, `Json` and `Random`; `Process` was moved to stage 6). For concurrency, first check how it works with the GC (the thread support of Boehm).

### Tools and quality

- [ ] Better error messages (suggesting candidates, showing several errors)
- [ ] Measure compile time and generated code (benchmarks in `tests/bench/`)
- [ ] Debug information for `-g` (DWARF). It can be followed in a debugger.
- [ ] Fuzzing (feed random input to the lexer and parser, and check that they do not crash)
- [ ] A language server (LSP) and editor extensions are to be considered after 1.0

### Distribution

- [ ] How to make a release: put the executables for the three OSes (`zevic`, `zevpm` and `zevfmt`), `runtime/` and the standard library together
- [ ] An installer (script). Consider packages for `winget`, `brew` and so on.
- [ ] An official site and a tutorial (`docs/tutorial/`), and a tour of the language
- [ ] Check that the process of proposals (`proposals/`) is working

---

## Repository layout (final form)

```
zevic/
  tools/zevic1/           the compiler written in Zevic (the C zevic0 is split off to zevic-lang/zevic-c)
  compiler/               the compiler written in Zevic (self-hosted version)
  runtime/
    zevic_rt.c  ...       the runtime
    third_party/bgc/      the Boehm GC
  packages/Zevic/         the standard library (Zevic.*): zevic.toml + src/
  tools/
    zevpm/  zevfmt/
  bootstrap/zevic.ll      the .ll that is kept committed
  tests/
    golden/               golden tests
    bench/                benchmarks
    run.py
  Makefile
  LICENSE  README.md  CHANGELOG.md

docs/
  spec/zevic-spec.md      the specification
  proposals/              proposals for changes to the language
  design/                 design notes (ir-notes.md and others)
  tutorial/

examples/                 samples (01 to 10, apps/)
.github/                  files shared by the organization
```

---

## Risks and countermeasures

| Risk | Countermeasure |
| --- | --- |
| Changes to the LLVM IR specification (differences between clang versions) | Fix it to clang 15 or newer. Try several versions in CI. Pointers are always `ptr`. |
| Monomorphization bloats the code | Generate each set of concrete types only once. Rely on optimization (`-O1`). Measure and decide. |
| Bugs at the boundary between the Boehm GC and C | The rules for the boundary are written in the specification. Use `Handle`. Make tests only for the boundary. |
| Problems specific to Windows (paths, the MSVC libraries) | Run on Windows from stage 1. Add it to CI early. |
| Generics are needed from stage 3 because `Array` and `Map` live in the standard library | Stage 3 has only the minimal version without constraints. Constraints come in stage 4 (together with `Map`). |
| The features keep growing and it never ends | Do not add the "future considerations" of the specification before v1.0. Keep to the scope of each stage. |
| Not being able to continue alone | Divide the work into small pieces and tag every stage. Make progress visible with samples and tests. |
| Self-hosting finds holes in the specification | Write a proposal and add the fix when found. The compiler itself is the biggest test. |

---

## What to start with (the first week)

1. Create the four private repositories and place `.github` and the specification (0-2)
2. Check clang (0-3)
3. Run a hand-written `hello.ll` (1-1)
4. Start on `lexer.c` (the start of stage 1)
