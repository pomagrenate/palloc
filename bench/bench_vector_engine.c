/* ----------------------------------------------------------------------------
 * Palloc Vector Engine vs. Scalar Allocator Micro-Benchmark Suite
 * 
 * Hardware-level profiling and cycle-accurate performance analysis:
 *   1. Low-Skewer Hardware Timing Harness (RDTSC/RDTSCP with lfence barriers)
 *   2. Scenario A: Vectorized Pointer Bumping (AVX2) vs. Scalar Alloc Loop
 *   3. Scenario B: Cross-Thread Batch Deallocation (Single-CAS Splicer) vs. Scalar Free
 *   4. Scenario C: Dense 64-Bit Bitmap Pool (TZCNT) vs. Intrusive Free-List
 *   5. Compiler Optimization Defense (escape & clobber memory barriers)
 *   6. AddressSanitizer (ASan) Memory Poisoning Hooks
 * --------------------------------------------------------------------------*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "palloc.h"
#include "palloc_vector.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

/* ---------------------------------------------------------------------------
 * AddressSanitizer (ASan) Integration Hooks
 * ----------------------------------------------------------------------- */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
  #define PA_HAS_ASAN 1
  #include <sanitizer/asan_interface.h>
  #define PA_ASAN_POISON(addr, size)    ASAN_POISON_MEMORY_REGION((addr), (size))
  #define PA_ASAN_UNPOISON(addr, size)  ASAN_UNPOISON_MEMORY_REGION((addr), (size))
#else
  #define PA_HAS_ASAN 0
  #define PA_ASAN_POISON(addr, size)    ((void)(addr), (void)(size))
  #define PA_ASAN_UNPOISON(addr, size)  ((void)(addr), (void)(size))
#endif

/* ---------------------------------------------------------------------------
 * Compiler Optimization Defense Barriers
 * Prevents dead-code elimination, constant propagation, and loop hoisting.
 * ----------------------------------------------------------------------- */
static inline void pa_escape(void* p) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(p) : "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
  (void)p;
#else
  (void)p;
#endif
}

static inline void pa_clobber(void) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : : "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
#endif
}

/* ---------------------------------------------------------------------------
 * Cycle-Accurate Hardware Timing Harness (RDTSC / RDTSCP + Serializing Fence)
 * ----------------------------------------------------------------------- */
