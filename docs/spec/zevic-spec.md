# Zevic Language Specification

Oct 27, 2026 · @荒木知也

## Overview and design principles

Zevic is a small, easy-to-write native language that can use C code as it is. This document is the language specification and records all the design decisions.

### Goals

- Call C libraries and sources without any bridge (`extern "C"`, `zevic app.zev b.c`)
- Readability close to C# (`class`, `interface`, `enum` and `match`, interpolated strings)
- Little to learn. As far as possible there is only one way to write the same thing
- Emit LLVM IR and let `clang` produce the machine code. The linker and cross-compilation are left to `clang`
- Write the compiler in Zevic itself (self-hosting) in the future
- Provide everything from building to formatting with `zevic` (the compiler), `zevpm` (the package manager) and `zevfmt` (the formatter)

### Non-goals

- Low-level control for writing an OS or a kernel (`unsafe`, manual memory and a `NoGC` mode have been moved to future considerations)
- Inheritance (only `interface`s and composition are used)
- Macros (variadic arguments are covered by `params`; if needed, to be considered after self-hosting)
- Function overloading, default arguments and named arguments
- Operator overloading (except `==` and ordering comparisons)

### Design principles

- Types are never omitted (variables, parameters, fields and return values). There are no implicit type conversions.
- A capital first letter means public, a small first letter means private
- `this` cannot be omitted. The conditions of `if`, `while`, `for` and `match` are enclosed in parentheses, and `{}` is mandatory
- `if` and `match` are expressions and can return values
- Failures that are part of normal processing are handled with `Result` and `?`, and bugs in the program with `Panic`
- "Nothing" is expressed with `Option`, not `null`
- Memory is managed automatically (GC)

### Name

- The name of the language is "Zevic" (pronounced "ZEV-ik"), and the extension is `.zev`. The name is final.
- Things derived from the name: the command names (`zevic`, `zevpm`, `zevfmt`), the root of the standard library (`Zevic.*`), `zevic.toml`, environment variables (such as `ZEVIC_HOME`) and the GitHub organization (`zevic-lang`)

### How to read this document

- Code examples use fixed-width type names (`I32`, `I64`, `F64` and so on).
- Things marked "decision" are settled; things marked "future" or "undecided" are not. The list of undecided items is in the chapter "Open questions and future considerations".
- For the numbers of the implementation order (1 to 7), see the chapter "Implementation plan".

## Lexical structure

The source is UTF-8 text, and the tokens are identifiers, reserved words, literals and symbols. Tokens are separated by whitespace and comments.

### Character encoding and whitespace

- UTF-8. A BOM is ignored. Both `LF` and `CRLF` newlines can be read, and `zevfmt` normalizes them to `LF`.
- Whitespace (spaces and tabs) separates tokens, and indentation has no meaning.

### Newlines and the end of statements

A newline ends a statement. With `;` you can write several statements on one line. In the following cases a statement continues after a newline:

- the line ends with a binary operator (`+ - * / % & | ^ << >> && || == != < > <= >=`), `,`, `(`, `[`, `{`, `=`, `=>`, `->` or `.`
- the next line starts with `.`, `&&` or `||`
- inside `( )` and `[ ]` (newlines are always ignored there)

When the next line starts with `-` or `(`, it is not taken as a continuation (so that a `-b` on the next line is not mistaken for a subtraction).

```
let total: I64 = nums
    .Filter((x: I32) => x > 0)
    .Map((x: I32) => x.ToI64())
    .Sum()

if (a > 0 &&
    b > 0) { ... }

let a: I32 = 1; let b: I32 = 2
```

### Comments

```
// a line comment
/* a block comment /* that can be nested */ */
/// a documentation comment (attached to the next declaration; for a future zevdoc)
```

Comments are not thrown away by the lexer but kept in the syntax tree (so that `zevfmt` does not delete them).

### Identifiers

- ASCII letters and digits and `_`. The first character is a letter or `_`. Upper and lower case are distinguished.
- Public names are `UpperCamelCase`; private names, local variables and parameters are `lowerCamelCase`.
- Only `_` means "anything" in a pattern.
- To use a reserved word as an identifier, enclose it in backquotes (`` `type` ``). This is needed to handle names such as `type` and `in` that appear in C headers.

### Reserved words (30)

They cannot be used as identifiers. They are all in lower case; only `Self` is capitalized.

| Category | Words |
| --- | --- |
| Declarations | `module` `import` `as` `fn` `let` `mut` |
| Types | `class` `struct` `enum` `interface` `static` `Self` |
| C interop | `extern` `export` |
| Control | `if` `else` `while` `loop` `for` `in` `match` `return` `break` `continue` `defer` |
| Asynchronous | `async` `await` |
| Values | `true` `false` `this` |

The `"C"` after `extern` and `export` is not a keyword but a string literal.

### Contextual keywords

They have a meaning only in their position, and can be used as identifiers elsewhere.

- `params`: at the start of a parameter declaration
- `test`: at the top level of a module, directly before a string literal

### Words reserved for the future

They cannot be used now: `macro`, `unsafe`, `type`, `where`, `const`, `yield`, `impl` and `null`.

### Reserved names

They are not keywords, but defining the same name at the top level of a module is an error.

- The prelude: `I8` `I16` `I32` `I64` `U8` `U16` `U32` `U64` `F32` `F64` `Bool` `Char` `String` `Unit` `Array` `Map` `Option` `Result` `Error` `ErrorKind` `StringBuilder` `Some` `None` `Ok` `Err` `Panic` `Same`
- Special method names: `Init` (the constructor) and `New` (a function generated by the compiler; defining it yourself is an error)

### Literals

**Integers**

```
123    1_000_000    0xFF    0b1010    0o17
255_U8    10_I64         // only when specifying the type, add _ and the type name
```

- `_` can be written only between digits (at the start, at the end or doubled is an error). Prefixes are lower case only.
- An octal number starting with 0, such as `017`, is an error.
- The type is decided by the context (`let x: U8 = 200`). Without context it is `I64`. Going out of range is a compile error.
- A negative number is a prefix `-` and an integer. As in `let x: I8 = -128`, it can be written up to the end of the range.

**Decimals**

```
1.5    2.0    1e10    1.5e-3    1_000.5    1.5_F32
```

- Digits are needed on both sides of `.` (`1.` and `.5` are errors). With an exponent it is a decimal even without a decimal point.
- The type is decided by the context, and is `F64` without one. `NaN` and infinity are `F64.NaN` and `F64.Infinity`.
- A `.` after digits is a decimal point only when digits follow it. `1.ToString()` is a method call, and `1..10` is a range.

**Booleans and Unit**

```
true    false    ()          // () is the value of Unit
```

**Characters and bytes**

```
'a'    '\n'    '\u{1F600}'    'あ'
b'a'    b'\n'    b'\xFF'      // a U8 byte (ASCII and \xNN)
```

- A `Char` is one Unicode character (`'ab'` is an error).
- `b'...'` is a `U8`. It is used when scanning source (`s.ByteAt(i) == b'a'`) and for C interop.

**Strings**

```
"hello"    "tab\there"    "quote: \""    "x = ${x}"    "price: \${1}"
```

- The usable `\` escapes are `\n` `\r` `\t` `\\` `\"` `\'` `\0` `\$` `\u{...}`, and `\xNN` (only 00 to 7F; a string is always valid UTF-8).
- A string may be written inside an interpolation `${expression}` (`"a ${f("b")} c"`). Interpolation takes no format specifier (use `Printf` for digits).
- A newline cannot be written inside the quotes (to find a forgotten closing quote early).

**Multi-line strings and raw strings**

```
let text: String = """
    Hello,
      ${name}!
    """

r"C:\temp\new"          // no escapes and no interpolation
r"""
a multi-line raw string
"""
```

- A newline follows the opening `"""`. The indentation of the line with the closing `"""` is removed from all lines. The last newline is not included.
- Inside `"""`, escapes and interpolation can be used. With `r`, both are disabled.

**Collections and tuples**

```
[1, 2, 3]            // an array (an empty one: let a: Array<I32> = [])
{"a": 1, "b": 2}      // a Map (an empty one: Map.New())
(1, "x")              // a tuple
```

- The type of the elements of an array literal is decided by **the type on the left-hand side**. `let a: Array<Shape> = [Shape.Circle(1.0), Shape.Empty]` reads the elements as `Shape` (the same for `interface` types).
- Without a type on the left-hand side, it is an error unless all elements have the same type. An empty `[]` needs a type on the left-hand side.
- The same goes for `Map` literals: the types of the keys and values are decided by the type on the left-hand side.

### Symbols

`. .. ..= , ; : => -> ? ! ~ & | ^ << >> + - * / % = == != < > <= >= && || ( ) [ ] { }`, and `${` only inside strings. `@` is used only for loop labels ([proposal 0001](../proposals/0001-loop-labels.md)). `#` and `::` are not used now.

## Types

The types of Zevic are the fixed-width numeric types, `Bool`, `Char`, `String` and `Unit`, then `Array`, `Map`, `Option` and `Result`, tuples, and user-defined types (`struct`, `class` and `enum`). There are no types of undetermined width named `Int` or `Float`.

### Basic types

| Type | Meaning |
| --- | --- |
| `I8` `I16` `I32` `I64` | signed integers |
| `U8` `U16` `U32` `U64` | unsigned integers (`U8` is the byte) |
| `F32` `F64` | IEEE 754 floating point |
| `Bool` | `true` / `false` |
| `Char` | a Unicode scalar value (32 bits) |
| `String` | an immutable UTF-8 string |
| `Unit` | the only value is `()`. It means "no value" |

- If integer arithmetic goes out of range, it is a `Panic` (no wrapping). Integer division by zero is also a `Panic`.
- An integer literal without a written type takes the type of the context. Without context it is `I64`, and a decimal is `F64`.
- An arbitrary-precision integer (`Integer`) does not exist now.

### Composite types

- **`Array<T>`**: a growable array. A reference type (managed by the GC). When the elements are `struct`s or numbers, the elements are stored side by side.
- **`Map<K, V>`**: a hash table. Iteration is in insertion order. The key is a type that implements `Hashable` (in the first implementation only integers such as `I32`, `Char`, `Bool` and `String`).
- **`Option<T>`**: `Some(T)` or `None`. Used instead of `null`.
- **`Result<T, E>`**: `Ok(T)` or `Err(E)`. By default `Error` is used for `E`.
- **Tuples**: `(I32, String)`. Elements are taken out only by destructuring (`let (a, b): (I32, String) = t`).
- **Function types**: `fn(I32) -> I32`
- **Union types**: `I32 | String`. A nameless "one of several types" (see the section "Union types" below).
- **`Self`**: inside an `interface` and a type definition, it refers to the type itself.

`Option` and `Result` are `enum`s of the standard library (chapter "Type definitions").

### Writing types and inference

- The types of variables, parameters, fields and return values must always be written (only when the return value is `Unit`, `-> Unit` can be omitted).
- The type arguments of generics are decided by the type on the left-hand side or the types of the arguments (`let s: Stack<I32> = Stack.New()`). It is an error if they cannot be decided.
- Integer and decimal literals whose type is not decided become `I64` and `F64` after the other constraints are decided.
- The types of the parameters of a lambda are written. The return type is decided from the expression of the body.

### Type conversions

There are no implicit conversions. They are made explicit with methods named `ToXxx`.

