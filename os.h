// os.h - platform layer for the benchmarks: cycle-counter timing and
// page-aligned allocation. Single header, no .c file: everything is
// static inline.
//
//   tick()        raw cycle counter, the rdtsc/cntvct read
//   resolution()  smallest step that counter can actually resolve, in ticks
//   calibrate()   ticks per nanosecond, measured not assumed
//   page_alloc()  page-aligned block straight from the OS
//   page_free()   release it
//
// Supports arm64 and x86_64 on macOS, Linux, and Windows.

#ifndef OS_H
#define OS_H

#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

// ------------------------------------------------ dead code elimination ----
#if defined(__clang__) || defined(__GNUC__)
  #define PretendToWrite(Variable) asm volatile ("" : "+r,x"(Variable))
  #define PretendToRead(Variable)  asm volatile ("" : : "r,x"(Variable))
  #define CompilerBarrier()        asm volatile ("" ::: "memory")
#else
  #define PretendToWrite(Variable) (void)(Variable)
  #define PretendToRead(Variable)  (void)(Variable)
  #define CompilerBarrier()
#pragma message("WARNING: Dead code macros are not implemented on this compiler.")
#endif
// ---------------------------------------------------------------- cache ----

// Enough words to evict L1 and L2 caches between each pick for "cold cache"
// benchmarks. Apple M2 L2: 16 MB, Intel i7 L2: 4 MB shared
const size_t EVICT_WORDS = 16 * 1024 * 1024 / sizeof(uint64_t);

// Apple M2: 128B, Intel i7: 64B. Pick smaller to touch all lines in both archs.
const size_t EVICT_CACHELINE_WORDS = 64 / sizeof(uint64_t);


static inline void evict_l1_l2(const uint64_t *thrash_buf) {
  for (size_t i = 0; i < EVICT_WORDS; i += EVICT_CACHELINE_WORDS) {
    uint64_t val = thrash_buf[i];
    PretendToRead(val);
  }
  CompilerBarrier();
}

// ---------------------------------------------------------------- timer ----

#if defined(__aarch64__)
// macOS does not expose the PMU cycle counter to userspace; cntvct_el0 is the
// architectural equivalent of rdtsc. isb stops the read drifting across the
// work being timed.
static inline uint64_t tick(void) {
  uint64_t t;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t)::"memory");
  return t;
}
#elif defined(__x86_64__)
static inline uint64_t tick(void) {
  __builtin_ia32_lfence();
  return __builtin_ia32_rdtsc();
}
#else
#error "os.h: no cycle counter for this architecture"
#endif

// Monotonic wall clock in nanoseconds, used only to calibrate the counter.
static inline uint64_t os_nanos(void) {
#if defined(_WIN32)
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (uint64_t)((c.QuadPart * 1000000000LL) / f.QuadPart);
#elif defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t resolution(void) {
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < 200000; i++) {
    uint64_t a = tick(), b = tick();
    if (b > a && b - a < best)
      best = b - a;
  }
  return best;
}

// Ticks per nanosecond. Measured against the monotonic clock rather than read
// from cntfrq_el0 / a TSC sysctl, so the same code is right on both arches.
static inline double calibrate(void) {
  uint64_t ns0 = os_nanos(), t0 = tick();
  while (os_nanos() - ns0 < 50000000ULL) {
  }
  uint64_t t1 = tick(), ns1 = os_nanos();
  return (double)(t1 - t0) / (double)(ns1 - ns0);
}

static void os_print_timer_spec(FILE*fp, double tpn) {
  fprintf(
      fp,
      "counter %.4f ticks/ns (%.2f MHz, resolution %.1f ns)\n",
      tpn,tpn*1000.0, resolution()/tpn);
}

// ------------------------------------------------------------- identity ----

// "arch / cpu / compiler", e.g. "arm64 / Apple M2 Max / clang". Stamped onto
// every result row so CSVs from different machines and toolchains can be
// concatenated and still be told apart. The separator is " / " because arch
// names carry no spaces, so a reader can recover the first field from a row.
// Resolved once into a static buffer.
static inline const char *os_info(void) {
  static char buf[200];
  if (buf[0])
    return buf;
  static char cpu[96];
  const char *arch;
#if defined(__aarch64__)
  arch = "arm64";
#elif defined(__x86_64__)
  arch = "x86_64";
#else
  arch = "unknown";
#endif
  const char *compiler;
#if defined(__clang__)
  compiler = "clang";
#elif defined(__GNUC__)
  compiler = "gcc";
#elif defined(_MSC_VER)
  compiler = "msvc";
#else
  compiler = "unknown";
#endif
#if defined(__APPLE__)
  size_t len = sizeof cpu;
  if (sysctlbyname("machdep.cpu.brand_string", cpu, &len, NULL, 0) != 0)
    cpu[0] = 0;
#elif defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      char *c = strchr(line, ':');
      if (c && strncmp(line, "model name", 10) == 0) {
        c += 2;
        c[strcspn(c, "\n")] = 0;
        snprintf(cpu, sizeof cpu, "%s", c);
        break;
      }
    }
    fclose(f);
  }
#elif defined(_WIN32)
  snprintf(cpu, sizeof cpu, "%s", "windows");
#endif
  if (!cpu[0])
    snprintf(cpu, sizeof cpu, "%s", "unknown");
  snprintf(buf, sizeof buf, "%s / %s / %s", arch, cpu, compiler);
  return buf;
}

// ----------------------------------------------------------- allocation ----

// A page-aligned block. `len` is the rounded-up size actually mapped, which
// page_free needs on POSIX.
typedef struct {
  void *p;
  size_t len;
} page_buf;

static inline size_t page_size(void) {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
#else
  return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

// Returns {NULL, 0} on failure.
static inline page_buf page_alloc(size_t bytes) {
  page_buf b = {NULL, 0};
  if (bytes == 0)
    return b;
  size_t pg = page_size();
  b.len = (bytes + pg - 1) & ~(pg - 1); // whole pages either way
#if defined(_WIN32)
  b.p = VirtualAlloc(NULL, b.len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *p =
      mmap(NULL, b.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  b.p = (p == MAP_FAILED) ? NULL : p;
#endif
  if (!b.p)
    b.len = 0;
  return b;
}

static inline void page_free(page_buf b) {
  if (!b.p)
    return;
#if defined(_WIN32)
  VirtualFree(b.p, 0, MEM_RELEASE);
#else
  munmap(b.p, b.len);
#endif
}

#endif // OS_H
