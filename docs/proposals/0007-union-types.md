# Proposal 0007: union types (`A | B`)

- Status: accepted (2026-11-06, implemented the same day)
- Date: 2026-11-06

## Motivation

A function that can fail is written with `Result<T, Error>` and `?`. But when you want to return one of two or three ad hoc kinds of value, such as "an `I32` or a `ParseError`", declaring an `enum` is heavy. With nameless union types, as in Kotlin or TypeScript, you can write the kinds of errors lightly and keep them in the type.

## Proposal

Union types in the style of Kotlin / TypeScript (`A | B`, `is`, smart casts, and a `match` like `when`).

```
fn Parse(s: String) -> I32 | ParseError { ... }

let r: I32 | ParseError = Parse("x")
if (r is ParseError) { Console.Println(r.Msg) } else { Console.Println("${r + 1}") }
match (r) {
    n: I32 => ...,
    e: ParseError => ...,
}
```

- Types: `A | B | C`. Nesting and order are normalized (flattened, duplicates removed, sorted by name). `Never` cannot be a member. At least two different types are needed.
- Conversions: only member to union, and union to a larger union that contains its members, are implicit. A numeric literal takes the type of the integer (decimal) member if there is exactly one.
- `x is T` and `x !is T`: the precedence is the same as comparison. `T` is a member.
- Narrowing: only for local variables that are not `mut`. In each branch of an `if`, on the right of `&&` and `||`, and after an `if` without `else` that ends in `return`, `break` or `continue`.
- `match` patterns: `name: Type` and `_: Type`. Exhaustiveness is checked.
- The value of a branch (`if`, `match`) is converted when a union is expected.
- Not possible: `==`, `${}`, `<` and so on (narrow first, then use them).

## Implementation

- Type: `T_UNION`. It holds the members sorted by name and has the same layout as an `enum` (`{ i32, [N x i64] }`). One variant per member (the payload is the member's value). The same set of members gives the same `Type` (interned).
- Syntax: `parse_type` joins types with `|` (`*A | B` is `(*A) | B`; `fn(A) -> B | C` is a function returning `B | C`; to make a function type a member, put it in parentheses: `(fn(A) -> B) | C`). `N_IS`, and `P_TYPE` for patterns (only inside `match` arms).
- Code generation: member to union writes the tag and the payload. Union to union remaps the tag with a table and copies the payload words. Narrowing creates a new local variable of the same name in the scope (it keeps the narrowed type even when a lambda captures it).
- Exhaustiveness: handled like the variants of an `enum` (the rows are narrowed per member).

## Alternatives

- A standard `enum` in the style of functional languages' `Either`: it would be enough if used with names, but lightness is the aim, so it is not adopted.
- Sugar for `T?` (Option): its role overlaps with `Option`, so it is not added now.
- Inferred unions (a union when the branches of an `if` have different types): the types would be hard to see, so a conversion happens only when a type is expected.

## Compatibility

`|` is new in type position. `|` in value position (a bit operation) and `|` in patterns do not change. `is` is a special word only after an operand (it can still be used as an identifier elsewhere).

## Open questions

- Sugar for `T?`, and cooperation with `Error` in `?` (propagating `I32 | Error` with `?`): for later.
- Narrowing of `mut` variables and fields.
- `==` and `ToString` for union types (generated automatically if the members support them).
