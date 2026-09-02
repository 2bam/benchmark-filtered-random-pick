// The generators under test, shared by speed.cpp (ns per random uint64) and
// select.cpp (random selection strategies). Platform bits live in os.h.
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

#include "shishua/shishua.h"

// ----------------------------------------------------------- generators ----

// Each generator is a state struct plus seed/next overloads below, so every
// call site just writes `seed(r)` / `next(r)`. Everything is inline on
// purpose: a function-pointer indirection would cost more than the cheapest
// generator's entire step.

// SHISHUA. prng_gen only writes whole 128-byte blocks, so a single random
// number comes out of a block refilled on demand. This is what calling code
// actually does, and it makes the "1 number" case mean something.
#define BLOCK 128
#define BLOCK_N (BLOCK / 8)

typedef struct {
  prng_state s;
  uint64_t buf[BLOCK_N];
  size_t pos;
} shishua_rng;

static inline void fixed_seed(shishua_rng *r) {
  uint64_t seed[4] = {1, 2, 3, 4};
  prng_init(&r->s, seed);
  r->pos = BLOCK_N;  // force a refill on first use
}

static inline uint64_t next(shishua_rng *r) {
  if (r->pos == BLOCK_N) {
    prng_gen(&r->s, (uint8_t *)r->buf, BLOCK);
    r->pos = 0;
  }
  return r->buf[r->pos++];
}

// Linear congruential generator: state = state * a + c. Knuth's MMIX
// constants. Returning the raw state means the low bits are barely random at
// all (bit 0 just alternates) - real uses return the high half.
typedef struct { uint64_t s; } lcg_rng;
static inline void fixed_seed(lcg_rng *r) { r->s = 1; }
static inline uint64_t next(lcg_rng *r) { return r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL; }

// Four independent LCG streams, drained round-robin. Same arithmetic per
// number as `lcg`, but the four multiplies are independent so they pipeline
// instead of serialising. This is the throughput number; `lcg` is the latency
// number.
typedef struct { uint64_t s[4]; size_t pos; } lcg4_rng;
static inline void fixed_seed(lcg4_rng *r) {
  // The seeds must not equal the increments. With seed_k == c_k == 2k+1 the
  // closed form collapses to s_k[t] = (2k+1) * s_0[t], so the four "streams"
  // are exact odd multiples of one another rather than independent - which is
  // what made this fail the selection-uniformity and d6-pairs checks. Spread
  // four independent seeds from one value with SplitMix64.
  uint64_t z = 0;
  for (int k = 0; k < 4; k++) {
    uint64_t x = (z += 0x9E3779B97F4A7C15ULL);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    r->s[k] = x ^ (x >> 31);
  }
  r->pos = 4;
}
static inline uint64_t next(lcg4_rng *r) {
  if (r->pos == 4) {
    for (int k = 0; k < 4; k++) r->s[k] = r->s[k] * 6364136223846793005ULL + (uint64_t)(2 * k + 1);
    r->pos = 0;
  }
  return r->s[r->pos++];
}

// xorshift64: a linear feedback shift register over GF(2) that shifts whole
// words rather than single bits. The bit-serial form needs 64 steps for a
// 64-bit number (~44 ns); this does it in three shifts and three xors.
typedef struct { uint64_t s; } xorshift_rng;
static inline void fixed_seed(xorshift_rng *r) { r->s = 0x123456789ABCDEFULL; }
static inline uint64_t next(xorshift_rng *r) {
  uint64_t x = r->s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return r->s = x;
}

// Which code path the generator actually compiled to, stamped onto result rows.
// Only shishua vectorises; the other three are scalar by construction.
static inline const char *simd(shishua_rng *) {
  switch (SHISHUA_TARGET) {
    case SHISHUA_TARGET_AVX2: return "avx2";
    case SHISHUA_TARGET_SSE2: return "sse2";
    case SHISHUA_TARGET_NEON: return "neon";
    default:                  return "scalar";
  }
}
static inline const char *simd(lcg_rng *)      { return "scalar"; }
static inline const char *simd(lcg4_rng *)     { return "scalar"; }
static inline const char *simd(xorshift_rng *) { return "scalar"; }

// Unbiased-enough bounded draw in [0, bound), via Lemire's multiply-shift. Uses
// the high bits of the word, which is where every generator here is strongest -
// `x % bound` would take the low bits, where an LCG's short period makes
// consecutive values correlate - see the README.
template <typename Rng>
static inline uint64_t below(Rng *r, uint64_t bound) {
  return (uint64_t)(((unsigned __int128)next(r) * bound) >> 64);
}

#endif  // RNG_H