| Conversion | Return value |
| --- | --- |
| Always fits (`I32` to `I64`, `I32` to `F64`, `U8` to `I32`) | a value |
| May not fit (`I64` to `I32`, `I32` to `U8`, `String` to `I32`) | `Result<T, Error>` |
| The version that cuts the range (such as `ToI32Wrap()`) | a value |
| `F64` to an integer (`ToI32()`, `ToI64()`, `Floor()`, `Ceil()`, `Round()`) | a value (the fractional part is cut off; `NaN` and out of range are a `Panic`) |
| `ToString()` of every type | `String` |

- When a conversion from `String` to a number fails, it returns an `Error` of `ErrorKind.InvalidFormat` (wrong format) or `ErrorKind.OutOfRange` (out of range).
- Arithmetic between integer types is only between the same types (`I32 + I64` is an error).
- Conversions to C types are in the chapter "C and C++ interop".

### Union types

`A | B | C` is a type that holds a value of exactly one of its members (a type for lightweight error handling in the style of Kotlin's `Result`, or for writing "a value or a failure" lightly). Unlike an `enum`, it is not declared, and the members are the types themselves.

```
fn Parse(s: String) -> I32 | ParseError {
    if (s == "1") { return 1 }          // a value of a member can be passed as it is
    ParseError.New("cannot parse ${s}")
}

let r: I32 | ParseError = Parse("x")
if (r is ParseError) {
    Console.Println(r.Msg)              // here r is a ParseError
} else {
    Console.Println("${r + 1}")         // here r is an I32
}
match (r) {
    n: I32 => Console.Println("ok ${n}"),
    e: ParseError => Console.Println(e.Msg),
}
```

- **The same type**: the order and the nesting of the members do not matter. `(A | B) | C` is the same type as `A | B | C`, and `B | A` as `A | B`. Writing the same type twice gives one. The name of the type lists the members in name order joined with `|` (error messages use this order too). It is an error if only one member is left. `Never` cannot be a member.
- **Implicit conversion** (the only implicit conversion in the language): the value of a member goes into a union type that contains that member. The value of a union type goes into a larger union type that contains all its members (`I32 | String` to `I32 | String | Bool`). The reverse (from a union to a member) is narrowed with `is` or `match`.
- A numeric literal takes the type of the integer (decimal) member if the expected union has exactly one. With two or more it becomes `I64` (`F64`), so it is an error if that is not a member.
- **`is` and `!is`**: `x is T` tells whether the value `x` of a union type is a `T` (`Bool`). It is an error if `T` is not a member. The precedence is the same as comparison (`<` and so on), and they cannot be chained.
- **Narrowing (smart casts)**: a local variable `x` that is not `mut` (including parameters) can be used as `T` (or as the union of the remaining members) within the range in which `x is T` is settled. The ranges are:
  - each branch of `if (x is T) { ... } else { ... }` (reversed for `!is`)
  - the right side of `&&` (`x is T && x.F`) and the right side of `||` (`x !is T || x.F`). Conditions joined with `!`, `&&` and `||` are also used as far as they are settled.
  - when the body of an `if` without `else` ends in `return`, `break` or `continue`, as in `if (x is T) { return ... }`, the rest of the block after it
  - `mut` variables, fields and expressions are not narrowed (because they can change in between). Take them into a `let` first.
- **`match`**: writing `name: Type` (or `_: Type`) as a pattern matches the member of that type and binds it to the name. Type patterns can be used only on values of union types. It is a compile error unless all members are covered (or `_` is used).
- **Branches of `if` and `match`**: when a union type is expected (the type of a `let`, the return type, a parameter), the value of each branch is converted to the union. Without an expectation, the types of the branches must be the same.
- Comparison (`==`) and embedding in `${}` cannot be done with the value of a union type (narrow it with `is` or `match` first).
- It can be used in generics as in `T | Error`. If the type argument cannot be decided from the other arguments or the return value, write it (`Pass<I32>(5)`).
- Implementation: held in the same form as an `enum` (a tag and an area as large as the largest member). A value type, which is copied.

### Equality and ordering

`==` is equality as a value, decided by the type.

| Type | Meaning of `==` |
| --- | --- |
| integers, `Bool`, `Char` | comparison of the values |
| `F32` `F64` | follows IEEE (`NaN == NaN` is false) |
| `String` | comparison of the content |
| `struct`, `enum`, tuples, `Option`, `Result` | automatic, field by field or variant by variant (when the content supports `==`) |
| `Array<T>`, `Map<K, V>` | element by element (when the elements support `==`) |
| `class` | by default comparison of references. If `Equatable` is implemented, `Equals` is called |

- `!=` is the negation of `==`. `==` between different types is an error.
- Whether two references are the same is checked with the prelude function `Same(a, b)`.
- `< > <= >=` can be used on integers, `Char`, `String` (byte order), `F32` and `F64`, and types that implement `Comparable`. Comparisons do not chain (`a < b < c` is an error).
- `struct` and `enum` have `Equatable` and `Hashable` automatically (when the content can have them). `Comparable` is written by hand.
- Integers, `Char`, `Bool` and `String` already implement `Equatable`, `Hashable` and `Comparable`. `F32` and `F64` have only `Equatable` and `Comparable` (`NaN` is ordered last), and are not `Hashable`.

## Variables, expressions and operators

Variables are declared with `let` (immutable) and `mut` (mutable), and the type and the initial value must always be written.

### Variables

```
let x: I32 = 10      // immutable
mut y: F64 = 2.5     // mutable
```

- The type and the initial value are required. There is no declaration without an initial value.
- A `let` variable cannot be reassigned, and **its content cannot be changed either**. `Push` and `Set` on an `Array` or a `Map`, calling a `mut fn` of a `class`, and assigning to a field are errors.
- A field of an element of a `mut` array can be assigned (`xs[i].Count += 1`). A `class` element is changed in place, and a `struct` element is taken out, changed and written back into the array. So for `struct` elements the array and the index are limited to simple expressions (a variable, a field, a number). For a `let` array it is an error (proposal 0004).
- To change something, declare it with `mut`.

```
mut words: Array<String> = ["a"]
words.Push("b")             // OK

let ws: Array<String> = ["a"]
ws.Push("b")                // error: the content of a let cannot be changed
```

- Function parameters are like `let` and cannot be changed. To change one, add `mut` (`fn Add(mut a: Array<I32>)`). Only a `mut` variable can be passed to a `mut` parameter.
- Value types such as `struct` are copied, so this rule matters for reference types (`class`, `Array`, `Map` and `StringBuilder`).
- At the top level of a module, only `let` (constants) can be written. `mut` cannot (global mutable state is forbidden).
- Declaring the same name twice in the same scope is an error. Whether an inner scope may declare the same name as an outer one (shadowing) is undecided.

### Blocks and expressions

- `{ ... }` is a block, and the last expression is its value. Without one it is `Unit`.
- `if` and `match` are expressions and can return values. All branches have the same type, and an `if` without `else` is `Unit`.
- Assignment is a statement, not an expression. `a = b = c` and `if (a = b)` cannot be written.
- `return`, `break`, `continue` and `Panic` can be written in the position of any expression (used as in `None => return`).

### Operator precedence

From the highest down.

| Rank | Operators | Associativity |
| --- | --- | --- |
| 1 | `.` (member), `( )` (call), `[ ]` (index), `?` | from the left |
| 2 | prefix `-` `!` `~` `await` | from the right |
| 3 | `*` `/` `%` | from the left |
| 4 | `+` `-` | from the left |
| 5 | `<<` `>>` | from the left |
| 6 | `&` | from the left |
| 7 | `^` | from the left |
| 8 | bitwise or (one vertical bar) | from the left |
| 9 | `==` `!=` `<` `>` `<=` `>=` | none |
| 10 | `&&` | from the left |
| 11 | logical or (two vertical bars) | from the left |
| 12 | `..` `..=` | none |
| 13 | `=>` (lambda) | the body extends as far to the right as possible |

- Bit operations (`& | ^ ~ << >>`) can be used only on integer types. Unlike C, they bind more strongly than comparison (`a & b == c` is `(a & b) == c`).
- Postfix operators (`.` `?` `[]`) are stronger than the prefix `-` and `!`. `-x.Abs()` is `-(x.Abs())`, and to call it on a negative number write `(-5).Abs()`.
- Comparisons and ranges do not chain. `a < b < c`, `a == b == c` and `1..2..3` are errors. `0..n - 1` is `0..(n - 1)`.
- `&&` and `||` short-circuit. `!` can be used only on a `Bool`.
- `+` is between numbers of the same type, or between `String`s.

### Integer and decimal arithmetic

- Going out of range with integers is a `Panic` (no wrapping). To wrap, use dedicated methods such as `WrapAdd(y)` (see the chapter "Standard library, first batch").
- Integer `/` truncates toward 0. The sign of `%` follows the left operand (as in C). Division by zero is a `Panic`.
- The shift amount is an `I32`. A negative number or the width of the type or more is a `Panic`.
- Decimals follow IEEE 754. `1.0 / 0.0` is infinity, not a `Panic`.
- There is no ternary operator, no `++` and no `--` (`if` is an expression, and `x += 1` is used).

### Assignment

```
x = 1
a[i] = 2
obj.Name = "x"
x += 1          // += -= *= /= %= &= |= ^= <<= >>= can be used
```

The left-hand side is a `mut` variable, a field of a `mut` variable, or an element of a `mut` array.

## Control flow

The control constructs are `if`, `while`, `loop`, `for` and `match`. The condition is always enclosed in parentheses, and the `{}` of the body cannot be omitted. `if` and `match` are expressions and can return values.

### if

```
if (x > 0) {
    Zevic.Console.Println("positive")
} else if (x == 0) {
    Zevic.Console.Println("zero")
} else {
    Zevic.Console.Println("negative")
}

let label: String = if (x > 0) { "positive" } else { "not positive" }
```
- The condition is only a `Bool` (`if (1)` is an error).
- Used as an expression, all branches have the same type and an `else` is needed. The type of an `if` without `else` is `Unit`.

### while and loop

```
while (i < 10) {
    i += 1
}

loop {
    if (done) { break }
}
```

- `loop` is an infinite loop, left with `break`. `break` carries no value.
- `break` and `continue` act on the innermost loop.
- A loop can be given a label with `name@`. `break@name` and `continue@name` act on the loop with that label (the same form as Kotlin).

```
outer@ for (row in rows) {
    for (x in row) {
        if (x < 0) { continue@outer }
        if (x == target) { break@outer }
    }
}
```

  - Only `while`, `loop` and `for` can carry a label. `name@` is written on the same line as the loop, with the `@` attached to the name.
  - In `break@name` there is no space between `break`, `@` and the name.
  - A label refers to a loop enclosing that position in the same function. A lambda cannot be crossed.
  - An unknown label, or a label with the same name as an enclosing loop, is an error.
  - When leaving, the `defer`s in between run from the inside out.

### for

```
for (i in 0..10) { ... }          // 0 to 9
for (i in 0..=10) { ... }         // 0 to 10
for (x in arr) { ... }
for (i, x in arr) { ... }         // for an array: (index, element)
for (k, v in map) { ... }         // for a Map: (key, value), in insertion order
for ((a, b) in pairs) { ... }     // when the elements are tuples
```

- There is no C-style `for (init; cond; step)`.
- The loop variable is like `let` and cannot be changed.
- The two-variable form is (index, element) for your own types too. Only `Map` is the exception, with (key, value). If the elements are tuples, destructure with the parenthesized `((a, b) in obj)`.
- The type of `0..n` follows the type of `n` (`I32` if `n: I32`). If both are constants, it is `I64`.
- `for (x in obj)` expands as follows (the rules for `Iterable` are in the chapter "Type definitions"):

```
mut it = obj.Iter()
loop {
    match (it.Next()) {
        Some(x) => { /* the body */ },
        None => break,
    }
}
```

### match

```
let kind: String = match (n) {
    0 => "zero",
    1..=9 => "one digit",
    10 | 20 | 30 => "a round number",
    n if (n > 100) => "large",
    _ => "anything else",
}
```

- It is a compile error if it is not exhaustive. `_` can gather the rest. A warning is given for patterns that cannot be reached.
- The values of the branches all have the same type.
- What can be written as a **pattern**:
  - literals (`0`, `"a"`, `'x'`, `true`) and ranges (`1..=9`)
  - `_` (anything) and a binding to a variable (`n`)
  - tuples (`(a, b)`)
  - variants of an `enum` (`Shape.Circle(r)` with positional arguments, `Shape.Named { Size: s, .. }` with names; `..` ignores the rest)
  - nesting (`Ok(Some(v))`)
  - several alternatives joined with `|` (`0 | 1`)
  - a member of a union type (`n: I32`, `_: Error`; see the section "Union types")
  - guards (`n if (n > 100)`). The condition of a guard is also enclosed in parentheses.
- An alias for the whole (`all @ Shape.Circle(_)`) is a future consideration.
- The only variants that can be written without qualification are `Some`, `None`, `Ok` and `Err`. Your own `enum`s are qualified with the type name, as in `Shape.Circle`.

### return, break, continue and Panic

```
let line: String = match (Zevic.Console.Prompt("Age: ")) {
    Some(l) => l,
    None => return,
}
```

- `return`, `break`, `continue` and `Panic` can be written in the position of any expression (the type `Never` is not visible to users).
- `return` leaves the function. In a function that returns `Unit`, write a `return` without a value.
- The last expression of a function body is the return value (`return` can be omitted).

### defer

```
fn Load(path: String) -> Result<String, Error> {
    let f: Zevic.Fs.File = Zevic.Fs.Open(path, "r").Context("opening ${path}")?
    defer f.Close()
    f.ReadAll()
}
```

- `defer statement` or `defer { ... }` runs the code when the enclosing block is left (per block, not per function).
- When there are several, they run in order starting from the one written last.
- It runs on a normal end, `return`, `break`, `continue` and an early return by `?`. It does not run on a `Panic` or `Zevic.Env.Exit`.
- The expression of the return value is evaluated first, and then the `defer`s run (in the example above, `Close()` runs after the result of `ReadAll()` is decided).
- Variables inside a `defer` see the value at the time it runs (the arguments are not fixed at the time of the `defer`).
- `return`, `break`, `continue` and `?` cannot be written inside a `defer`. The result of a call that returns a `Result` is discarded.
- Implementation: at each exit of a block, the compiler copies the deferred code into the IR.

### Asynchronous code (async, await)

```
import Zevic.Async

let a: Async.Task<I64> = async { slow(3) }     // starts running at once on another thread
let b: Async.Task<I64> = async { slow(4) }
let total: I64 = await a + await b              // wait for the end and take out the result
```

- `async { ... }` is an expression of type `Async.Task<T>` (`T` is the type of the value of the block). It is the same as `Async.Spawn(() => { ... })`.
- `await e` is a prefix operator, as strong as `!`. It is the same as `e.Await()`. A `Task` can be awaited any number of times, and the same value is returned.
- Functions do not get `async`. An asynchronous function is an ordinary function that returns a `Task<T>`.
- `return`, `break` and `?` inside the block only leave the block, as in a lambda.
- Memory is shared between threads. To pass things safely, use `Async.Channel<T>` (`Send`, `Recv`, `Close`) and `Async.Mutex` (`Lock`, `Unlock`).
- Using a `mut` local variable of the enclosing function inside an `async` block is an error (because it would race). Class values, however, can be shared as references.
- A `Panic` inside a `Task` stops the whole program. When `main` ends, the program ends without waiting for running tasks.

### Random numbers (Zevic.Random)

```
import Zevic.Random

mut rng: Random.Rng = Random.Rng.New(42)       // the same seed gives the same sequence on every system
let dice: I64 = rng.Int(1, 7)                    // 1 or more, less than 7
let p: F64 = rng.Float()                         // 0.0 or more, less than 1.0
Random.Shuffle(rng, xs)                          // reorder
let one: Option<I64> = Random.Pick(rng, xs)      // pick one (None when empty)
```

- A generator without a fixed seed is `Random.Rng.Seeded()` (made from the clock). The methods are `NextU64`, `Int(low, high)`, `Float` and `Bool`.
- The algorithm is SplitMix64. **It cannot be used for cryptography.** Do not share a generator between threads; make one per task. See `docs/proposals/0005-random.md` for details.

### JSON (Zevic.Json)

```
import Zevic.Json

let v: Json.Value = Json.Parse("{\"name\": \"zevic\", \"tags\": [1, 2]}")?
let name: String = v.Get("name").UnwrapOr(Json.Value.Null).AsString().UnwrapOr("?")
Console.Println(Json.Stringify(v))      // {"name":"zevic","tags":[1,2]}
Console.Println(Json.Pretty(v))         // indented
```

- `Json.Value` is an enumeration of `Null`, `Bool(Bool)`, `Number(F64)`, `Str(String)`, `List(Array<Value>)` and `Object(Map<String, Value>)`. Values are taken out with `AsBool`, `AsF64`, `AsI64`, `AsString`, `AsArray` and `AsObject` (`None` when the kind is different), `Get(key)` and `At(index)`.
- The error of `Parse` is `ErrorKind.Parse` and includes the line and column. It follows RFC 8259 strictly (a trailing comma or comments are errors). Nesting is limited to 512 levels.
- Objects keep the order of their keys. Numbers are `F64` (integers up to 2^53 are exact). See `docs/proposals/0006-json.md` for details.

### HTTP (Zevic.Http)

```
import Zevic.Http

let r: Http.Response = Http.Get("http://127.0.0.1:8080/hello")?     // there are also Post(url, body) and Send(request)
Console.Println("${r.Status} ${r.Body}")                              // a 404 or 500 is seen in Status, not as an Err

let server: Http.Server = Http.Listen("127.0.0.1", 0)?                // port 0: pick a free port
let task: Async.Task<Result<Unit, Error>> = async {
    server.Serve((req: Http.Request) => Http.Response.Text(200, "hello ${req.Path()}"))
}
server.Close()                                                        // ends Serve
```

- Only `http://`. `https://` is an `Err` (TLS is not supported). Redirects are not followed. Each connection is closed after one request.
- The body is handled with `Content-Length`, `chunked`, or until the connection closes. The limit is 16 MiB and the timeout is 30 seconds.
- `Serve` creates one `async` task per connection. See `docs/proposals/0003-http.md` for details.

## Functions and lambdas

Functions are declared with `fn`, and the types of the parameters and the return value must always be written. There is only one function of a given name in a module (no overloading).

### Declaring functions

```
fn Add(a: I32, b: I32) -> I32 {
    a + b
}

fn Greet(name: String) {                 // only when the return value is Unit, -> Unit can be omitted
    Zevic.Console.Println("Hello, ${name}")
}

fn Main() -> Unit { ... }                // it may be written too
```

- The last expression of the body is the return value. To leave in the middle, write `return value` (just `return` for `Unit`).
- Parameters cannot be changed. To change one, add `mut` (`fn Fill(mut a: Array<I32>)`).
- There is no syntax for declaring a function inside a function (use a lambda).
- Recursion can be used. The order of declarations does not matter, and a function can be called from anywhere in the module.

### Variadic arguments (params)

Only the last parameter can be a `params`. The caller writes the values in a row, and the receiving side gets them as an array.

```
fn Sum(params nums: Array<I32>) -> I32 {
    nums.Reduce(0, (acc: I32, x: I32) => acc + x)
}

Sum(1, 2, 3)          // nums is [1, 2, 3]
Sum()                 // nums is []
```

`params` is a contextual keyword and has a meaning only at the start of a parameter declaration.

### Overloading, default arguments and named arguments

None of the three exists. Use the following instead.

- You want several ways to create something: provide static functions with different names (`Point.Origin()`, `Point.FromPolar(r, theta)`)
- You want the same function for each numeric type: make it a method of the type (`x.Abs()`, `a.Min(b)`)
- You want to omit an argument: use a function with another name, or `params` or `Option`

### Using functions as values

A function is a value, and can be put in a variable or passed as an argument. The type is `fn(parameter types) -> return type`.

```
fn Show(x: I32) -> Unit { Zevic.Console.Println("${x}") }

let f: fn(I32) -> Unit = Show
nums.Each(Show)
```

### Lambdas and closures

```
let inc: fn(I32) -> I32 = (x: I32) => x + 1

nums.Each((x: I32) => Zevic.Console.Println("${x}"))

nums.Each((x: I32) => {              // several lines are written with {}; the last expression is the value
    let y: I32 = x * 2
    Zevic.Console.Println("${y}")
})
```

- The types of the parameters are written. The return type is decided from the expression of the body.
- `return`, `break` and `?` inside a lambda only leave the lambda, not the enclosing function. To leave the enclosing function in the middle, use `for`.
- `mut` variables of the enclosing function can be captured, and changes are visible outside (they are shared). Captured variables are managed by the GC.
- The body extends to the right of `=>` as far as possible.

### Generic functions

```
fn First<T>(a: Array<T>) -> Option<T> {
    a.Get(0)
}

fn Max<T: Comparable>(a: T, b: T) -> T {
    if (a.Compare(b) >= 0) { a } else { b }
}
```

- Type arguments are decided from the types of the arguments or the type on the left-hand side. They may also be given explicitly, as in `First<I32>(a)`.
- Constraints on type arguments are written `<T: Hashable>`. There is no `where`. Constraints come in implementation stage 5 (together with `interface`); until then only the built-in types are covered.
- Implementation: a separate function is made for each type used (monomorphization).

### Names of functions

- Public functions are `UpperCamelCase`, private ones `lowerCamelCase`.
- Functions of other modules are called qualified with the imported path (or alias) (`Zevic.Console.Println`, `U.Add`).

## Type definitions

Your own types are defined with `struct` (a value type), `class` (a reference type) and `enum` (choices with data), and common promises are expressed with `interface`. There is no inheritance; code is shared with the default implementations of `interface` and with composition through fields.

### class and struct

```
class Person {
    name: String                  // lower case = private (inside the module)
    Age: I32                      // upper case = public
    Tags: Array<String> = []      // an initial value of a field

    fn Init(name: String, age: I32) {
        this.name = name
        this.Age = age
    }

    fn Name() -> String { this.name }
    mut fn Birthday() { this.Age += 1 }
    static fn Anonymous() -> Person { Person.New("?", 0) }
}

mut p: Person = Person.New("Taro", 20)
p.Birthday()
```

|  | `class` | `struct` |
| --- | --- | --- |
| Kind | reference type (managed by the GC) | value type (copied on assignment) |
| Creation | `Type.New(...)` | `Type.New(...)` |
| Implementing an `interface` | possible | possible (a value made into an `interface` type is placed on the heap) |
| Default of `==` | comparison of references | field-by-field comparison |

**Rules**

- A field is written `name: Type`, and an initial value can be given with `= expression`. A capital first letter is public, a small one is private (visible only inside the same module).
- Methods are `fn`. **`this` cannot be omitted** (`this.name`).
- **`mut fn`** can change the fields of `this`. It can be called only from a `mut` variable. Inside an ordinary `fn`, the content of `this` cannot be changed (deep immutability).
- **`static fn`** and **`static` fields** are called with the type name, as in `Person.Anonymous()`. A `static` field is a constant and cannot be assigned (mutable global state cannot be held).
- To assign to a public field from outside the class, the variable must be `mut`.
- There is no inheritance. To use the features of another type, hold that type in a field and write forwarding methods for what you need.

```
class Labeled {
    Inner: Circle
    Label: String

    fn Init(inner: Circle, label: String) {
        this.Inner = inner
        this.Label = label
    }
    fn Area() -> F64 { this.Inner.Area() }     // forwarding is written by hand
}
```

### Creation (Init and New)

- The constructor is `Init`. Creation is `Type.New(...)`, a static function made by the compiler.
- `New` allocates the area, puts in the initial values of the fields, calls `Init(...)` and returns the reference (or the value). `New` always succeeds.
- `Init` cannot be called directly (`p.Init(...)` is an error). It has no return value. `Init` is implicitly a `mut fn`.
- By the time `Init` is left, all fields must be initialized (fields with an initial value may be omitted).
- A class without an `Init` also gets a `New`. Then **all fields need an initial value**.
- There is only one `Init` (no overloading). Several ways of creating are made as static functions (`Point.Origin()`).
- Creation that can fail (opening a file, parsing) is not a `New` but a static function that returns a `Result`.
- `struct` has `New` too: `let p: Point = Point.New(1, 2)`.
- `extern struct`, which should have the same layout as a C struct, is in the chapter "C and C++ interop".

### enum

```
enum Shape {
    Circle(F64),
    Rect(F64, F64),
    Named { Name: String, Size: F64 },
    Empty,

    fn Area() -> F64 {
        match (this) {
            Shape.Circle(r) => 3.14 * r * r,
            Shape.Rect(w, h) => w * h,
            Shape.Named { Size: s, .. } => s * s,
            Shape.Empty => 0.0,
        }
    }
}

let s: Shape = Shape.Circle(2.0)
```

- There are three kinds of variants: without data (`Empty`), with positional arguments (`Circle(F64)`) and with names (`Named { ... }`).
- The value of a named variant is made in the same form as the pattern: `Shape.Named { Name: "x", Size: 3.0 }` (all fields are needed, in any order; the expressions are evaluated in the order of the declaration). Making a positional variant with `{ }`, or a named variant with `( )`, is an error (added on 2026-10-31).
- A value type. `==` and `Hashable` are added automatically when the content supports them.
- Variants are qualified with the type name, as in `Shape.Circle`. Only `Some`, `None`, `Ok` and `Err` can be written without qualification.
- Methods and implementations of `interface`s can be written. There is no inheritance.
- When an `enum` holds itself (recursion), it holds it through a `class` or an `Array` field.
- `Option` and `Result` are defined by the standard library with this mechanism.

```
enum Option<T> { Some(T), None }
enum Result<T, E> { Ok(T), Err(E) }
```

### interface

```
interface Shape {
    fn Area() -> F64
    fn Describe() -> String {            // a default implementation
        "area = ${this.Area()}"
    }
}

class Circle : Shape {
    r: F64
    fn Init(r: F64) { this.r = r }
    fn Area() -> F64 { 3.14 * this.r * this.r }
}

let s: Shape = Circle.New(2.0)
```

- The `interface`s to implement are listed after `:` separated by commas, as in `class Dog : Hashable, Printable` (the same for `struct`).
- An `interface` can have default implementations. If the defaults of two `interface`s collide, it is an error to be fixed by rewriting it in the type.
- An `interface` can extend another `interface` (`interface Hashable : Equatable`). It only adds requirements and holds no state.
- A value of an `interface` type is held as a pair (a reference to the object, a function table), and calls are resolved dynamically.
- An `interface` that uses `Self` (`Equals(other: Self)`) is **only for generic constraints** and cannot be used as the type of a value.
- The return value of an implementing method may be a more concrete type than in the `interface` declaration (for example `Iter()` returns a concrete `ListIter<T>`).
- Generic methods cannot be put in the function table of an `interface`. They can be written as default implementations (made for each type used).

### Standard interfaces

```
interface Equatable { fn Equals(other: Self) -> Bool }
interface Hashable : Equatable { fn Hash() -> U64 }
interface Comparable : Equatable { fn Compare(other: Self) -> I32 }   // negative / 0 / positive

interface Iterator<T> { mut fn Next() -> Option<T> }
interface Iterable<T> {
    fn Iter() -> Iterator<T>
    // the following are default implementations (writing only Iter() is enough to use them)
    fn ToArray() -> Array<T>
    fn Each(f: fn(T) -> Unit)
    fn EachIndexed(f: fn(I64, T) -> Unit)
    fn Map<U>(f: fn(T) -> U) -> Array<U>
    fn Filter(f: fn(T) -> Bool) -> Array<T>
    fn Reduce<U>(init: U, f: fn(U, T) -> U) -> U
    fn Find(f: fn(T) -> Bool) -> Option<T>
    fn Any(f: fn(T) -> Bool) -> Bool
    fn All(f: fn(T) -> Bool) -> Bool
    fn Count(f: fn(T) -> Bool) -> I64
}
```

- `Map` and `Filter` return arrays (they are not lazy).
- `Array`, `Map` and ranges (`0..n`) already implement `Iterable`.
- Every type has `ToString()`. For primitives it is built in; for `struct`, `enum` and `class` a default is made that prints the type name and the fields. If you write `fn ToString() -> String` yourself, that one is used (`"${x}"` calls `x.ToString()`).

### Generics

```
class Stack<T> {
    items: Array<T> = []
    mut fn Push(x: T) { this.items.Push(x) }
    mut fn Pop() -> Option<T> { this.items.Pop() }
}

class Cache<K: Hashable, V> { ... }
fn Largest<T: Comparable + Hashable>(a: Array<T>) -> Option<T> { ... }
```

- `struct`, `class`, `enum`, `interface` and functions can take type parameters.
- Constraints are written `<K: Hashable>`, and several are joined with `+`. There is no `where`.
- Separate code is made for each type argument used (monomorphization).
- Constraints come in implementation stage 5 (together with `interface`). Until then the keys of a `Map` are only built-in types.

### Destruction

Destructors and finalizers are not shown to users. Cleanup of things like files is done with `defer`.

## Modules and import

Code is divided into **modules**. A file always begins with exactly one `module` declaration (it cannot be omitted).

```
module Main

import Zevic.Console
import MyApp.Util.Math as M

fn Main(args: Array<String>) {
    Console.Println("sum = ${M.Add(1, 2)}")        // it can also be written Zevic.Console.Println
}
```

### module

- The name is PascalCase separated by `.` (`MyApp.Util.Math`). Every part starts with a capital letter.
- Several files that declare a `module` of the same name form one module.
- The module name matches the directory structure in the package (`src/Util/Math.zev` is `MyApp.Util.Math`). Only the `src/Main.zev` of an executable is `module Main`.
- The standard library starts with `Zevic.` (`Zevic.Console`, `Zevic.Fs`). `Zevic` is reserved and cannot be used for the name of a user's package.

### import

- `import` is written with the full path. There is no wildcard (`import X.*`) and no way to import only some of the names.
- Without `as`, the full path is also used when using the module (`MyApp.Util.Math.Add(1, 2)`).
- **The standard library can omit the leading `Zevic`**. After `import Zevic.Console`, both `Console.Println("hi")` and `Zevic.Console.Println("hi")` can be written (they are the same). If the omitted name collides with another `import` or one of your own names, it is an error, so use the full path or `as`.
- To write it shorter, use `as` (`import Zevic.Console as C` gives `C.Println("hi")`).
- With `as`, the original name can no longer be used (only `C.Println`).
- A collision of aliases is an error. Importing the same module twice is also an error.
- Put the `import`s together at the top of the file, directly after `module`.
- **Circular `import`s are forbidden.** The dependencies must go in one direction (the compiler shows the path of the cycle).

### Visibility

| Start of the name | Where it is visible |
| --- | --- |
| upper case | public (can be used from other modules) |
| lower case | private (can be used only from files of the same module) |

- It applies to all of functions, types, fields, methods and `static` fields.
- Even when a type is public, its lower-case fields are not visible from outside.
- There are no keywords such as `private`.

### The prelude

The only names that can be used without `import` are the following.

- Types: `I8` `I16` `I32` `I64` `U8` `U16` `U32` `U64` `F32` `F64` `Bool` `Char` `String` `Unit` `Array` `Map` `Option` `Result` `Error`
- Variants: `Some` `None` `Ok` `Err`
- Built-ins: `Panic` `Assert`
- Standard `interface`s: `Equatable` `Hashable` `Comparable` `Iterable` `Iterator`

Anything else (`Console`, `Math`, `Fs` and so on) needs an `import`.

### The entry point

- An executable (`kind = "bin"`) starts at the public function `Main` of `module Main`. There must be **exactly one**.
- Only two forms are allowed: `fn Main()` and `fn Main(args: Array<String>)`.
- `args` are the command-line arguments. The program name is not included (`args[0]` is the first argument).
- The exit code is returned with `Env.Exit(code)` (needs `import Zevic.Env`). A form where `Main` returns a `Result` is not included for now.
- A library (`kind = "lib"`) has no `module Main`.

### Search rules

1. The directory that has `zevic.toml` is the root of the package.
2. The modules of your own package are found from the directory structure under `src/`.
3. Dependency packages (fetched with `zevpm get`) are found by the names in `[deps]` of `zevic.toml`. The package name of the dependency becomes the first part of the module name.
4. The standard library is the `Zevic` package bundled with the compiler (`packages/Zevic/`, an ordinary zevpm package with `zevic.toml` and `src/`). The check of `zevic = ">=..."` is done against the version of this package.

```
myapp/
  zevic.toml
  src/
    Main.zev          // module Main
    Util/Math.zev     // module MyApp.Util.Math
```

## Error handling and Panic

Errors are divided into two kinds. **Failures that can happen** are returned as a `Result`, and **bugs in the program** stop it with a `Panic`. There are no exceptions.

| Kind | Examples | Handling |
| --- | --- | --- |
| Failures that can happen | a missing file, a string that is not a number, a lost connection | return `Result<T, Error>` |
| Bugs | an array index out of range, a failed `Unwrap`, integer overflow, division by zero | `Panic` (stops the program) |

### Result and ?

```
import Zevic.Fs

fn ReadPort(path: String) -> Result<U16, Error> {
    let text: String = Fs.ReadAll(path)?           // on Err, return from the function on the spot
    let n: I32 = text.Trim().ToI32()?
    if (n < 0 || n > 65535) {
        return Err(Error.New(ErrorKind.Invalid, "port out of range: ${n}"))
    }
    Ok(n.ToU16())
}
```

- `expression?` takes out `x` for `Ok(x)`, and for `Err(e)` does a `return` from the function with that `e`.
- `?` can be used only inside a function whose return value is `Result<_, Error>`. The error type is fixed to `Error`, so no conversion is needed.
- `?` cannot be used on an `Option` either. Turn it into a `Result` with `OkOr` first.
- When it succeeds with no value, return `Ok(())`.
- A `Result` cannot be ignored. A call whose return value is not used gives a warning (to discard it explicitly, use `let _: ... =`).

### Error

```
class Error {
    Kind: ErrorKind
    Message: String
    Cause: Option<Error>
}

enum ErrorKind { NotFound, Invalid, Permission, Io, Parse, Timeout, Other }
```

| Method | Meaning |
| --- | --- |
| `Error.New(kind, message)` | create |
| `e.Kind()` / `e.Message()` / `e.Cause()` | read |
| `r.Context(msg)` | on `Err`, wrap it in a new `Error` and keep the original as the `Cause` |
| `r.MapErr(f)` | convert the content of the `Err` |
| `o.OkOr(e)` | turn an `Option` into a `Result` (`Err(e)` for `None`) |
| `r.Unwrap()` / `r.UnwrapOr(d)` | Panic on `Err` / a default value |
- `ErrorKind` is only a classification; detailed information goes in the message.
- Your own types cannot be put in an `Error`. When the classification is not enough, use `Other` and tell them apart by the message.
- `ToString()` prints `kind: message`, followed by `caused by: ...` if there is a cause.

### Panic

```
Panic("unreachable state")        // can be written in the position of an expression of any type
Assert(x > 0, "x must be positive")
```

- `Panic(msg)` writes the message, the file name and the line number to standard error and exits with **exit code 101**. It cannot be recovered from.
- The return type is `Never` (as an expression it can be any type).
- `Assert(cond, msg)` does a `Panic` when `cond` is `false`. It does not disappear in release builds.
- Built-in operations that cause a Panic:
  - `a[i]` out of range, and a missing key in `m[k]`
  - `Unwrap()` hitting `None` / `Err`
  - integer arithmetic overflowing, division or remainder by `0`, a shift amount of the width or more
  - those `ToXxx()` that do not return a `Result` when out of range (see the conversion table in the chapter on types)
- For operations that should wrap around, use `WrapAdd`, `WrapSub` and `WrapMul`.
- `defer` is **not run** before a Panic (the program stops at once). Write code that needs cleanup on the assumption that the OS cleans up.
- A Panic inside an `export "C"` function called from the C side also ends the process as it is (it does not return to the C side).

### Exit codes

| Exit code | Meaning |
| --- | --- |
| 0 | `Main` returned normally |
| 101 | `Panic` |
| any | `Env.Exit(code)` |

### Relation to tests

A `Panic` inside a `test` block makes that test fail and moves on to the next test (see the chapter on tests).

## Memory management

Zevic manages memory with a GC (garbage collector). Users do not write deallocation. Low-level control of memory is not a goal of this language.

### Policy

- The implementation uses the vendored **Boehm GC**. It is a conservative GC, so the compiler does not have to make stack maps.
- `class`, `Array`, `Map`, `String` and the environments of closures are all placed on the GC heap. `struct` and `enum` are values, placed inside their owner (a variable, a field, an element of an array).
- Allocation is concentrated in runtime functions. The code the compiler generates allocates in no other way.

| Function | Use |
| --- | --- |
| `zevic_alloc(size)` | an area that may contain pointers (`class`, `Array<class>` and so on) |
| `zevic_alloc_atomic(size)` | an area that contains no pointers (the content of a `String`, `Array<I32>` and so on). The GC does not examine the content. |

- There is no way to call deallocation. There is no `Drop` or `Dispose` either.
- **OS resources** such as files and sockets are closed with `defer` (do not rely on the timing of GC collection).

```
import Zevic.Fs

let f: Fs.File = Fs.Open(path)?
defer f.Close()
```

### Stages of implementation

1. The first compiler (`zevic0`) runs without a GC. `zevic_alloc` just calls `malloc` and never frees.
2. Once it works, the content of `zevic_alloc` is replaced with the Boehm GC. The callers do not change.
3. The initialization of the runtime (`GC_init`) is called at the start of the `main` the compiler makes.

### Threads and the GC

- Threads (`Zevic.Async`) use the thread-aware build of the Boehm GC. Threads are created through a wrapper of the runtime.
- The state of the runtime (error numbers, the working values for starting processes, the target of the test `longjmp`) is kept per thread.

### Settings

- Tuning of the GC (the upper limit of the heap, the frequency of collection) is done with environment variables (`ZEVIC_GC_...`). It cannot be touched from source code.
- A `NoGC` mode and `unsafe` are not in the specification for now (see "Open questions and future considerations").

### The boundary with C

The rules for passing GC-managed memory to C are collected in "The boundary with the GC" in the chapter "C and C++ interop". The main points are:

- Passing the address of GC memory to C and using it only during the call is safe (during the call, the pointer on the stack keeps the area alive).
- When **C keeps it after the call**, put it in a `Handle` so that the GC does not collect it.
- Memory that was `malloc`ed is outside the GC. `free` is done on the C side.

## C and C++ interop

Zevic can call C functions and expose Zevic functions to C. C++ is used through functions wrapped in `extern "C"`. No header files are needed; you write `extern` declarations.

### Importing (extern)

```
import Zevic.Console

extern "C" {
    fn puts(s: CString) -> CInt
    fn abs(n: CInt) -> CInt
    fn getenv(name: CString) -> CString
}

fn Main() {
    let r: CInt = puts("hello".ToC())
    Console.Println("puts returned ${r.ToI32()}")
}
```

- Declare C functions with `fn` in an `extern "C" { ... }` block. No body is written.
- A declaration can be used **only inside that module**. It is not re-exported to other modules. To make it public, wrap it in a Zevic function.
- The name is the C name as it is (no mangling). To change the C name, give an alias in the form `fn Foo() -> CInt = "c_name"`.
- Calls are the same as for ordinary functions. But the parameters and the return value are **C types only**.

### Exporting (export)

```
import Zevic.Console

export "C" fn Greet(name: CString) -> CVoid {
    Console.Println("hello, ${String.FromC(name)}")
}
```

- A function marked `export "C"` is emitted with a name callable from C (no mangling).
- The parameters and the return value are C types only.
- An ordinary function has its name changed to the form `zevic_<Module>_<Name>`, and is not called directly from C.
- `Main` cannot be `export`ed.
- A Panic inside an `export`ed function ends the process (it does not return to C).
- `export`ing the same name in two places is an error.

### C types

There are types for all the basic types of C. The names start with `C`.

| Zevic type | C type | Notes |
| --- | --- | --- |
| `CChar` / `CSChar` / `CUChar` | `char` / `signed char` / `unsigned char` | `CChar` is the `char` of the implementation |
| `CShort` / `CUShort` | `short` / `unsigned short` |  |
| `CInt` / `CUInt` | `int` / `unsigned int` |  |
| `CLong` / `CULong` | `long` / `unsigned long` | the width differs between Windows and Linux |
| `CLongLong` / `CULongLong` | `long long` / `unsigned long long` |  |
| `CSizeT` / `CSSizeT` / `CPtrDiff` | `size_t` / `ssize_t` / `ptrdiff_t` |  |
| `CIntPtr` / `CUIntPtr` | `intptr_t` / `uintptr_t` |  |
| `CFloat` / `CDouble` | `float` / `double` |  |
| `CBool` | `_Bool` |  |
| `CString` | `const char*` | NUL-terminated |
| `CVoid` | `void` | can be written only as a return value |
| `*T` | `T*` | a pointer |

- The widths of the C types are decided by the compiler to match the C compiler of that platform.
- The signedness of `CChar` differs between implementations. If you need a sign, use `CSChar` or `CUChar`.

### Conversions (no implicit conversions)

Zevic types and C types are **not converted automatically**. Convert explicitly.

| Conversion | How to write it |
| --- | --- |
| `I32` to `CInt` | `x.ToCInt()` |
| `CInt` to `I32` | `c.ToI32()` |
| `I64` to `CLongLong` | `x.ToCLongLong()` |
| `CLongLong` to `I64` | `c.ToI64()` |
| `CLong` to `I64` | `c.ToI64()` |
| `CSizeT` to `U64` | `c.ToU64()` (on systems where the widths do not match, use the version that returns a `Result`) |
| `F64` to `CDouble` | `x.ToCDouble()` |
| `CDouble` to `F64` | `c.ToF64()` |
| `String` to `CString` | `s.ToC()` |
| `CString` to `String` | `String.FromC(c)` |

- A `CString` made with `.ToC()` is valid **only during the call**. If the C side keeps it, copy it outside the GC with `NewString`.
- `String.FromC(c)` **copies** the content of `c`, checks it as UTF-8 and makes a `String`. When it is not UTF-8, it is not a Panic; use `String.FromCLossy(c)` (it turns invalid bytes into `U+FFFD`).
- For conversions that can go out of range (`CLong` to `CInt` and so on), there are `ToCInt()`, which returns a `Result`, and `ToCIntWrap()`, which wraps around.

### Pointers

```
extern "C" {
    fn memcpy(dst: *CVoid, src: *CVoid, n: CSizeT) -> *CVoid
    fn qsort(base: *CVoid, n: CSizeT, size: CSizeT, cmp: fn(*CVoid, *CVoid) -> CInt) -> CVoid
}
```

- A pointer is written `*T`. `*CVoid` is `void*`.
- A pointer can **only be passed to C functions and received from them**. Syntax such as `*p` for reading and writing the content is not included for now.
- Pointers can be compared (`==`), including with `Null`. `Null` is a constant that can be used only for pointer types.
- The address of the start of an `Array<T>` is obtained with `a.Ptr()`. This value is valid **only during the call** (the GC does not move memory, so the position does not change, but it becomes stale when the array grows).
- For callbacks, Zevic functions (those that do not capture a closure) can be passed as `fn` types. Lambdas that capture cannot be passed.

### Variadic arguments

To import a C function with variadic arguments such as `printf`, use `...`.

```
extern "C" {
    fn printf(fmt: CString, ...) -> CInt
}

printf("%d %s\n".ToC(), 42.ToCInt(), "x".ToC())
```

- Only C types can be passed for the `...` part.
- Zevic's `Printf` is something else: it checks the types and the number of the format arguments at compile time (see the chapter on the standard library).

### extern struct

You can declare a type with the same layout as a C struct.

```
extern "C" {
    struct Point { X: CInt, Y: CInt }
    fn Move(p: *Point, dx: CInt) -> CVoid
}

mut p: Point = Point.New(1, 2)
Move(&p, 10.ToCInt())
```

- The layout and padding follow the same rules as the C compiler.
- Fields can be only C types or other `extern struct`s.
- An `extern struct` is a value type like `struct`. A pointer can be taken with `&p` (only of a `mut` variable).
- `union`s and bit fields are not handled.

### The boundary with the GC

The GC does not know **that a pointer passed to the C side has been stored in C memory**. To make C hold memory managed by the GC, follow these rules.

| Situation | Rule |
| --- | --- |
| Used only during the call | it can be passed as it is |
| The C side keeps it after the call | put it in a `Handle` and pass the number of the Handle (`CULong`). When finished, call `h.Release()`. |
| The C side keeps a string | copy it outside the GC (`malloc`) with `NewString(s)`. `free` it on the C side. |
| Receiving memory that C `malloc`ed | handle it as a `CString`, copy it with `String.FromC`, and `free` the original |

```
let h: Handle<Counter> = Handle.New(counter)    // registered so that the GC does not collect it
RegisterCallback(h.Id())                        // pass the number to C
...
h.Release()                                     // remove it when no longer needed
```

- `Handle<T>` registers the reference of a `class` in a table where it can be put in and taken out by number. While it is in the table, the GC does not collect it.
- A number that comes back from the C side is turned back into an `Option<T>` with `Handle.Get<T>(id)`.

### zevic.h

- A package that has `export "C"` functions generates **`zevic.h`** at build time (`build/include/<package>.h`).
- From C, use it by `#include`ing that header.
- If the header is not needed, set `header = false` in `zevic.toml`.

### Building and linking

```
zevic hello.zev native/hello.c -o hello
```

- Files listed on the command line are handled according to their **extension** (the table below).
- In a package, the files under `native/` are added to the build automatically.
- In `[native]` of `zevic.toml`, write the libraries to link and the search paths.

```
[native]
libs = ["m", "sqlite3"]
lib_dirs = ["/opt/lib"]
include_dirs = ["native/include"]
defines = ["FOO=1"]
```

### Extensions and languages

| Extension | Handling |
| --- | --- |
| `.zev` | Zevic source |
| `.c` | C (compiled with `clang`) |
| `.cpp` `.cc` `.cxx` | C++ (compiled with `clang++`) |
| `.m` `.mm` | Objective-C / C++ (macOS only) |
| `.s` `.S` | assembly (`clang`) |
| `.o` `.obj` `.a` `.lib` `.so` `.dylib` `.dll` | linked as they are |

- For C++ files, Zevic can call only the functions wrapped in `extern "C"` (C++ name mangling, classes and templates are not handled directly).
- When C++ is included, `clang++` is used for linking, and the C++ standard library is linked too.
- The external compiler for each language can be given with the `CC` and `CXX` environment variables, or with `--cc` and `--cxx`.
- It is built so that external languages can be added by adding one line to the table of extensions.

### The native/ directory

```
myapp/
  zevic.toml
  src/
    Main.zev
    Sqlite.zev          // extern declarations and the wrapper
  native/
    wrapper.c
    wrapper.cpp
    include/wrapper.h
```

- C/C++ files in `native/` are each compiled automatically at `zevpm build`.
- Subdirectories under `native/` are searched recursively too.
- Put headers in `native/include/` (the default of `include_dirs`).
- A `.c` and a `.cpp` with the same name give the same `.o`, so do not let the names overlap.

### Style guidelines

- Gather the `extern` declarations of external functions **in one module**, and publish them through wrapper functions on the Zevic side that use `Result` and Zevic types.
- Do not show C types to other modules (users do not need to be aware of C types).
- The `Zevic.C` module has tools such as the conversions of C types, `Handle`, `NewString` and `Free`.

## Standard library, first batch

The first things to implement are Core, Array, Map, Console and C. Names start with `Zevic.`, as in `Zevic.Core`. The types of Core and the prelude can be used without `import`.

### Core

```
Panic(msg: String) -> Never
Assert(cond: Bool, msg: String)

// integers (I8 to I64, U8 to U64)
x.ToString() -> String
x.ToF64() -> F64                 // a conversion that cannot fail
x.ToI64() -> I64                 // to a wider type a value is returned
x.ToI32() -> Result<I32, Error>  // to a narrower type, Err when out of range
x.ToI32Wrap() -> I32             // out of range wraps around
x.WrapAdd(y) / WrapSub(y) / WrapMul(y)   // operations that wrap around
x.Abs() -> the same type  x.Min(y)  x.Max(y)  x.Clamp(lo, hi)

// floating point (F32, F64)
x.ToI32() -> Result<I32, Error>  // the fractional part is cut off. NaN and out of range are Err
x.IsNaN() -> Bool  x.IsInf() -> Bool

// String (UTF-8, immutable, positions are in bytes)
s.Len() -> I64                   // number of bytes
s.CharCount() -> I64             // number of characters
s.IsEmpty() -> Bool
s.ToI32() -> Result<I32, Error>  // ToI64, ToF64 and so on likewise
s + t,  s == t,  s < t

// Bool / Char
x.ToString() -> String

// Option<T>
o.IsSome() / o.IsNone() -> Bool
o.Unwrap() -> T                  // Panic for None
o.UnwrapOr(d: T) -> T
o.OkOr(e: Error) -> Result<T, Error>

// Result<T, Error>
r.IsOk() / r.IsErr() -> Bool
r.Unwrap() -> T                  // Panic for Err
r.UnwrapOr(d: T) -> T
r.Context(msg: String) -> Result<T, Error>
r.MapErr(f: fn(Error) -> Error) -> Result<T, Error>

// Error
Error.New(kind: ErrorKind, message: String) -> Error
e.Kind() / e.Message() / e.Cause()
```

- `Min`, `Max` and `Clamp` can be used on all `Comparable` types.
- For the rules of conversion see the chapter "Types": a conversion that cannot fail returns a value, and a conversion that can fail returns a `Result`.
- The names of the `Wrap*` family are settled as WrapAdd, WrapSub and WrapMul. Use them only when you do not want a Panic on overflow.

### Array

```
[1, 2, 3]                          // a literal
Array<T>.New() -> Array<T>
a.Len() -> I64
a.IsEmpty() -> Bool
a[i] / a[i] = x                    // out of range is a Panic
a.Get(i: I64) -> Option<T>
a.First() / a.Last() -> Option<T>

// changing (can be called only from a mut variable)
mut fn Push(x: T)
mut fn Pop() -> Option<T>
mut fn Insert(i: I64, x: T)        // out of range is a Panic
mut fn RemoveAt(i: I64) -> T       // out of range is a Panic
mut fn Clear()
mut fn Reverse()
mut fn Sort()                      // T: Comparable
mut fn Extend(other: Array<T>)
// returns a new array
a.Slice(from: I64, to: I64) -> Array<T>
a.Concat(other: Array<T>) -> Array<T>
a.Map<U>(f: fn(T) -> U) -> Array<U>
a.Filter(f: fn(T) -> Bool) -> Array<T>

// searching and aggregating (the default implementation of Iterable)
a.Contains(x: T) -> Bool           // T: Equatable
a.IndexOf(x: T) -> Option<I64>     // T: Equatable
a.Find / Any / All / Count / Reduce / Each / EachIndexed
a.Join(sep: String) -> String      // for an Array<String>

a.Ptr() -> *T                      // for C interop. Valid only during the call
```

- Lengths and positions are `I64`.
- `Sort` is a stable sort. Comparison uses `Compare`.
- The ranges `0..n` and `0..=n` are `Iterable<I64>` and can be used in `for`.

### Map

```
Map<K, V>.New() -> Map<K, V>       // K: Hashable
m.Len() -> I64
m.Get(k: K) -> Option<V>
m[k]                               // Panic if missing
m.Contains(k: K) -> Bool
m.Keys() / m.Values() -> Array
mut fn Set(k: K, v: V)
mut fn Remove(k: K) -> Option<V>
mut fn Clear()
for ((k, v) in m) { ... }          // in insertion order
```

- The order of iteration is the **order of insertion**.
- Changing the `Map` during iteration is a Panic.
- `Set<T>` is provided too (`Add`, `Contains`, `Remove` and `Len`).

### Console

```
Print(s: String)
Println(s: String)
Printf(fmt: String, ...)           // %d %u %x %f %s %c %%, with width and precision
Eprint / Eprintln / Eprintf        // standard error output

ReadLine() -> Option<String>       // without the newline. EOF is None
Read() -> Option<Char>             // one character. EOF is None
ReadAll() -> String
Prompt(msg: String) -> Option<String>   // show the message, then ReadLine
```

- If output fails, nothing happens (they return `Unit`). A failure of input is `None`, as EOF.
- If the format string of `Printf` is a literal, the types and the number are **checked at compile time**. If it is a variable, it is checked at run time, and a mismatch is a Panic.
- `%d` accepts any width of integer (decided from the type).
- `Text.Format(fmt, ...) -> String` uses the same format as `Printf` (`%d %u %o %x %X %f %e %g %s %c %%`; flags, width and precision as in C) and returns a `String` without printing. The format must be a literal, and the types and the number are checked at compile time.
- String interpolation `"${x}"` calls `x.ToString()`.

### C

```
Types: CChar CInt CLong CSizeT CFloat CDouble CString CVoid ... (see the table in the chapter "C and C++ interop")
x.ToCInt() / c.ToI32()             // conversions
s.ToC() -> CString
String.FromC(c: CString) -> String
String.FromCLossy(c: CString) -> String
NewString(s: String) -> CString    // copied with malloc (free it on the C side)
Free(p: *CVoid)
Handle<T>.New(x: T) -> Handle<T>   // h.Id() / h.Release() / Handle.Get<T>(id)
```

## Standard library, second and third batches

After the first batch works, the second and third batches are implemented in that order. Here the role of each module and the policy for the main APIs are written. The detailed list of functions is decided just before implementation.

### Second batch

| Module | Role | Main APIs (policy) |
| --- | --- | --- |
| `Zevic.Fs` | files and directories | `ReadAll(path)`, `WriteAll(path, s)`, `Open(path, mode)`, `Exists(path)`, `List(dir)` and `Remove(path)`. All return a `Result`. A `File` is closed with `Close()` in a `defer`. |
| `Zevic.Text` | string operations | `Split`, `Join`, `Trim`, `Replace`, `StartsWith`, `EndsWith`, `Contains`, `ToUpper`, `ToLower` and `Lines`. Positions are in bytes. |
| `Zevic.Math` | mathematics | `Sqrt`, `Sin`, `Cos`, `Tan`, `Pow`, `Log`, `Exp`, `Floor`, `Ceil` and `Round`, and the constants `Pi` and `E` |
| `Zevic.Time` | time | `Now()`, `Duration` and `Sleep(d)`, and measuring elapsed time |
| `Zevic.Env` | environment | `Get(name)`, `Set(name, v)`, `ProgramName()`, `Exit(code)` and `RawArgs()` |
| `Zevic.Process` | starting external processes | `Run(cmd, args)` returns the exit code and the output as a `Result`. It is in the second batch because self-hosting calls `clang` and `git`. |

- `Fs` is made as a thin wrapper that calls the C runtime through `extern "C"`, and turns `errno` into an `ErrorKind` and a `Result`.
- Differences between OSes (Windows and POSIX) are absorbed by the bundled `.c` files.
- `Env.Exit` returns `Never`.

### Third batch

| Module | Role |
| --- | --- |
| `Zevic.Net` | TCP and UDP, an HTTP client |
| `Zevic.Thread` | threads, mutexes and channels |
| `Zevic.Async` | asynchronous processing |
| `Zevic.Json` | reading and writing JSON |
| `Zevic.Random` | random numbers |
| `Zevic.Path` `Zevic.Bytes` `Zevic.Chars` | path operations, byte strings (`Array<U8>`), classification and conversion of characters (`Char`) (v0.4.1) |
| `Zevic.Encoding` `Zevic.Digest` | hex, Base64 and URL encoding; CRC-32, Adler-32, SHA-256 and HMAC-SHA-256 (v0.4.1) |
| `Zevic.DateTime` `Zevic.Log` `Zevic.Cli` | dates and times (fixed offsets), logging, parsing command-line arguments (v0.4.1) |
| `Zevic.Regex` `Zevic.Csv` `Zevic.Toml` | regular expressions (a Pike VM), CSV (RFC 4180), TOML 1.0 (v0.4.1) |
| `Zevic.Deflate` `Zevic.Zlib` `Zevic.Gzip` `Zevic.Zip` `Zevic.Tar` | compression and decompression, reading and writing zip and tar archives (with zip-slip protection) (v0.4.1) |
| `Zevic.Test` | assertions for tests (see the chapter on tests). The real implementation is in implementation stage 6, ahead of the second batch. |

- The model of concurrency (`Thread` / `Async`) is undecided. It will be decided after looking at how it works with the GC (the thread support of Boehm).

### Naming rules

- Types are capitalized nouns, and operations are verbs.
- Conversions are `ToXxx`. Conversions and creation that can fail (`Open`, `Parse`) return a `Result`.
- Functions that return a Boolean start with `Is` or `Has`.
- Methods that change something are `mut fn`.
- Strings are fixed to UTF-8. Paths are also handled as UTF-8.

## zevic (the compiler)

`zevic` is the command of the compiler. It turns Zevic source into LLVM IR, compiles and links it with `clang`, and makes an executable.

### Usage

```
zevic hello.zev -o hello                 # build
zevic hello.zev native/a.c -o hello      # together with C
zevic hello.zev --emit-ll -o hello.ll    # emit only the LLVM IR
zevic --version
```

| Option | Meaning |
| --- | --- |
| `-o <file>` | the output file |
| `--emit-ll` | stop at LLVM IR (text) |
| `-O0` `-O1` `-O2` | optimization level (the default is `-O1`) |
| `-g` | debug information |
| `-l<name>` `-L<dir>` `-I<dir>` `-D<k=v>` | options for linking and C |
| `--cc <path>` `--cxx <path>` | specify the external compilers |
| `--test` | build an executable for tests |

### The flow of compilation

1. **Parsing**: lexing and parsing build the AST.
2. **Name resolution**: follow `module` and `import` and connect names. Circular `import`s are detected.
3. **Type checking**: check the types, infer generics, and check the exhaustiveness of `match` and the immutability of `let`.
4. **Monomorphization**: expand generics for each type used.
5. **Lowering**: turn closures into functions and environments, and expand `defer` and `?`.
6. **Emitting**: make the whole program **one `.ll`** (textual LLVM IR).
7. **Building**: `clang` compiles the `.ll` and the C/C++ files and links them with the runtime.

- The whole program is compiled at once (whole-program compilation). Separate compilation per module and incremental builds are not done for now.
- LLVM IR is emitted directly as text. There is no dependence on the C++ API or the libraries of LLVM.
- The approach of generating C from Zevic source is not taken.

### The external tool (clang)

- **clang 15 or newer** is required (it must be able to read textual LLVM IR, and pointer types must be opaque).
- `clang` is looked for in this order:
  1. the path given with `--cc`
  2. the environment variable `ZEVIC_CLANG`
  3. `clang` on `PATH`
  4. the usual install locations (`C:\Program Files\LLVM\bin` on Windows, Homebrew on macOS, `/usr/lib/llvm-*/bin`)
- When it is not found or the version is old, it prints how to install it (`winget install LLVM.LLVM`, `apt install clang`, `brew install llvm`) and exits.
- On Windows the MSVC libraries are needed, so the Visual Studio Build Tools are assumed to be installed.

### The runtime

- The runtime (`zevic_alloc`, strings, `Array`, `Map`, Panic and the body of `Printf`) is **written in C**. It is bundled with the compiler and compiled together at every build.
- The sources of the Boehm GC are bundled too (users do not need to install a GC).
- The names of the runtime functions that generated code calls all start with `zevic_`.

### Rules for generated code

| Zevic | LLVM IR |
| --- | --- |
| `I8` to `I64` / `U8` to `U64` | `i8` to `i64` (the sign is distinguished by the instruction) |
| `F32` / `F64` | `float` / `double` |
| `Bool` | `i1` (`i8` in memory) |
| `String` | `{ ptr, i64 }` (a pointer to the bytes and the length). The content is placed with a trailing NUL. |
| `class` | a `ptr` to a struct on the GC heap |
| `struct` / `enum` | a tagged struct held as a value |
| `interface` type | `{ ptr, ptr }` (the object and the function table) |
| `fn` type (function value) | `{ ptr, ptr }` (the function and the environment) |
| `Unit` | a type of size 0 (`void` as a return value) |

- Integer arithmetic is made with instructions that check for overflow (`llvm.sadd.with.overflow` and so on), and calls `Panic` when it happens.
- Range checks of arrays and maps are also put into the generated code.
- Names take the form `zevic_<Module>_<Name>` (periods become `_`). `export "C"` and `extern` are kept as they are.

### Error display

- An error shows the file name, line and column and the offending line, and explains the cause in one sentence.
- Errors that can be reported are reported together in one run (it does not stop at the first one).
- Exit codes: success `0`, compile error `1`, failure of an external tool `2`.

## zevpm

`zevpm` is the command for managing packages and builds. Use `zevpm` for everyday development, and use `zevic` directly only to try a single file.

### Commands

| Command | Meaning |
| --- | --- |
| `zevpm new <name>` | create a new package (`--lib` for a library) |
| `zevpm init` | make the current directory a package |
| `zevpm build` | build (`--release` to optimize) |
| `zevpm run [-- args]` | build and run |
| `zevpm test` | run the tests |
| `zevpm fmt` | format (calls `zevfmt`) |
| `zevpm clean` | delete `build/` |
| `zevpm get <repo>` | add a dependency and fetch it |
| `zevpm remove <name>` | remove a dependency |
| `zevpm update [name]` | bring dependencies up to date |

### Directory structure

`zevpm new myapp` creates the following structure.

```
myapp/
  zevic.toml          package settings
  zevic.lock          the fixed versions of the dependencies (commit it)
  src/
    Main.zev          module Main
  native/             C/C++ (only when needed)
  tests/              (optional) integration tests
  build/              generated files (do not commit)
  .gitignore
```

- With `--lib`, `src/<Name>.zev` is made instead of `src/Main.zev`.
- In `build/`, the `.ll`, the `.o`, the executable and `include/` are made.
- `.gitignore` lists `build/`.

### zevic.toml

```
[package]
name = "MyApp"
version = "0.1.0"
kind = "bin"            # "bin" or "lib"
zevic = ">=0.1"

[deps]
JsonPlus = { git = "https://github.com/someone/jsonplus", tag = "v1.2.0" }
LocalLib = { path = "../locallib" }

[native]
libs = ["m"]
```

- `name` is PascalCase. It becomes the first part of the `module` names.
- `version` is `major.minor.patch`.
- A dependency in `[deps]` is given with `git` (exactly one of `tag`, `branch` and `rev`) or with `path`.

### zevpm get

```
zevpm get github.com/someone/jsonplus@v1.2.0
zevpm get someone/jsonplus            # the GitHub short form
```

- It fetches from a **GitHub repository** by default, with `git clone --depth 1`.
- What is fetched is cached in `~/.zevpm/`. The same version is not fetched again and again.
- A line is added to `[deps]` of `zevic.toml`, and the commit hash is recorded in `zevic.lock`.
- When no tag is given, the latest commit of the default branch is used. Using a tag is recommended.
- There is no registry. Version resolution (when several dependencies ask for different versions of the same package) is not done either. A conflict is an error, and the user chooses.
- Dependencies of dependencies (transitive dependencies) are fetched in the same way.
- The `native/` of a dependency is also added to the build automatically.
- This command needs `git`.

### Output

- `zevpm build` is a wrapper that calls `zevic`. It collects all `.zev` files of `src/`, the files of `native/` and the dependencies, and passes them on.
- The executable is `build/<name>` (with `.exe` on Windows).
- With `kind = "lib"`, a static library and `include/<name>.h` are made in `build/`.

## zevfmt

`zevfmt` is the formatter. **It has no configuration file and no options**, and there is exactly one way of writing (the same idea as `gofmt`).

```
zevfmt file.zev          # the formatted result goes to standard output
zevfmt -w file.zev       # rewrite the file
zevfmt -check src/       # list the files that need formatting; exit code 1 if there are any
```

### Rules

| Item | Rule |
| --- | --- |
| Indentation | four spaces |
| Line length | lines are not wrapped (long lines stay as they are) |
| Braces | the opening brace is on the same line (`if (x) {`). `else` is on the same line as `}`. |
| Operators | one space on each side (`a + b`). No space for unary operators. |
| Commas | one space after. A comma after the last item of a multi-line list too. |
| Blank lines | consecutive blank lines become one. One blank line between functions. |
| `import` | sorted alphabetically |
| End of statement | no `;`. Trailing whitespace is removed, and the file ends with a newline. |
| Comments | the content is not changed. Only the position is aligned. |

- The meaning of the source does not change. The compiled result is the same whether it is formatted or not.
- A file with a syntax error is not formatted, and the error is shown.
- Formatting is **idempotent**. Formatting an already formatted result again changes nothing.
- `zevpm fmt` formats all the `.zev` files in the package.
- The implementation reuses the parser of the compiler (to keep comments, the parser keeps them).
- It is built in implementation stage 7. Until then, keep to these rules by hand.

## Tests

Tests are provided as a feature of the language. You write `test` blocks in the source and run them with `zevpm test`.

```
module MyApp.Util.Math
import Zevic.Test

fn Add(a: I32, b: I32) -> I32 { a + b }

test "Add adds two numbers" {
    Test.Equal(Add(1, 2), 3)
}

test "Add overflows" {
    Test.Panics(() => Add(2147483647, 1))
}
```

### How to write them

- Write `test "name" { ... }` at the top level of a file. The name is a string literal.
- They may be written inside the same module. Variables and functions, including private (lower-case) ones, are visible.
- Larger tests may be split into files ending in `_test.zev` (`Math_test.zev`). They are treated as the same module.
- `test` blocks and `_test.zev` files are **ignored in normal builds** and do not go into the executable.
- Files in the `tests/` directory are integration tests that use only public things.

### Zevic.Test

| Function | Meaning |
| --- | --- |
| `Test.Equal(actual, expected)` | they are equal (`T: Equatable`). When they differ, both values are shown. |
| `Test.NotEqual(a, b)` | they are not equal |
| `Test.True(cond)` / `Test.False(cond)` | truth values |
| `Test.Some(o)` / `Test.None(o)` | the state of an `Option` |
| `Test.Ok(r)` / `Test.Err(r)` | the state of a `Result` |
| `Test.Panics(f)` | `f` panics |
| `Test.Fail(msg)` | fail at once |

- Every assertion, when it fails, shows the file name and the line and aborts that test.

### Running

```
zevpm test                    # all
zevpm test Add                # only the tests whose name contains "Add"
```

- An executable for tests is built, and a runner that calls the tests in order goes into `main`.
- Each test is wrapped with `setjmp`. **Inside a test a Panic does not end the process**; it returns to the runner with `longjmp` and makes only that test fail.
- Things allocated on the heap inside a test may be carried over to the next test (leaks are not a concern).
- `defer` is not run in a test interrupted by a Panic.
- The display of the results:

```
running 3 tests
  ok    Add adds two numbers
  ok    Add overflows
  FAIL  Parse rejects empty input
        Math_test.zev:12: expected 1, got 0

2 passed, 1 failed
```

- If there are failures the exit code is `1`, otherwise `0`.
- Running tests in parallel, and benchmarks, are not included for now.

## Implementation plan

The first compiler, `zevic0`, is **written in C** and grown from small parts. In the future, the Zevic compiler is written in Zevic (self-hosting), and the C compiler becomes unnecessary.

### Stages

| Stage | Goal | What becomes possible |
| --- | --- | --- |
| 1 | Hello World | `module` / `fn` / strings / `Console.Println`. The parser, the skeleton of type checking, LLVM IR output and calling `clang` |
| 2 | Computation and control | integers and floating point, `let` and `mut`, `if`, `while`, `for` and `loop`, functions, overflow checks, Panic |
| 3 | Data | `struct`, `enum`, `match`, tuples, `Option`, `Result` and `?`, `Array`, `String` |
| 4 | Classes and C | `class`, `Init` and `New`, `interface`, `extern "C"` and `export "C"`, linking C files, `Map` |
| 5 | Abstractions | generics (constraints and monomorphization), closures, `defer`, `Iterable`, replacing with the Boehm GC |
| 6 | Tools | `zevpm` (`new`, `build`, `run`, `test` and `get`), `test` blocks, `zevic.toml`, the second batch of the standard library |
| 7 | Self-hosting | `zevfmt`, the compiler written in Zevic, bootstrapping |

- Making stage 1 work is the top priority. Until then, do not add features from the specification.
- At the end of each stage, put the samples and tests that work in that stage into the repository.
- `zevic0` does not free memory (no GC until stage 5). The compiler itself runs and ends quickly, so this is not a problem.

### The structure of zevic0 (C)

```
zevic0/
  lexer.c   parser.c   ast.c
  resolve.c        # module / import
  check.c          # type checking
  mono.c           # monomorphization
  lower.c          # closures, defer, ?
  emit_ll.c        # LLVM IR (text)
  driver.c         # calling clang
  runtime/         # zevic_alloc, strings, Array, Map, Panic
```

### Bootstrapping

1. Compile the compiler `zevic1`, written in Zevic, with `zevic0` (the one made in C).
2. Compile the same source with `zevic1` to make `zevic2`.
3. If the `.ll` files that `zevic1` and `zevic2` emit are **the same**, self-hosting has succeeded.
4. **Commit** the final `.ll` of this stage to the repository. Even without C, the compiler can be built with just `clang` right after cloning.

### Policy

- The implementation language is C (C11). The build is `clang` and a simple `Makefile`.
- The only dependency library is the bundled Boehm GC.
- At first, do not be greedy about error messages. File, line and cause are enough.
- Tests are basically golden tests: `.zev` files with the expected output next to them.

## Running the project

To grow the language over a long time, it is run as a GitHub organization. The name of the language is Zevic.

### GitHub organization

- The organization name is `zevic-lang` (`zevic` was already taken).
- It is created **private** at first, and made public at the stage where Hello World and the README work (v0.1.0).

| Repository | Content |
| --- | --- |
| `zevic` | the compiler (`zevic0`, later the self-hosted version) and the runtime |
| `zevpm` | the package manager (at first it may live in the same repository as `zevic`) |
| `packages/Zevic` | the standard library (`Zevic.*`), a package with a `zevic.toml` |
| `docs` | the language specification, tutorials and design notes |
| `examples` | sample code |
| `.github` | Issue / PR templates and the code of conduct shared by the organization |

### License

- The compiler, the tools and the standard library: **Apache-2.0** (it has a patent clause, and can be used together with the bundled Boehm GC (MIT-style)).
- The specification and the documents: CC BY 4.0 (or likewise Apache-2.0).
- The README says that generated executables may need to show the license notice of the runtime.

### Branches and review

- `main` is always kept in a state where the build and the tests pass. Do not push directly; go through a PR.
- Changes to the language specification are discussed in a proposal in `docs` (`proposals/NNNN-title.md`) and then applied.
- Commit messages are in English, with a summary on the first line.

### CI (continuous integration)

- With GitHub Actions, build and test on three systems: Linux, macOS and Windows.
- Content: building `zevic0`, the golden tests in `tests/`, compiling the samples, and `zevfmt -check`.
- After self-hosting, it also checks that the `.ll` of the bootstrap matches.

## Open questions and future considerations

### Open questions

| Item | Status |
| --- | --- |
| Concurrency | The model of `Thread` and `Async`. To be decided after looking at how it works with the Boehm GC. |
| Generic constraints | To be added in implementation stage 5. The policy is not to provide `where`. |
| `Integer` (arbitrary precision) | Whether to add it, and how to write literals |
| A `Main` that returns a `Result` | Not allowed for now. To be reconsidered when needed. |
| Reading and writing through pointers | The syntax `*p`. For now pointers are only passed. |
| Formats in string interpolation | Whether to add format specifiers such as `"${x:5.2}"` |

### Future considerations (not included now)

- A `NoGC` mode and `unsafe` (low-level control for OS development)
- `union`s and bit fields in `extern struct`
- Macros as a language feature (function arguments are of fixed length, and variable length is only `params` and `Printf`)
- Inheritance (the policy is not to add it; see whether composition and `interface` are enough while using the language)
- Incremental builds and compilation per module
- A package registry and version resolution
- A language server (LSP) and editor integration
- Direct use of C++ classes and templates
- Other LLVM targets such as WebAssembly
- Whether to let users write the `Never` type

## Appendix A: summary of the grammar (EBNF-like)

Notation: `A B` is concatenation, `A | B` is choice, `[A]` is optional, and `{A}` is repetition zero or more times. `'x'` is the character itself. A newline separates statements, and there is no `;`.

```
file        = 'module' modname { import } { decl }
import      = 'import' modname [ 'as' Ident ]
modname     = Ident { '.' Ident }

decl        = fnDecl | structDecl | classDecl | enumDecl | ifaceDecl
            | externBlock | exportFn | testDecl | constDecl
fnDecl      = [ 'mut' ] [ 'static' ] 'fn' Ident [ tparams ] '(' [ params ] ')' [ '->' type ] block
params      = param { ',' param } ; param = [ 'mut' ] Ident ':' type | 'params' Ident ':' type
tparams     = '<' tparam { ',' tparam } '>' ; tparam = Ident [ ':' type { '+' type } ]

classDecl   = 'class' Ident [ tparams ] [ ':' typelist ] '{' { field | fnDecl } '}'
structDecl  = 'struct' Ident [ tparams ] [ ':' typelist ] '{' { field | fnDecl } '}'
field       = [ 'static' ] Ident ':' type [ '=' expr ]
enumDecl    = 'enum' Ident [ tparams ] [ ':' typelist ] '{' variant { ',' variant } [ ',' ] { fnDecl } '}'
variant     = Ident [ '(' typelist ')' | '{' field { ',' field } '}' ]
variantLit  = path '{' Ident ':' expr { ',' Ident ':' expr } [ ',' ] '}'      (the value of a named variant; path ends in an upper-case name, as in Shape.Named)
ifaceDecl   = 'interface' Ident [ tparams ] [ ':' typelist ] '{' { fnSig | fnDecl } '}'

externBlock = 'extern' '"C"' '{' { cfn | cstruct } '}'
cfn         = 'fn' Ident '(' [ cparams ] ')' [ '->' type ] ; cparams may end with ',' '...'
exportFn    = 'export' '"C"' fnDecl
testDecl    = 'test' String block

block       = '{' { stmt } [ expr ] '}'
stmt        = 'let' pat ':' type '=' expr | 'mut' pat ':' type '=' expr
            | expr [ assignOp expr ] | 'return' [ expr ] | jump
            | 'defer' expr | [ label ] loopStmt
loopStmt    = 'while' '(' expr ')' block | 'loop' block
            | 'for' '(' forPat 'in' expr ')' block
label       = ident '@'
jump        = ( 'break' | 'continue' ) [ '@' ident ]    (* no space before or after '@' *)
forPat      = pat | Ident ',' pat

expr        = ifExpr | matchExpr | asyncExpr | 'await' expr | binary | unary | postfix | lambda | literal | '(' expr ')'
asyncExpr   = 'async' block
ifExpr      = 'if' '(' expr ')' block [ 'else' ( ifExpr | block ) ]
matchExpr   = 'match' '(' expr ')' '{' arm { ',' arm } [ ',' ] '}'
arm         = pat [ 'if' '(' expr ')' ] '=>' expr
lambda      = '(' [ lparam { ',' lparam } ] ')' '=>' expr
postfix     = primary { '.' Ident | '(' [ args ] ')' | '[' expr ']' | '?' }

pat         = '_' | literal | Ident | range | tuplePat | variantPat | structPat
type        = Ident [ '<' typelist '>' ] | '(' typelist ')' | 'fn' '(' [ typelist ] ')' '->' type | '*' type
```

- The precedence of operators follows the table in the chapter "Variables, expressions and operators".
- The parentheses of conditions and the `{}` are mandatory (`if (x) { ... }`).
- Reserved words: `module import as fn let mut static struct class enum interface test extern export if else match while loop for in break continue return defer async await this params true false`

## Appendix B: a complete sample program

One program that uses the main features of the specification (it counts how often words occur). The arguments are `count <file>` or `top <file> [count]`.

```
module Main

import Zevic.Console
import Zevic.Env
import Zevic.Fs
import Zevic.Test
import Zevic.Text

enum Command {
    Count(String),
    Top(String, I64),
}

struct Entry {
    Word: String
    Count: I64

    fn Init(word: String, count: I64) {
        this.Word = word
        this.Count = count
    }
}

class Counter {
    counts: Map<String, I64> = Map.New()

    mut fn Add(word: String) {
        let n: I64 = this.counts.Get(word).UnwrapOr(0)
        this.counts.Set(word, n + 1)
    }

    fn Distinct() -> I64 { this.counts.Len() }

    // The `limit` most frequent words, the most frequent first (equal counts keep the order of the words).
    fn Top(limit: I64) -> Array<Entry> {
        mut items: Array<Entry> = []
        for ((w, n) in this.counts) {
            items.Push(Entry.New(w, n))
        }
        items.SortBy((a: Entry, b: Entry) => b.Count.Compare(a.Count))
        let end: I64 = if (limit < items.Len()) { limit } else { items.Len() }
        items.Slice(0, end)
    }
}

fn Parse(args: Array<String>) -> Result<Command, Error> {
    if (args.Len() < 2) {
        return Err(Error.New(ErrorKind.Invalid, "usage: wc <count|top> <file> [n]"))
    }
    match (args[0]) {
        "count" => Ok(Command.Count(args[1])),
        "top" => {
            let n: I64 = if (args.Len() > 2) { args[2].ToI64()? } else { 10 }
            Ok(Command.Top(args[1], n))
        },
        other => Err(Error.New(ErrorKind.Invalid, "unknown command: ${other}")),
    }
}

fn Load(path: String) -> Result<Counter, Error> {
    let text: String = Fs.ReadAll(path).Context("cannot read ${path}")?
    mut counter: Counter = Counter.New()
    for (line in Text.Lines(Text.ToLower(text))) {
        for (word in Text.Split(line, " ")) {
            if (!word.IsEmpty()) { counter.Add(word) }
        }
    }
    Ok(counter)
}

fn Run(args: Array<String>) -> Result<Unit, Error> {
    match (Parse(args)?) {
        Command.Count(path) => {
            let c: Counter = Load(path)?
            Console.Printf("%d distinct words\n", c.Distinct())
        },
        Command.Top(path, n) => {
            let c: Counter = Load(path)?
            for (i, e in c.Top(n)) {
                Console.Printf("%2d. %-12s %d\n", i + 1, e.Word, e.Count)
            }
        },
    }
    Ok(())
}

fn Main(args: Array<String>) {
    match (Run(args)) {
        Ok(_) => {},
        Err(e) => {
            Console.Eprintln(e.ToString())
            Env.Exit(1)
        },
    }
}

test "Parse rejects an unknown command" {
    Test.Err(Parse(["zzz", "a.txt"]))
}
```

- This sample is the same as `examples/apps/wordcount/src/Main.zev` and runs as it is with the compiler implementation (`tests/examples_test.py` checks that they match).
- The `Zevic` of the standard library can be omitted (`Console.Println` and `Zevic.Console.Println` are the same).
- The first goal of implementation stage 1 is not this program but "Hello World" (`module Main` / `Console.Println`).
