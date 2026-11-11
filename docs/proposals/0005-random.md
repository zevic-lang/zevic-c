# Proposal 0005: random numbers (`Zevic.Random`)

- Status: accepted (2026-11-03, implemented the same day)
- Date: 2026-11-03

## Motivation

Dice, shuffling and test data: random numbers are used often, even in small programs. The standard library has none, so you have to go through `Process` or C.

## Proposal

A new standard library module, `Zevic.Random`. Nothing is added to the runtime; it is written in Zevic alone.

```
import Zevic.Random

mut rng: Random.Rng = Random.Rng.New(42)       // fix the seed: the same seed gives the same sequence on every system
let dice: I64 = rng.Int(1, 7)                    // 1 or more, less than 7
let p: F64 = rng.Float()                         // 0.0 or more, less than 1.0
let up: Bool = rng.Bool()
let seeded: Random.Rng = Random.Rng.Seeded()     // the seed comes from the clock (different on every run)

Random.Shuffle(rng, xs)                          // reorder (Fisher-Yates)
let one: Option<I64> = Random.Pick(rng, xs)      // pick one (None when empty)
```

- The algorithm is SplitMix64. The state is a single 64-bit value; it is fast, and a one-bit difference in the seed changes the sequence a lot. The published reference values (seed 1234567 gives 6457827717110365317, 3203168211198807973, ...) are used in the tests.
- `Int(low, high)` avoids bias by throwing away values from the lopsided range and drawing again. `high <= low` is a `Panic`. It works even when the range is wider than `I64` (from `I64.Min` to `I64.Max`).
- **Not for cryptography.** The rest can be predicted from a few outputs. The documentation says so.
- The generator is a `class` (it has state). Do not share it between threads (make one per task).
- `Shuffle` and `Pick` are functions, not methods of `Rng` (to avoid generic methods).

## Not included

- Distributions such as the normal distribution, and the OS source of randomness (`/dev/urandom`). To be handled in a separate proposal when needed.
- A global generator (a style such as `Random.Int(...)`). It hides state and is hard to test.

## Impact

- The standard library only. No impact on the language, the runtime or `zevfmt`.

## Tests

- Reference values, the same sequence for the same seed, the balance of the dice (each face 9500 to 10500 times in 60000 rolls), negative ranges and the largest range, the range and the mean of `Float`, `Shuffle` keeping the elements, `Pick` and `Bool`.