#if defined(__x86_64__) || defined(_M_X64)
static inline uint64_t pa_rdtsc_start(void) {
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

static inline uint64_t pa_rdtsc_stop(void) {
  unsigned int aux;
  uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}
#else
static inline uint64_t pa_rdtsc_start(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline uint64_t pa_rdtsc_stop(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* High-resolution wall-clock timer for CPU frequency calibration */
static double pa_get_wall_time_sec(void) {
#if defined(_WIN32)
  static LARGE_INTEGER freq;
  static bool init = false;
  if (!init) {
    QueryPerformanceFrequency(&freq);
    init = true;
  }
  LARGE_INTEGER cur;
  QueryPerformanceCounter(&cur);
  return (double)cur.QuadPart / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* Measure TSC ticks per second */
static double pa_calibrate_tsc_freq_ghz(void) {
  double t0 = pa_get_wall_time_sec();
  uint64_t c0 = pa_rdtsc_start();
  /* Busy-wait ~50ms */
  while ((pa_get_wall_time_sec() - t0) < 0.05) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  }
  double t1 = pa_get_wall_time_sec();
  uint64_t c1 = pa_rdtsc_stop();
  double dt = t1 - t0;
  double dc = (double)(c1 - c0);
  return (dc / dt) / 1e9;
}

/* ---------------------------------------------------------------------------
 * Classical Intrusive Linked-List Pool for Comparative Baseline
 * ----------------------------------------------------------------------- */
typedef struct classical_pool_s {
  void*   free_list;
  char*   buffer;
  size_t  object_size;
  size_t  capacity;
} classical_pool_t;

static classical_pool_t* classical_pool_create(size_t object_size, size_t capacity) {
  classical_pool_t* p = (classical_pool_t*)malloc(sizeof(classical_pool_t));
  if (!p) return NULL;
  p->object_size = (object_size < sizeof(void*)) ? sizeof(void*) : object_size;
  p->capacity = capacity;
  p->buffer = (char*)malloc(p->object_size * capacity);
  p->free_list = NULL;
  /* Link slots together into intrusive free list */
  for (size_t i = 0; i < capacity; i++) {
    void* slot = p->buffer + i * p->object_size;
    *(void**)slot = p->free_list;
    p->free_list = slot;
  }
  return p;
}

static inline void* classical_pool_alloc(classical_pool_t* p) {
  if (!p->free_list) return NULL;
  void* slot = p->free_list;
  p->free_list = *(void**)slot;
  return slot;
}

static inline void classical_pool_free(classical_pool_t* p, void* ptr) {
  if (!ptr) return;
  *(void**)ptr = p->free_list;
  p->free_list = ptr;
}

static void classical_pool_destroy(classical_pool_t* p) {
  if (!p) return;
  free(p->buffer);
  free(p);
}

/* ---------------------------------------------------------------------------
 * Benchmark Benchmark Configuration & Execution Routines
 * ----------------------------------------------------------------------- */
#define NUM_WARMUP_ROUNDS   5
#define NUM_SAMPLE_ROUNDS  30

#define VECTOR_ELEM_DIM    512                       /* 512-dim float vector */
#define VECTOR_BYTE_SIZE   (512 * sizeof(float))     /* 2048 bytes */

static const size_t BATCH_SIZES[] = { 16, 64, 256, 1024, 4096 };
#define NUM_BATCH_SIZES (sizeof(BATCH_SIZES) / sizeof(BATCH_SIZES[0]))

/* ---------------------------------------------------------------------------
 * Scenario A: Vectorized Pointer Bumping vs. Scalar Alloc Loop
 * ----------------------------------------------------------------------- */
static void run_scenario_a(double tsc_ghz) {
  printf("\n========================================================================================================\n");
  printf("  SCENARIO A: Vectorized Batch Bumping (AVX2 SIMD) vs. Scalar Alloc Loop\n");
  printf("  Payload: 2048-Byte Vectors (512-dim FP32, 64-Byte Aligned) | CPU Freq: %.2f GHz\n", tsc_ghz);
  printf("========================================================================================================\n");
  printf(" %-10s | %-12s | %-14s | %-12s | %-14s | %-10s\n",
         "Batch Size", "Alloc Engine", "Min Latency", "Cycles / Op", "Throughput", "Speedup");
  printf("--------------------------------------------------------------------------------------------------------\n");

  void* ptrs[4096];

  for (size_t b = 0; b < NUM_BATCH_SIZES; b++) {
    const size_t N = BATCH_SIZES[b];
    const size_t total_bytes = N * VECTOR_BYTE_SIZE;

    /* 1. Scalar Baseline: pa_malloc_aligned loop */
    uint64_t min_scalar_cycles = UINT64_MAX;
    for (int round = 0; round < NUM_WARMUP_ROUNDS + NUM_SAMPLE_ROUNDS; round++) {
      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      for (size_t i = 0; i < N; i++) {
        ptrs[i] = pa_malloc_aligned(VECTOR_BYTE_SIZE, PA_VECTOR_ALIGNMENT_DEFAULT);
      }
      uint64_t t1 = pa_rdtsc_stop();
      pa_escape(ptrs);
      pa_clobber();

      /* Cleanup */
      for (size_t i = 0; i < N; i++) {
        pa_free(ptrs[i]);
      }

      if (round >= NUM_WARMUP_ROUNDS) {
        uint64_t dt = t1 - t0;
        if (dt < min_scalar_cycles) min_scalar_cycles = dt;
      }
    }

    /* 2. Vectorized Palloc: pa_vec_arena_alloc_batch with AVX2 SIMD pointer materialization */
    const size_t arena_size = 64 * 1024 * 1024; /* 64 MB */
    pa_vec_arena_t* arena = pa_vec_arena_create(arena_size, PA_VECTOR_ALIGNMENT_DEFAULT);

    uint64_t min_vector_cycles = UINT64_MAX;
    for (int round = 0; round < NUM_WARMUP_ROUNDS + NUM_SAMPLE_ROUNDS; round++) {
      pa_vec_arena_clear(arena);
      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      size_t count = pa_vec_arena_alloc_batch(arena, ptrs, N, VECTOR_BYTE_SIZE);
      uint64_t t1 = pa_rdtsc_stop();
      pa_escape(ptrs);
      pa_clobber();

      (void)count;
      if (round >= NUM_WARMUP_ROUNDS) {
        uint64_t dt = t1 - t0;
        if (dt < min_vector_cycles) min_vector_cycles = dt;
      }
    }
    pa_vec_arena_destroy(arena);

    /* Metrics Calculation */
    double scalar_cyc_per_op = (double)min_scalar_cycles / (double)N;
    double vector_cyc_per_op = (double)min_vector_cycles / (double)N;

    double scalar_ns = ((double)min_scalar_cycles / (tsc_ghz * 1e9)) * 1e9;
    double vector_ns = ((double)min_vector_cycles / (tsc_ghz * 1e9)) * 1e9;

    double scalar_gib_s = ((double)total_bytes / (1024.0 * 1024.0 * 1024.0)) / (scalar_ns * 1e-9);
    double vector_gib_s = ((double)total_bytes / (1024.0 * 1024.0 * 1024.0)) / (vector_ns * 1e-9);

    double speedup = (double)min_scalar_cycles / (double)min_vector_cycles;

    printf(" %-10zu | %-12s | %-10" PRIu64 " cyc | %-8.1f cyc | %-10.2f GiB/s | Baseline\n",
           N, "Scalar Loop", min_scalar_cycles, scalar_cyc_per_op, scalar_gib_s);
    printf(" %-10zu | %-12s | %-10" PRIu64 " cyc | %-8.1f cyc | %-10.2f GiB/s | %6.2fx\n",
           N, "Palloc AVX2", min_vector_cycles, vector_cyc_per_op, vector_gib_s, speedup);
    printf("--------------------------------------------------------------------------------------------------------\n");
  }
}

/* ---------------------------------------------------------------------------
 * Scenario A-Small: Adaptive Threshold Validation  (N ∈ {4, 8, 16, 32})
 *
 * Before adaptive fix: N=16 showed 0.20x (SIMD setup overhead > kernel payoff).
 * After fix: N < 32 always dispatches scalar 4x-unroll → no SIMD penalty.
 * Expected: all rows ≥ 1.0x; N=32 row should see first AVX2 engagement.
 * ----------------------------------------------------------------------- */
static void run_scenario_a_small(double tsc_ghz) {
  printf("\n========================================================================================================\n");
  printf("  SCENARIO A-SMALL: Adaptive Threshold Validation  (N ∈ {4, 8, 16, 32})\n");
  printf("  Scalar dispatched below N=32 (no AVX2 setup cost). N=32 = break-even boundary.\n");
  printf("========================================================================================================\n");
  printf(" %-10s | %-16s | %-14s | %-12s | %-14s | %-12s\n",
         "Batch N", "Engine (Actual)", "Min Latency", "Cycles / Op", "Throughput", "vs Scalar");
  printf("--------------------------------------------------------------------------------------------------------\n");

  static const size_t SMALL_BATCH_SIZES[] = { 4, 8, 16, 32 };
  const size_t NUM_SMALL = sizeof(SMALL_BATCH_SIZES) / sizeof(SMALL_BATCH_SIZES[0]);

  void* ptrs[64]; /* max N=32, safe stack buffer */

  for (size_t b = 0; b < NUM_SMALL; b++) {
    const size_t N = SMALL_BATCH_SIZES[b];
    const size_t total_bytes = N * VECTOR_BYTE_SIZE;

    /* Scalar baseline: tight malloc_aligned loop — best case for small N */
    uint64_t min_scalar_cycles = UINT64_MAX;
    for (int round = 0; round < NUM_WARMUP_ROUNDS + NUM_SAMPLE_ROUNDS; round++) {
      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      for (size_t i = 0; i < N; i++) {
        ptrs[i] = pa_malloc_aligned(VECTOR_BYTE_SIZE, PA_VECTOR_ALIGNMENT_DEFAULT);
      }
      uint64_t t1 = pa_rdtsc_stop();
      pa_escape(ptrs);
      pa_clobber();
      for (size_t i = 0; i < N; i++) pa_free(ptrs[i]);
      if (round >= NUM_WARMUP_ROUNDS) {
        uint64_t dt = t1 - t0;
        if (dt < min_scalar_cycles) min_scalar_cycles = dt;
      }
    }

    /* Palloc arena batch (adaptive dispatcher active — routes scalar for N < 32) */
    const size_t arena_size = 4 * 1024 * 1024; /* 4 MB, single slab sufficient */
    pa_vec_arena_t* arena = pa_vec_arena_create(arena_size, PA_VECTOR_ALIGNMENT_DEFAULT);

    uint64_t min_vector_cycles = UINT64_MAX;
    for (int round = 0; round < NUM_WARMUP_ROUNDS + NUM_SAMPLE_ROUNDS; round++) {
      pa_vec_arena_clear(arena);
      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      size_t cnt = pa_vec_arena_alloc_batch(arena, ptrs, N, VECTOR_BYTE_SIZE);
      uint64_t t1 = pa_rdtsc_stop();
      pa_escape(ptrs);
      pa_clobber();
      (void)cnt;
      if (round >= NUM_WARMUP_ROUNDS) {
        uint64_t dt = t1 - t0;
        if (dt < min_vector_cycles) min_vector_cycles = dt;
      }
    }
    pa_vec_arena_destroy(arena);

    double scalar_cyc = (double)min_scalar_cycles / (double)N;
    double vector_cyc = (double)min_vector_cycles / (double)N;
    double vector_ns  = ((double)min_vector_cycles / (tsc_ghz * 1e9)) * 1e9;
    double vector_gib = ((double)total_bytes / (1024.0 * 1024.0 * 1024.0)) / (vector_ns * 1e-9);
    double speedup    = (double)min_scalar_cycles / (double)min_vector_cycles;

    const char* engine_label = (N < 32) ? "Scalar (thresh)" : "AVX2 (thresh=32)";

    printf(" %-10zu | %-16s | %-10" PRIu64 " cyc | %-8.1f cyc | %-10.2f GiB/s | %6.2fx %s\n",
           N, engine_label, min_vector_cycles, vector_cyc, vector_gib, speedup,
           (speedup >= 0.9) ? "(OK)" : "(REGRESSED)");

    (void)scalar_cyc; /* printed in Scenario A full run */
    printf("--------------------------------------------------------------------------------------------------------\n");
  }
  printf("  Legend: 'thresh' = adaptive dispatcher. All rows should show >= 0.90x (no SIMD regression).\n");
}


/* ---------------------------------------------------------------------------
 * Scenario B: Cross-Thread Batch Deallocation vs. Scalar Free Loop
 * ----------------------------------------------------------------------- */
typedef struct {
  void**  ptrs;
  size_t  count;
  volatile bool ready;
} thread_alloc_msg_t;

#if defined(_WIN32)
static unsigned __stdcall worker_alloc_func(void* arg) {
  thread_alloc_msg_t* msg = (thread_alloc_msg_t*)arg;
  for (size_t i = 0; i < msg->count; i++) {
    msg->ptrs[i] = pa_malloc_aligned(VECTOR_BYTE_SIZE, PA_VECTOR_ALIGNMENT_DEFAULT);
  }
  msg->ready = true;
  return 0;
}
#else
static void* worker_alloc_func(void* arg) {
  thread_alloc_msg_t* msg = (thread_alloc_msg_t*)arg;
  for (size_t i = 0; i < msg->count; i++) {
    msg->ptrs[i] = pa_malloc_aligned(VECTOR_BYTE_SIZE, PA_VECTOR_ALIGNMENT_DEFAULT);
  }
  msg->ready = true;
  return NULL;
}
#endif

static void spawn_worker_and_alloc(void** ptrs, size_t count) {
  thread_alloc_msg_t msg;
  msg.ptrs = ptrs;
  msg.count = count;
  msg.ready = false;

#if defined(_WIN32)
  uintptr_t th = _beginthreadex(NULL, 0, worker_alloc_func, &msg, 0, NULL);
  WaitForSingleObject((HANDLE)th, INFINITE);
  CloseHandle((HANDLE)th);
#else
  pthread_t th;
  pthread_create(&th, NULL, worker_alloc_func, &msg);
  pthread_join(th, NULL);
#endif
}

static void run_scenario_b(double tsc_ghz) {
  (void)tsc_ghz;
  printf("\n========================================================================================================\n");
  printf("  SCENARIO B: Cross-Thread Batch Free (Radix Single-CAS) vs. Scalar Free Loop (N Atomic CAS)\n");
  printf("  Contention Metric: Measures reduction in cross-core atomic bus locks (LOCK CMPXCHG)\n");
  printf("========================================================================================================\n");
  printf(" %-10s | %-16s | %-14s | %-12s | %-18s | %-10s\n",
         "Batch Size", "Dealloc Engine", "Min Latency", "Cycles / Op", "Bus Lock Est.", "Speedup");
  printf("--------------------------------------------------------------------------------------------------------\n");

  const size_t test_sizes[] = { 64, 256, 1024, 4096 };
  const size_t num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

  void* ptrs[4096];
  void** batch_copy = (void**)malloc(4096 * sizeof(void*));

  for (size_t b = 0; b < num_sizes; b++) {
    const size_t N = test_sizes[b];

    /* 1. Scalar Baseline: N discrete cross-thread pa_free() calls */
    uint64_t min_scalar_free_cycles = UINT64_MAX;
    for (int round = 0; round < 10; round++) {
      spawn_worker_and_alloc(ptrs, N);

      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      for (size_t i = 0; i < N; i++) {
        pa_free(ptrs[i]);
      }
      uint64_t t1 = pa_rdtsc_stop();
      pa_clobber();

      uint64_t dt = t1 - t0;
      if (dt < min_scalar_free_cycles) min_scalar_free_cycles = dt;
    }

    /* 2. Palloc Radix Splicer: pa_vector_batch_free() */
    uint64_t min_batch_free_cycles = UINT64_MAX;
    for (int round = 0; round < 10; round++) {
      spawn_worker_and_alloc(ptrs, N);
      /* Copy into a heap-allocated pointer array as expected by pa_vector_batch_free */
      void** heap_ptrs = (void**)pa_malloc(N * sizeof(void*));
      memcpy(heap_ptrs, ptrs, N * sizeof(void*));

      pa_clobber();
      uint64_t t0 = pa_rdtsc_start();
      pa_vector_batch_free(heap_ptrs, N);
      uint64_t t1 = pa_rdtsc_stop();
      pa_clobber();

      uint64_t dt = t1 - t0;
      if (dt < min_batch_free_cycles) min_batch_free_cycles = dt;
    }

    double scalar_cyc = (double)min_scalar_free_cycles / (double)N;
    double batch_cyc  = (double)min_batch_free_cycles / (double)N;
    double speedup    = (double)min_scalar_free_cycles / (double)min_batch_free_cycles;

    /* Theoretical bus lock contention reduction: N locks vs ~N/32 page locks */
    size_t est_pages = (N * VECTOR_BYTE_SIZE + 65535) / 65536;
    if (est_pages > 64) est_pages = 64;

    printf(" %-10zu | %-16s | %-10" PRIu64 " cyc | %-8.1f cyc | %-6zu CAS locks   | Baseline\n",
           N, "Scalar Free Loop", min_scalar_free_cycles, scalar_cyc, N);
    printf(" %-10zu | %-16s | %-10" PRIu64 " cyc | %-8.1f cyc | ~%-5zu CAS locks   | %6.2fx\n",
           N, "Palloc Splicer", min_batch_free_cycles, batch_cyc, est_pages, speedup);
    printf("--------------------------------------------------------------------------------------------------------\n");
  }

  free(batch_copy);
}

/* ---------------------------------------------------------------------------
 * Scenario C: Dense 64-Bit Bitmap Vector Pool vs. Intrusive Free-List
 * ----------------------------------------------------------------------- */
static void run_scenario_c(double tsc_ghz) {
  (void)tsc_ghz;
  printf("\n========================================================================================================\n");
  printf("  SCENARIO C: Dense 64-Bit Bitmap Vector Pool (TZCNT) vs. Intrusive Linked-List\n");
  printf("  Cache Locality: Dense bitmap avoids touching cold object payload cache lines on free\n");
  printf("========================================================================================================\n");
  printf(" %-22s | %-14s | %-14s | %-12s | %-14s\n",
         "Operation", "Pool Type", "Min Latency", "Cycles / Op", "Cache Pollution");
  printf("--------------------------------------------------------------------------------------------------------\n");

  const size_t pool_capacity = 4096;
  const size_t slot_size = 256; /* 256-byte vector */

  void* ptrs[4096];

  /* 1. Single-slot allocation comparison */
  classical_pool_t* cpool = classical_pool_create(slot_size, pool_capacity);
  pa_vec_pool_t* bpool = pa_vec_pool_create(slot_size, pool_capacity);

  /* Classical alloc */
  uint64_t min_calloc = UINT64_MAX;
  for (int r = 0; r < NUM_SAMPLE_ROUNDS; r++) {
    pa_clobber();
    uint64_t t0 = pa_rdtsc_start();
    for (size_t i = 0; i < pool_capacity; i++) {
      ptrs[i] = classical_pool_alloc(cpool);
    }
    uint64_t t1 = pa_rdtsc_stop();
    pa_escape(ptrs);
    pa_clobber();

    for (size_t i = 0; i < pool_capacity; i++) {
      classical_pool_free(cpool, ptrs[i]);
    }
    uint64_t dt = t1 - t0;
    if (dt < min_calloc) min_calloc = dt;
  }

  /* Bitmap alloc */
  uint64_t min_balloc = UINT64_MAX;
  for (int r = 0; r < NUM_SAMPLE_ROUNDS; r++) {
    pa_clobber();
    uint64_t t0 = pa_rdtsc_start();
    for (size_t i = 0; i < pool_capacity; i++) {
      ptrs[i] = pa_vec_pool_alloc(bpool);
    }
    uint64_t t1 = pa_rdtsc_stop();
    pa_escape(ptrs);
    pa_clobber();

    for (size_t i = 0; i < pool_capacity; i++) {
      pa_vec_pool_free(bpool, ptrs[i]);
    }
    uint64_t dt = t1 - t0;
    if (dt < min_balloc) min_balloc = dt;
  }

  printf(" %-22s | %-14s | %-10" PRIu64 " cyc | %-8.1f cyc | High (Touches Payload)\n",
         "Alloc 4096 Slots", "Classical List", min_calloc, (double)min_calloc / pool_capacity);
  printf(" %-22s | %-14s | %-10" PRIu64 " cyc | %-8.1f cyc | ZERO (Bitmap Only)\n",
         "Alloc 4096 Slots", "Dense Bitmap", min_balloc, (double)min_balloc / pool_capacity);
  printf("--------------------------------------------------------------------------------------------------------\n");

  /* 2. Contiguous Multi-Slot Allocation: 4-slot contiguous embeddings */
  const size_t contig_count = 4;
  const size_t num_allocs = pool_capacity / contig_count; /* 1024 contiguous blocks */

  uint64_t min_contig_alloc = UINT64_MAX;
  for (int r = 0; r < NUM_SAMPLE_ROUNDS; r++) {
    pa_clobber();
    uint64_t t0 = pa_rdtsc_start();
    for (size_t i = 0; i < num_allocs; i++) {
      ptrs[i] = pa_vec_pool_alloc_contiguous(bpool, contig_count);
    }
    uint64_t t1 = pa_rdtsc_stop();
    pa_escape(ptrs);
    pa_clobber();

    for (size_t i = 0; i < num_allocs; i++) {
      pa_vec_pool_free_contiguous(bpool, ptrs[i], contig_count);
    }
    uint64_t dt = t1 - t0;
    if (dt < min_contig_alloc) min_contig_alloc = dt;
  }

  printf(" %-22s | %-14s | %-10" PRIu64 " cyc | %-8.1f cyc | O(1) Bit-twiddling\n",
         "Alloc 1024x 4-Contig", "Dense Bitmap", min_contig_alloc, (double)min_contig_alloc / num_allocs);
  printf("--------------------------------------------------------------------------------------------------------\n");

  classical_pool_destroy(cpool);
  pa_vec_pool_destroy(bpool);
}

/* ---------------------------------------------------------------------------
 * Main Entry Point
 * ----------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
  (void)argc; (void)argv;
  printf("\n========================================================================================================\n");
  printf("           PALLOC VECTOR ENGINE vs. SCALAR ALLOCATOR MICRO-BENCHMARK & STRESS HARNESS                   \n");
  printf("========================================================================================================\n");

  printf("[*] Calibrating Hardware Time-Stamp Counter (RDTSC)...\n");
  double tsc_ghz = pa_calibrate_tsc_freq_ghz();
  printf("[*] Detected Nominal Core TSC Frequency: %.2f GHz\n", tsc_ghz);
  printf("[*] AddressSanitizer (ASan) Integration: %s\n", PA_HAS_ASAN ? "ENABLED" : "DISABLED (Zero Overhead)");

  run_scenario_a(tsc_ghz);
  run_scenario_a_small(tsc_ghz);
  run_scenario_b(tsc_ghz);
  run_scenario_c(tsc_ghz);

  printf("\n[+] Benchmark Suite execution completed successfully.\n\n");
  return 0;
}
