# Proposal 0002: asynchronous `async { }` and `await`, and `Zevic.Async`

- Status: accepted (2026-11-03, implemented the same day; type names are written with the `Async.` prefix, as in `Async.Task<T>`)
- Date: 2026-11-03

## Motivation

Work with long waits, such as HTTP (proposal 0003) and `Process`, should be able to run side by side.
Today a single thread has to wait for each one in turn.

## Proposal

### Language: two keywords

```
let a: Task<I32> = async { slowSquare(3) }
let b: Task<I32> = async { slowSquare(4) }
let total: I32 = await a + await b
```

- `async { ... }` is an expression of type `Task<T>` (`T` is the type of the value of the block). The block starts running on another thread at once.
- `await e` is a prefix operator (as strong as `!`). `e` is a `Task<T>`; it waits until the task is done and returns the `T`. It is the same as `e.Await()`.
- Both are **sugar**: `async { b }` expands to `Async.Spawn(() => { b })` and `await e` to `e.Await()`. There is no coloring of functions with `async`. An asynchronous function is an ordinary function that returns a `Task<T>`.
- `async` and `await` become reserved words. Existing code that uses these names has to write them in backquotes.
- `return`, `break` and `?` inside the block only leave the block, as in a lambda.

### Standard library: `Zevic.Async`

```
class Task<T> {
    fn Await() -> T              // calling it twice returns the same value
    fn IsDone() -> Bool
}
fn Spawn<T>(f: fn() -> T) -> Task<T>

class Channel<T> {
    fn Init(capacity: I64)       // 0 or less means unbounded
    mut fn Send(v: T)            // waits when full; Panic after it is closed
    mut fn Recv() -> Option<T>   // waits when empty; None when closed and empty
    mut fn Close()
}

class Mutex {
    fn Init()
    fn Lock()
    fn Unlock()
}
```

### How it runs

- A `Task` is one OS thread (a POSIX pthread). The Boehm GC is built with thread support, and threads are registered at run time with `GC_pthread_create`.
- There is no type restriction on what can be passed between threads (references are shared). Avoiding data races is up to the programmer, and `Channel` and `Mutex` are the tools for that. The compiler enforces one rule: **an `async` block cannot use a `mut` local variable of the enclosing function (a non-class value)** (an error), because accidents like `count += 1` are the most common. Class values can be shared as references (for classes that hold a `Channel` or a `Mutex`).
- A `Panic` inside a `Task` stops the whole program, as before (there is no mechanism to stop only that thread for now).
- When `main` ends, the program ends without waiting for running threads.

## Alternatives

- Transforming into a state machine (Rust / Kotlin style `async fn`): too large for single-pass code generation, and it brings function coloring.
- Coroutines on one thread: the Boehm GC cannot scan other stacks. To be reconsidered when the GC is replaced.
- Only `Spawn`, with no new syntax: that would work, but `async {}` and `await` are short and readable, so they are added.

## Impact

- Lexer: the reserved words `async` and `await`. Syntax: a prefix `await` and the `async` block as an expression.
- Runtime: a thread-aware GC, and thin C functions for threads, mutexes and condition variables. `-lpthread` is linked (only where needed).
- Standard library: `Zevic.Async` is added.
- `zevfmt`: a space after `await`, and the same for `async {`.
- Threads on Windows are unverified (to be checked after running CI).

## Tests

- Golden: two `async` blocks side by side whose results are added, a producer and a consumer with a `Channel`, a counter protected by a `Mutex`, and the error for using a `mut` local variable.
- A speed check: ten 100 ms `Sleep`s side by side finish in a few hundred milliseconds in total, not in one second.
