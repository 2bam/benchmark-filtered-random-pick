// Three ways to answer one question: give me one random element that matches.
//
//   reservoir_every  one pass, no scratch, draws for every match (Algorithm R)
//   reservoir_skip   one pass, no scratch, draws per replacement (Algorithm L)
//   cache            filter matches into scratch, then index it
//
// Both the random generator and the pick strategy are template parameters, so
// every combination is a separate fully-inlined instantiation and the calls at
// main() are the experiment matrix, there is no dispatch table.
//
// stdout is the dataset: one row per (generator, strategy, size, match rate).
// Banners go to stderr, so `./select > select.csv` leaves a clean file behind.
//
// PICKS_PER_SAMPLE to minimize timer precision errors
// SAMPLES          take samples for mean/best distribution

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "os.h"
#include "rng.h"

// MEMO: PICKS_PER_SAMPLE is lower than the highest size (100k) otherwise
// flushing the
//       cache each rep takes forever.
const int SAMPLES = 100; // timed samples per cell

// Elements picked per sample

const int PICKS_PER_SAMPLE = 100;
// const size_t PICKS_PER_SAMPLE = 200000;  // elements visited per sample

// const size_t sizes[] = {10, 100, 1000, 10000, 100000};
// const size_t sizes[] = {10, 100, 1000}; //, 10000, 100000};
const size_t sizes[] = {10, 100, 1000, 10000, 100000};
const size_t NSIZES = sizeof(sizes) / sizeof(sizes[0]);

struct Rate {
  const char *label;
  int pct;
};
const Rate rates[] = {
    {"single", 0}, {"1%", 1},   {"10%", 10},   {"22%", 22},
    {"47%", 47},   {"82%", 82}, {"100%", 100},
};

// A record rather than a bare integer: the predicate reads one field out of a
// 32-byte struct, so the scan strides memory the way real code does instead of
// walking a densely packed scalar array.
struct elem {
  int64_t w, x, y, z;
};

// ------------------------------------------------------------ predicate ----

// Matches are the entries below the fence. fill_with_quota scatters them, so
// the predicate is a real comparison rather than something the branch predictor
// can learn.
const int64_t FENCE = 200;
// Note: Could be another template argument that gets inlined too
static inline bool matches(const elem &e) { return e.w < FENCE; }

// ------------------------------------------------------------ algorithms ----

// Each strategy is a tag type holding nothing but its pick.
struct ReservoirEvery {
  // Draws for EVERY match: keep the i-th one with probability 1/i (Algorithm
  // R). No scratch, and never needs the match count up front.
  template <typename Rng>
  static const elem *pick(const elem *items, size_t size, Rng &rng,
                          const elem **) {
    const elem *chosen = 0;
    uint64_t seen = 0;
    for (size_t i = 0; i < size; i++) {
      if (matches(items[i])) {
        seen++;
        if (below(&rng, seen) == 0)
          chosen = &items[i];
      }
    }
    return chosen;
  }
};

struct ReservoirSkip {
  // SKIPS to the next replacement instead of drawing per match (Algorithm L).
  // If match i is the winner, the next to displace it is ceil(i/U), since
  // P(ceil(i/U) > j) = P(U < i/j) = i/j, the survival probability R gives.
  template <typename Rng>
  static const elem *pick(const elem *items, size_t size, Rng &rng,
                          const elem **) {
    const elem *chosen = 0;
    uint64_t seen = 0, target = 1;
    for (size_t i = 0; i < size; i++) {
      if (matches(items[i])) {
        if (++seen == target) {
          chosen = &items[i];
          double u =
              double((next(&rng) >> 11) | 1) / 9007199254740992.0; // 2^53
          double next_target = std::ceil(double(seen) / u); // ceil, not floor
          target = (next_target >= double(UINT64_MAX)) ? UINT64_MAX
                                                       : uint64_t(next_target);
          if (target <= seen)
            target = seen + 1;
        }
      }
    }
    return chosen;
  }
};

struct FilterCache {
  // Filter, then index - both halves in one call, which is the whole
  // operation. Correct even if the array changed since the last pick.
  template <typename Rng>
  static const elem *pick(const elem *items, size_t size, Rng &rng,
                          const elem **scratch) {
    size_t count = 0;
    for (size_t i = 0; i < size; i++)
      if (matches(items[i]))
        scratch[count++] = &items[i];
    return scratch[below(&rng, count)];
  }
};

// ------------------------------------------------------------- fixtures ----

// Fills container[0..size) so exactly `quota` entries fall below `fence` and
// the rest above, then shuffles so the matches are scattered and unpredictable.
// Uses shishua for better distribution.
static void fill_with_quota(elem *container, size_t size, size_t quota,
                            uint64_t fence, shishua_rng &rng) {

  for (size_t i = 0; i < size; i++) {
    container[i].w = i < quota ? next(&rng) % fence : fence + next(&rng) % 1000;
    container[i].x = next(&rng);
    container[i].y = next(&rng);
    container[i].z = next(&rng);
  }
  for (size_t i = size; i > 1; i--) {
    size_t j = size_t(below(&rng, i));
    elem swap = container[i - 1];
    container[i - 1] = container[j];
    container[j] = swap;
  }
}

