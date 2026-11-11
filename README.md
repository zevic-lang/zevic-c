# zevic-c

The **C implementation** (`zevic0`) of the Zevic language. The self-hosted compiler, written in Zevic itself, is in [zevic-lang/zevic](https://github.com/zevic-lang/zevic) (`zevic-lang/zevic` builds from `bootstrap/zevic.ll` and clang alone, and does not need this repository).

This repository is the C version split off at v0.4.1. It is kept for these purposes:

- As the first compiler, to rebuild the Zevic compiler from scratch (for example when bringing it to a new OS or CPU)
- As a reference implementation whose output can be compared with the self-hosted version
- A copy of the standard library (`packages/Zevic`), the runtime (`runtime/`) and the tests. Keeping it in sync with the main repository is done by hand.

The description below is as of the split.

---

# Zevic

Zevic is a small, easy-to-write native language that can use C code as it is.
It turns source code into **textual LLVM IR** and lets `clang` produce the machine code.

- Language specification: [`docs/spec/zevic-spec.md`](docs/spec/zevic-spec.md)
- Implementation plan: [`docs/plan.md`](docs/plan.md)
- The first compiler, `zevic0`, is written in C (C11) (`zevic0/`)

```
module Main

import Zevic.Console

fn Main() {
    Console.Println("Hello, World!")
}
```

## Building

You need a C compiler, `make` and **clang 15 or newer** (used to build the LLVM IR that `zevic` generates).

### Installing LLVM (clang)

| OS | Command |
| --- | --- |
| Ubuntu / Debian | `sudo apt install clang` |
| Windows | `winget install LLVM.LLVM` (the MSVC libraries are needed too, so install the Visual Studio Build Tools as well) |
| macOS | `brew install llvm` |

`zevic` looks for clang in this order: `--cc`, the `ZEVIC_CLANG` environment variable, `clang` on `PATH`, and then the usual install locations (`C:\Program Files\LLVM\bin`, Homebrew, `/usr/lib/llvm-*/bin`).
If clang is missing or too old, it prints the installation instructions above and exits.

```sh
make            # builds build/zevic and build/zevpm (`make zevfmt` for the formatter)
make test       # runs the golden tests, the zevpm tests and the examples
./build/zevic examples/01_hello.zev -o hello && ./hello
```

## Usage

```sh
zevic hello.zev -o hello                # build
zevic hello.zev native/a.c -o hello     # build C files together (.c .cpp .o .a)
zevic app.zev -Iinc -DDEBUG -L. -lfoo   # -I -D -L -l go straight to the compiler / linker
zevic app.zev --emit-header zevic.h     # also write a C header for the export "C" functions
zevic hello.zev --emit-ll -o hello.ll   # emit only the LLVM IR
zevic --version
```

The standard library (the `Zevic` package, `packages/Zevic/`) and the runtime (`runtime/`) are looked up in `share/zevic/` next to the executable,
or where the `ZEVIC_HOME` environment variable points. No extra setup is needed when you run from the repository root.

## zevpm (packages and builds)

```sh
zevpm new myapp          # creates myapp/ with zevic.toml and src/Main.zev (--lib for a library)
cd myapp
zevpm run                # build and run (arguments: zevpm run -- a b)
zevpm test               # run the test blocks and _test.zev files (zevpm test name to filter)
zevpm get owner/name@v1.0.0   # add a git dependency (the commit goes in zevic.lock, cached in ~/.zevpm)
zevpm build --release    # output goes to build/ (a library gives a .a and include/<Name>.h)
```

`zevpm` calls `zevic`, so put it in the same directory (or on `PATH`, or point the `ZEVIC` environment variable at the compiler). Git dependencies need `git`.

## The runtime and the GC

The runtime and the Boehm GC (`runtime/third_party/bgc`) are written in C. They are compiled once, by the first build, and reused from
`~/.cache/zevic` (`%LOCALAPPDATA%\zevic\cache` on Windows; change it with `ZEVIC_CACHE`).

## Releases

Every time a build of `main` succeeds, `.github/workflows/ci.yml` publishes the Linux, macOS and Windows executables to GitHub Releases.

## Examples

- [`examples/`](examples/): Hello, FizzBuzz, factorial, a calculator, `interface`, working with C (`06_c_interop/`), working with other languages (`interop/`), files, external programs, statistics, collections (`07` to `10`), `async` (`11`), HTTP (`12`) and a `zevpm` package (`apps/wordcount/`).

## Status

v0.4.1: types, `struct` / `class` / `enum`, union types (`A | B`, `is`, type patterns), `interface`, generics, closures, `defer`, the GC, working with C,
`test` blocks, loop labels (`break@name`), `async` / `await`, HTTP (`Zevic.Http`, no TLS), `zevpm` and the standard library (`Fs` `Env` `Text` `Math` `Time` `Process` `Random` `Json` `Path` `DateTime` `Regex` `Csv` `Toml` `Net` `Zip` `Gzip` and more) all work.
`zevfmt` (format with `zevfmt -w .`, check with `zevfmt -check .`) works too. The plan is in [`docs/plan.md`](docs/plan.md).

See [`CHANGELOG.md`](CHANGELOG.md) for how far the implementation has come.

## License

Apache-2.0 (`LICENSE`).
