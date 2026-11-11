# Proposal 0004: assigning to a field of an array element (`xs[i].F = v`)

- Status: accepted (2026-11-03, implemented the same day)
- Date: 2026-11-03

## Motivation

Code such as `xs[i].Count += 1` is an error today. For a `class` element you have to take it out into a `mut` local variable and change that,
and for a `struct` element you have to rebuild the whole element and write `xs[i] = ...`. This shape comes up often in code that handles tables and grids.

```
mut t: Tok = toks[i]       // today's workaround
t.Skip = true
```

## Proposal

Allow `xs[i].F = v`, `xs[i].F += v`, `xs[i].A.B = v` and `xs[i][j].F = v`.

- Allowed only when the array (`xs`) is a `mut` variable or parameter, or a field of `this` inside a `mut fn`. A `let` array is still an error as before ("cannot assign to field ... because `xs` is not `mut`").
- If the element is a `class`, the object in place is changed through the reference.
- If the element is a `struct`, it is a value: a copy is taken out, changed, and written back to the array with `SetIndex`. Copies of the same value elsewhere do not change.
- Because of the write back, for `struct` elements the array and the index are limited to **simple expressions** (a variable, a field, a number). `ps[Next()].X = 5` is an error ("needs a simple array and index"), because the index would be evaluated twice. `class` elements have no such limit, since they are evaluated once.

## Alternatives

- Allowing only `class`: a rule that excludes only `struct` is hard to understand.
- Generating a function expression that moves the index into a variable: the compiler would add hidden variables. Limiting to simple expressions gives a shorter rule.

## Impact

- Only the assignment handling of the compiler. The syntax does not change. Existing code works as it is.
- No impact on `zevfmt`.

## Tests

- `class` and `struct` elements, nested `struct`s, arrays of arrays, fields of `this` inside a `mut fn`, and `+=`.
- A `let` array, a parameter that is not `mut`, and a complex index are errors.
