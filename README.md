# Filtered random pick benchmark

What is the cheapest way to pick one random element out of an array that matches
a predicate?

```sh
make run        # build, measure everything, write the CSVs
make plot       # run + draw the plots (needs python3 and gnuplot)
```

Needs a C++11 compiler (`clang++` or `g++`), `git` to clone
[SHISHUA](https://github.com/espadrine/shishua) and, optionally, `python3` +
`gnuplot` for plot rendering.

Tested on MacOS and Linux. If you make it work on Windows, or want to share your
unique results, please create a Pull Request.

## Room to improve

- MacOS
  - Pin to a Performance Core (P-core)
  - Use privileged instruction timestamping in MacOS (userspace in my M2 has
    24Mhz limit!) to subtract eviction times for cold cache measurements. Cold
    measurements for an M2 are unreliable.

## What gets measured

| binary     | question                                             | output       |
|------------|------------------------------------------------------|--------------|
| `./speed`  | how many ns does one random `uint64` cost?           | `speed.csv`  |
| `./select` | what is the cheapest way to pick a matching element? | `select.csv` |

`./plot.py` turns CSVs into PNGs under `plots/` in a per-machine subdirectory.

## The generators

| name       | what it is |
|------------|------------|
| `shishua`  | [SHISHUA](https://github.com/espadrine/shishua), a fast vectorised generator; NEON/AVX2/SSE2/scalar chosen at compile time |
| `lcg`      | 64-bit linear congruential generator, `s = s*a + c`, Knuth's MMIX constants |
| `lcg4`     | four independent LCG streams drained round-robin, so the multiplies pipeline |
| `xorshift` | Marsaglia's xorshift64 — a word-oriented linear feedback shift register |

Every bounded draw goes through Lemire's multiply-shift, `(x * range) >> 64`,
which reads the **high** bits. That matters: `x % range` reads the low bits,
which is exactly where the cheap generators are broken: an LCG's low bits have
a short period, so `x % 6` gives dice whose parity alternates every roll.

## The selection algorithms

Given an array of `{w, x, y, z}` structs, pick one uniformly at random from
those whose `w` falls in a range. Each strategy is a tag type carrying its own
name and a templated `pick`, so the label and the code are one declaration:

```cpp
struct ReservoirEvery {
  static constexpr const char *name = "reservoir_every";
  template <typename Rng>
  static const elem *pick(const elem *a, size_t n, picker<Rng> &p);
};
```

Both the generator and the strategy are template parameters, so every
combination is a separate fully-inlined instantiation and the calls at the
bottom of `main` *are* the experiment matrix — there is no dispatch table.

- **`reservoir_every`** — one pass, no scratch. Keep the *i*-th match with
  probability `1/i`. This is Algorithm R (Vitter 1985, credited there to Alan
  Waterman). It draws from the RNG for **every** match.

- **`reservoir_skip`** — same result, but it draws once per *replacement*
  instead of once per match, jumping straight to the next match that will win.
  This is the `k = 1` case of the skip idea behind Algorithm L (Li 1994).

  If match `i` is the current winner, it survives match `j` with probability
  `(j-1)/j`, so it survives all of `i+1..j` with `∏(m-1)/m = i/j`. Sampling the
  next winner as `J = ceil(i/U)` for uniform `U` in (0,1) reproduces exactly
  that, since `P(ceil(i/U) > j) = P(U < i/j) = i/j`. With 5000 matches the
  winner only changes about `H(5000) ≈ 9` times, so this replaces ~5000 draws
  with ~9.

  `ceil` is load-bearing: `floor` gives `i/(j+1)` and biases the pick toward
  later matches — by enough that a fairness test on which match gets chosen
  rejects it outright.

- **`cache`** — filter every match into a scratch buffer, then index it. Both
  halves happen in **one call**, because that is the operation; filtering and
  indexing separately are fragments nobody performs on their own. Correct even
  if the array changed since the last pick.


The scratch buffer is page-allocated once up front (`mmap` / `VirtualAlloc`) and
reused. Only the *allocation* is amortised — never the work: `cache` refilters
on every call, so it stays correct if the array changed.

Because everything inlines, these are best-case numbers — they assume the
compiler can see through to the generator at each call site. Routing the same
strategies through function pointers costs about 0.7 ns per call, invisible on
a scan but large on anything O(1).

## Why per-word speed is not the whole story

The benchmark asks each generator for **one word at a time**, which is not what
SHISHUA is fast at. `shishua_next` hands back one word from a 128-byte block and
refills when the block runs out, so every call pays a compare, a predicted
branch, a load and an index bump:

```
shishua bulk (prng_gen)        0.184 ns/word   0.0230 ns/byte
shishua per-word (buffered)    0.439 ns/word   0.0548 ns/byte
lcg4 per-word (buffered)       0.382 ns/word   0.0478 ns/byte
lcg per-word                   1.196 ns/word   0.1495 ns/byte
```

Filling a buffer directly, SHISHUA runs **2.4x faster** than the per-word
figure. That bookkeeping costs about 0.26 ns/word — more than the generation
itself — so over half of what `speed` reports is the wrapper rather than the
algorithm, and the SIMD advantage never gets to show.

`lcg4` pays the same bookkeeping over a 4-word buffer, but its work underneath
is four independent 64-bit multiplies that pipeline at roughly one per cycle.
Cheap arithmetic plus identical overhead lands it right beside SHISHUA. Compare
plain `lcg` at 1.196 ns to see what that instruction-level parallelism is worth:
identical arithmetic per word, 3x slower, because one serial chain of multiplies
cannot overlap.

So at this granularity you are largely measuring buffer bookkeeping. SHISHUA's
advantage only appears when randomness is consumed in bulk — filling an array,
generating a texture, seeding a particle system — which is also the regime
`select`'s `cache` strategy is closest to.

(Those are hot-loop numbers into a 128 KiB buffer that stays resident. Bulk
generation into memory large enough to miss cache would be bandwidth-bound
rather than 0.023 ns/byte.)

## Output format

One format, no flags. Rows are comma-separated **and** space-padded, so the same
bytes read as a table in a terminal and parse as CSV.

Every strategy reports one number for one operation, so no field is ever blank.
`breakeven` is derived rather than measured, so `plot.py` computes it from the
four rows of a cell instead of the CSV carrying it.

Every cell is also benchmarked with `thrash` set to `yes`: a 16 MiB pre-filled
buffer is streamed before each pick, flushing the caches, so the numbers
describe the pick with nothing warm.

`info` (`arch / cpu / compiler`) and `simd` are on every row so CSVs from
different machines and toolchains can be concatenated and still be told apart.

## Some findings

- **Cost tracks branch entropy, not workload.** Filtering a 10000-element array
  peaks at 47% match (~4250 ns) and drops to ~2390 ns at 100% — *doing more than
  twice the work*. At 100% the predicate is always true and perfectly predicted.
- **Raw generation speed does not predict application speed.** `lcg4` is the
  fastest generator in `speed` (~0.31 vs shishua's ~0.39 ns/number) but that
  lead does not survive contact with a real access pattern — chasing a pointer
  table into a struct array eats its instruction-level-parallelism advantage.
- **`reservoir_skip` is only worth it when draws are expensive.** It cuts ~5000
  draws to ~9, but shishua draws cost ~0.33 ns, so the branch it adds costs more
  than the draws it saves.
- **Filtering into a buffer beats reservoir sampling even for a single pick.**
  `cache` refilters the whole array and still comes out ahead of both reservoir
  variants at most sizes and match rates — the filter loop is branchless where
  the reservoir's is not. That is with the scratch buffer already allocated;
  allocating per pick would cost ~1.2 µs of `mmap`/`munmap`.
- **How you call the code changes what you measure.** With everything inlined,
  clang hoists `lcg`'s per-match RNG call out of its branch and issues it for
  every *element*, so `reservoir_every` is flat across match rate (~11500 ns at
  every rate). Route the same code through a function pointer and that stops:
  the cost becomes proportional to matches instead, tracking
  `1500 + 0.95·matches` almost exactly — 1484 ns at one match, 10941 ns at
  10000. Same algorithm, same generator, two different curves depending only on
  whether the compiler could see through the call.

## Files

```
Makefile       setup / build / run / clean
os.h           cycle counter, calibration, page-aligned allocation, runner info
rng.h          the four generators
speed.cpp      ns per random uint64
select.cpp     selection strategies
plot.py        CSVs -> plots/<arch>-<cpu>-<compiler>/*.png  (via gnuplot)
```

Timing reads the cycle counter directly — `cntvct_el0` on arm64 (macOS does not
expose the PMU counter to userspace), `rdtsc` on x86_64 — calibrated against the
monotonic clock rather than assumed. Each measurement is repeated inside a timed
sample to clear the counter's ~17 ns resolution, and sampled 50–100 times to get
a distribution: `ns_mean` includes interference, `ns_best` estimates the floor.

Generators are template parameters rather than function pointers on purpose.
An indirect call costs ~0.7 ns, twice a whole shishua step — enough to invert
the measured ranking between shishua and lcg. Templates monomorphise to the
same code a macro would emit, with real error messages and no backslashes.

## References

- Vitter, J. S. (1985). *Random Sampling with a Reservoir.* ACM Transactions on
  Mathematical Software 11(1), 37–57. — Algorithm R
- Li, K.-H. (1994). *Reservoir-Sampling Algorithms of Time Complexity
  O(n(1+log(N/n))).* ACM TOMS 20(4), 481–493. — Algorithm L, the skip idea
- Lemire, D. (2019). *Fast Random Integer Generation in an Interval.* ACM TOMACS
  29(1), 1–12. — the multiply-shift bounded draw
- Marsaglia, G. (2003). *Xorshift RNGs.* Journal of Statistical Software 8(14).
- Knuth, D. E. *The Art of Computer Programming, Vol. 2: Seminumerical
  Algorithms.* — LCG theory, and Algorithm R credited to Waterman
- [SHISHUA](https://github.com/espadrine/shishua) — Thaddée Tyl; the repo's
  README explains the design
