# Proposal 0001: loop labels, `break@label` and `continue@label`

- Status: accepted (2026-11-03, implemented the same day)
- Date: 2026-11-03

## Motivation

The specification says "`break` and `continue` act on the innermost loop. There is no labeled `break` for now."
To leave a double loop at once, you have to use a flag variable, or move the loop into a function and `return`.

```
mut found: Bool = false
for (row in rows) {
    for (x in row) {
        if (x == target) { found = true; break }
    }
    if (found) { break }
}
```

## Proposal

As in Kotlin, a loop gets a label, and `break@label` and `continue@label` refer to an outer loop.

```
outer@ for (row in rows) {
    for (x in row) {
        if (x == target) { break@outer }
        if (x < 0) { continue@outer }
    }
}
```

- A label is `name@`, written on the same line directly before `while`, `loop` or `for`. Only these three can carry a label.
- In `break@name` and `continue@name` there is no space between `break`, `@` and the name (`break @x` and `break@ x` are syntax errors). The name after `@` is a label, not an expression.
- A label refers to an enclosing labeled loop **in the same function**. It cannot cross a lambda boundary (as before, a `break` inside a lambda only affects loops inside that lambda).
- An unknown label is a compile error. So is a label that has the same name as the label of an enclosing loop (labels do not shadow).
- The `defer`s between the `break@name` and the loop it leaves run from the inside out, as with `break`.
- `break` and `continue` without a label work as before, on the innermost loop.
- `break@name` and `continue@name` have the type `Never` as expressions (like `break`, they can be written in any expression position).
- A label name may start with a lowercase or an uppercase letter (labels do not cross modules, so there is no public / private distinction).

Grammar:

```
labeled   = ident '@' ( whileStmt | loopStmt | forStmt )
jump      = ( 'break' | 'continue' ) [ '@' ident ]     (no space before or after '@')
```

## Alternatives

- `break outer` (Swift / Go style): the label name is hard to tell from an expression, and it collides with a possible future where `break` takes a value. Not chosen.
- `'outer: for` (Rust style): `:` collides with type annotations and `'` with character literals.
- `outer: for` (Go / Java style): `:` is easily confused with type annotations and map-like notation.
- Moving the loop into a function, or returning an `Option`: still possible today. Labels do not get in the way when added.

## Impact

- Lexer: `@` becomes a new symbol (`TK_AT`). Today it is an error ("`@` is not used").
- Syntax: `name@ loop` at the start of a statement, and `@name` after `break` and `continue`.
- Code generation: the loop table keeps the names, and the jump target is chosen from it.
- `zevfmt`: no space before or after `@`. The body of a labeled loop is formatted as usual.
- Existing programs do not change (none used `@`).

## Tests

- Golden tests: `break@` and `continue@` in double and triple loops, in `while`, `loop` and `for`, and together with `defer`.
- Error tests: an undefined label, a label on a statement that is not a loop, a duplicate label, a label across a lambda, and a space after `break`.
- `zevfmt` is idempotent and keeps the AST unchanged.