// ------------------------------------------------------------ benchmark ----

// Rows are comma-separated and padded so they read as a table in a terminal.
// The padding lands inside the fields; the reader strips it.
#define ROW_FMT                                                                \
  "%-48s, %-7s, %-9s, %-16s, %6s, %-7s, %-7s, %7s, %9s, %7s, %10s, %10s\n"

static void csv_header() {
  printf(ROW_FMT, "info", "simd", "rng", "algo", "n", "rate", "thrash",
         "matches", "match_pct", "samples", "ns_mean", "ns_best");
}

static void csv_row(const char *simd_name, const char *rng_name,
                    const char *algo, bool thrash, size_t size,
                    const char *rate_label, size_t match_count, double ns_mean,
                    double ns_best) {
  char size_s[24], matches_s[24], pct_s[24], samples_s[24], ns_mean_s[32],
      ns_best_s[32];
  snprintf(size_s, sizeof size_s, "%zu", size);
  snprintf(matches_s, sizeof matches_s, "%zu", match_count);
  snprintf(pct_s, sizeof pct_s, "%.1f",
           100.0 * double(match_count) / double(size));
  snprintf(samples_s, sizeof samples_s, "%d", SAMPLES);
  snprintf(ns_mean_s, sizeof ns_mean_s, "%.2f", ns_mean);
  snprintf(ns_best_s, sizeof ns_best_s, "%.2f", ns_best);
  printf(ROW_FMT, os_info(), simd_name, rng_name, algo, size_s, rate_label,
         thrash ? "yes" : "no", matches_s, pct_s, samples_s, ns_mean_s,
         ns_best_s);
  fflush(stdout);
}

// One generator x one strategy, swept over every size and match rate. The inner
// `reps` loop exists because one call can be shorter than the counter's ~17 ns
// resolution; the outer loop exists to get a distribution, so ns_best is
// available alongside the mean.
//
// With `thrash`, every pick is preceded by cache eviction. The flush is
// untimed: only the pick is measured, and the count of timed picks (ITERATIONS
// x reps) matches the non-thrash mode so ns_best is comparable.
template <typename Rng, typename Algo>
static void bench(const char *rng_name, const char *algo, double tpn,
                  elem *container, const elem **scratch,
                  const uint64_t *thrash) {
  Rng rng;
  fixed_seed(&rng);
  shishua_rng fixture_rng;

  for (size_t size : sizes) {
    for (const Rate &rate : rates) {
      size_t quota = std::min(size, (size * size_t(rate.pct)) / 100 + 1);
      // MEMO: fixed seed should produce same shuffle for same params
      fixed_seed(&fixture_rng);
      fill_with_quota(container, size, quota, FENCE, fixture_rng);

      uint64_t total = 0, best = UINT64_MAX;

      // MEMO: Reps here are about minimizing timer resolution error, can't do
      //       without them. Samples are for mean/best distribution.
      if (thrash) {
        // uint64_t evict_total = 0, evict_best = UINT64_MAX;
        // for (int sample = 0; sample < SAMPLES; sample++) {
        //   uint64_t evict_start = tick();
        //   for (size_t rep = 0; rep < PICKS_PER_SAMPLE; rep++) {
        //     evict_l1_l2(thrash + (rep & 1) * EVICT_WORDS);
        //   }
        //   uint64_t evict_elapsed = tick() - evict_start;
        //   evict_total += evict_elapsed;
        //   if (evict_elapsed < evict_best)
        //     evict_best = evict_elapsed;
        // }

        for (int sample = 0; sample < SAMPLES; sample++) {
          // MEMO: Measuring evict times inside the rep aint great as its
          //       resolution will distort the pick's total, but measuring
          //       eviction separately and subtracting after
          //       T(evict+pick)-T(evict_only) was too jumpy and erratic even
          //       swapping thrash bufs, better a noisy but actual measurement
          //       of what really happened during eviction.
          uint64_t start_evict_pick = tick();

          uint64_t elapsed_evict_only = 0;
          for (size_t rep = 0; rep < PICKS_PER_SAMPLE; rep++) {
            uint64_t start_evict_only = tick();
            evict_l1_l2(thrash + (rep & 1) * EVICT_WORDS);
            elapsed_evict_only += tick() - start_evict_only;

            uint64_t w =
                Algo::template pick<Rng>(container, size, rng, scratch)->w;
            PretendToRead(w);
            CompilerBarrier();
          }
          uint64_t elapsed_evict_pick = tick() - start_evict_pick;
          // uint64_t elapsed = elapsed_evict_pick;
          uint64_t elapsed = (elapsed_evict_pick > elapsed_evict_only)
                                 ? (elapsed_evict_pick - elapsed_evict_only)
                                 : 0;
          total += elapsed;
          if (elapsed < best)
            best = elapsed;
        }
        // total = (uint64_t)std::max(0LL, (int64_t)total-
        // (int64_t)evict_total); best = (uint64_t)std::max(0LL, (int64_t)best-
        // (int64_t)evict_best);
      } else {
        for (int sample = 0; sample < SAMPLES; sample++) {
          // MEMO: these reps are about minimizing timer precision error
          uint64_t start = tick();
          for (size_t rep = 0; rep < PICKS_PER_SAMPLE; rep++) {
            uint64_t w =
                Algo::template pick<Rng>(container, size, rng, scratch)->w;
            PretendToRead(w);
          }
          uint64_t elapsed = tick() - start;
          total += elapsed;
          if (elapsed < best)
            best = elapsed;
        }
      }

      // double emean_ns = !thrash?0.0:
      //     double(etotal) / double(SAMPLES) / double(PICKS_PER_SAMPLE) / tpn;
      // double ebest_ns = !thrash?0.0:double(ebest) / double(PICKS_PER_SAMPLE)
      // / tpn;
      double emean_ns = 0, ebest_ns = 0;

      double mean_ns = -emean_ns + double(total) / double(SAMPLES) /
                                       double(PICKS_PER_SAMPLE) / tpn;
      double best_ns =
          -ebest_ns + double(best) / double(PICKS_PER_SAMPLE) / tpn;
      if (mean_ns < 0)
        mean_ns = -1;
      if (best_ns < 0)
        best_ns = -1;
      csv_row(simd(&rng), rng_name, algo, thrash != nullptr, size, rate.label,
              quota, mean_ns, best_ns);
    }
  }
}

