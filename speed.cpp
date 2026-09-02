// Speed benchmark for SHISHUA against a few classic rng generators.
// Timer and generators live in os.h / rng.h.

#include <cstdint>
#include <cstdio>

#include "os.h"
#include "rng.h"

constexpr int SAMPLES = 100;                // timed samples per batch size
constexpr int PICKS_PER_SAMPLE = 100000; // numbers generated per sample

constexpr int sizes[] = {1, 10, 100, 1000, 10000, 100000};

#define ROW_FMT "%-48s, %-7s, %-9s, %6s, %8s, %8s\n"

static void csv_header() {
  printf(ROW_FMT, "info", "simd", "rng", "batch", "ns_mean", "ns_best");
}

template <typename Rng> static void speed(const char *name, double tpn) {
  Rng r;
  fixed_seed(&r);

  for (int size : sizes) {
    uint64_t total = 0, best = UINT64_MAX;
    for (int it = 0; it < SAMPLES; it++) {
      uint64_t t0 = tick();
      for (int rep = 0; rep < PICKS_PER_SAMPLE; rep++) {
        uint64_t randval = next(&r);
        PretendToRead(randval);
      }
      uint64_t elapsed = tick() - t0;
      total += elapsed;
      if (elapsed < best)
        best = elapsed;
    }

    char size_s[24], mean_s[32], best_s[32];
    snprintf(size_s, sizeof size_s, "%d", size);
    snprintf(mean_s, sizeof mean_s, "%.4f", double(total) / SAMPLES / PICKS_PER_SAMPLE / tpn);
    snprintf(best_s, sizeof best_s, "%.4f", double(best) / PICKS_PER_SAMPLE / tpn);
    printf(ROW_FMT, os_info(), simd(&r), name, size_s, mean_s, best_s);
  }
}

int main() {
  double tpn = calibrate();

  os_print_timer_spec(stderr, tpn);
  fprintf(stderr, "%d samples per strategy, %d picks per sample\n", SAMPLES, PICKS_PER_SAMPLE);

  csv_header();
  speed<shishua_rng>("shishua", tpn);
  speed<lcg_rng>("lcg", tpn);
  speed<lcg4_rng>("lcg4", tpn);
  speed<xorshift_rng>("xorshift", tpn);
  return 0;
}