template <typename Rng>
static void bench_all(const char *name, double tpn, elem *container,
                      const elem **scratch, const uint64_t *thrash) {
  bench<Rng, ReservoirEvery>(name, "reservoir every", tpn, container, scratch,
                             thrash);
  bench<Rng, ReservoirSkip>(name, "reservoir skip", tpn, container, scratch,
                            thrash);
  bench<Rng, FilterCache>(name, "filter cache", tpn, container, scratch,
                          thrash);
}

int main() {
  page_buf container_buf = page_alloc(sizes[NSIZES - 1] * sizeof(elem));
  page_buf scratch_buf = page_alloc(sizes[NSIZES - 1] * sizeof(const elem *));
  page_buf thrash_buf = page_alloc(2 * EVICT_WORDS * sizeof(uint64_t));
  if (!container_buf.p || !scratch_buf.p || !thrash_buf.p)
    return 1;

  elem *container = (elem *)container_buf.p;
  const elem **scratch = (const elem **)scratch_buf.p;
  double tpn = calibrate();

  // Prefill thrash buffer so pages are _really_ allocated and different
  uint64_t *thrash = (uint64_t *)thrash_buf.p;
  for (size_t i = 0; i < 2 * EVICT_WORDS; i++)
    thrash[i] = i;

  os_print_timer_spec(stderr, tpn);
  fprintf(stderr, "%d samples per strategy, %d picks per sample\n", SAMPLES,
          PICKS_PER_SAMPLE);

  size_t total_picks =
      NSIZES * (sizeof(rates) / sizeof(rates[0])) * SAMPLES * PICKS_PER_SAMPLE;
  fprintf(stderr,
          "Cold benchmark: %zu sizes * %zu rates * %d samples * "
          "%d picks/sample = %zu picks total\n",
          NSIZES, sizeof(rates) / sizeof(rates[0]), SAMPLES, PICKS_PER_SAMPLE,
          total_picks);

  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  fprintf(stderr, "Current Timer Frequency: %llu Hz (%.2f MHz)\n", freq,
          freq / 1000000.0);

  csv_header();
  // warm cache

  bench_all<shishua_rng>("shishua", tpn, container, scratch, nullptr);
  bench_all<lcg_rng>("lcg", tpn, container, scratch, nullptr);
  bench_all<lcg4_rng>("lcg4", tpn, container, scratch, nullptr);
  bench_all<xorshift_rng>("xorshift", tpn, container, scratch, nullptr);

  // cold cache
  bench_all<shishua_rng>("shishua", tpn, container, scratch, thrash);
  bench_all<lcg_rng>("lcg", tpn, container, scratch, thrash);
  bench_all<lcg4_rng>("lcg4", tpn, container, scratch, thrash);
  bench_all<xorshift_rng>("xorshift", tpn, container, scratch, thrash);

  page_free(container_buf);
  page_free(scratch_buf);
  page_free(thrash_buf);
  return 0;
}
