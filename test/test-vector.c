/* ----------------------------------------------------------------------------
 * Test suite for palloc vector extension and high-throughput vector engine
 * Verifies:
 *   1. 4GB virtual reservation does NOT consume 4GB or 5GB RAM upfront
 *   2. Physical committed RAM tracks the bump pointer within one commit grain (<= 2MB overhead)
 *   3. Reset releases physical RAM back to OS immediately
 *   4. SIMD 64-byte alignment
 *   5. Vectorized Pointer Bumping (AVX2 / Scalar)
 *   6. Dense 64-Bit Bitmap Vector Pool with Contiguous Multi-Slot Allocation
 *   7. Radix-Masked Page Binning & Single-CAS Splicing Batch Free
 *   8. Graceful NULL on out-of-capacity and overflow
 * --------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "palloc.h"
#include "palloc_vector.h"

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

static void test_vector_4gb_tight_memory(void) {
  printf("  Test: 4GB Vector Arena Tight Memory Model...\n");

  const size_t four_gb = 4ULL * 1024ULL * 1024ULL * 1024ULL;
  pa_vec_arena_t* arena = pa_vec_arena_create(four_gb, PA_VECTOR_ALIGNMENT_DEFAULT);
  assert(arena != NULL);

  pa_vec_arena_stats_t stats;
  pa_vec_arena_get_stats(arena, &stats);

  printf("    Initial stats: reserved = %zu MB, committed = %zu KB, used = %zu KB\n",
         stats.capacity_bytes / (1024 * 1024),
         stats.committed_bytes / 1024,
         stats.used_bytes / 1024);

  assert(stats.capacity_bytes >= four_gb);
  assert(stats.committed_bytes <= 2 * 1024 * 1024);
  assert(stats.used_bytes == 0);

  const size_t alloc_size = 64 * 1024 * 1024;
  float* vec = (float*)pa_vec_arena_alloc(arena, alloc_size);
  assert(vec != NULL);
  assert(((uintptr_t)vec % PA_VECTOR_ALIGNMENT_DEFAULT) == 0);

  vec[0] = 3.14159f;
  vec[(alloc_size / sizeof(float)) - 1] = 2.71828f;
  assert(vec[0] == 3.14159f);
  assert(vec[(alloc_size / sizeof(float)) - 1] == 2.71828f);

  pa_vec_arena_get_stats(arena, &stats);
  printf("    After 64MB alloc: committed = %zu MB, used = %zu MB\n",
         stats.committed_bytes / (1024 * 1024),
         stats.used_bytes / (1024 * 1024));

  assert(stats.used_bytes == alloc_size);
  assert(stats.committed_bytes >= alloc_size);
  assert(stats.committed_bytes <= alloc_size + 2 * 1024 * 1024);

  pa_vec_arena_reset(arena);
  pa_vec_arena_get_stats(arena, &stats);
  printf("    After reset: committed = %zu KB, used = %zu KB\n",
         stats.committed_bytes / 1024,
         stats.used_bytes / 1024);

  assert(stats.used_bytes == 0);
  assert(stats.committed_bytes == 0);

  float* vec2 = (float*)pa_vec_arena_alloc(arena, 1024 * sizeof(float));
  assert(vec2 != NULL);
  assert(vec2 == vec);
  vec2[0] = 42.0f;
  assert(vec2[0] == 42.0f);

  pa_vec_arena_destroy(arena);
  printf("  Test: 4GB Vector Arena Tight Memory Model OK!\n");
}

static void test_vector_simd_batch_bumping(void) {
  printf("  Test: SIMD Vectorized Pointer Bumping...\n");

  const size_t arena_cap = 64 * 1024 * 1024;
  pa_vec_arena_t* arena = pa_vec_arena_create(arena_cap, PA_VECTOR_ALIGNMENT_DEFAULT);
  assert(arena != NULL);

  const size_t batch_count = 512;
  const size_t vec_size = 2048; /* 512 floats */
  void* ptrs[512];

  size_t allocated = pa_vec_arena_alloc_batch(arena, ptrs, batch_count, vec_size);
  assert(allocated == batch_count);
  (void)allocated;

  /* Verify pointers are correctly spaced, non-null, and 64-byte aligned */
  for (size_t i = 0; i < batch_count; i++) {
    assert(ptrs[i] != NULL);
    assert(((uintptr_t)ptrs[i] % PA_VECTOR_ALIGNMENT_DEFAULT) == 0);
    if (i > 0) {
      assert((uintptr_t)ptrs[i] == (uintptr_t)ptrs[i - 1] + vec_size);
    }
    /* Write test data to verify accessibility */
    float* fv = (float*)ptrs[i];
    fv[0] = (float)i;
    fv[511] = (float)(i * 3);
  }

  for (size_t i = 0; i < batch_count; i++) {
    float* fv = (float*)ptrs[i];
    assert(fv[0] == (float)i);
    assert(fv[511] == (float)(i * 3));
    (void)fv;
  }

  pa_vec_arena_destroy(arena);
  printf("  Test: SIMD Vectorized Pointer Bumping OK!\n");
}

static void test_vector_bitmap_pool_dense_and_contiguous(void) {
  printf("  Test: Dense Bitmap Vector Pool & Contiguous Allocation...\n");

  const size_t slot_size = 256; /* 256 bytes */
  pa_vec_pool_t* pool = pa_vec_pool_create(slot_size, 64);
  assert(pool != NULL);

  /* Single slot allocation */
  void* single_objs[64];
  for (size_t i = 0; i < 64; i++) {
    single_objs[i] = pa_vec_pool_alloc(pool);
    assert(single_objs[i] != NULL);
    assert(((uintptr_t)single_objs[i] % 16) == 0);
    memset(single_objs[i], 0xCD, slot_size);
  }

  /* Free every other slot (32 slots) */
  for (size_t i = 0; i < 64; i += 2) {
    pa_vec_pool_free(pool, single_objs[i]);
  }

  /* Re-allocate: should pick up free slots without touching cold payload */
  for (size_t i = 0; i < 64; i += 2) {
    void* p = pa_vec_pool_alloc(pool);
    assert(p != NULL);
    memset(p, 0xEE, slot_size);
  }

  /* Test Contiguous Allocation: 4 slots */
  void* contig4 = pa_vec_pool_alloc_contiguous(pool, 4);
  assert(contig4 != NULL);
  for (size_t i = 0; i < 4; i++) {
    char* slot_i = (char*)contig4 + i * slot_size;
    memset(slot_i, (int)(0x10 + i), slot_size);
  }

  /* Test Contiguous Allocation: 8 slots */
  void* contig8 = pa_vec_pool_alloc_contiguous(pool, 8);
  assert(contig8 != NULL);
  for (size_t i = 0; i < 8; i++) {
    char* slot_i = (char*)contig8 + i * slot_size;
    memset(slot_i, (int)(0x20 + i), slot_size);
  }

  /* Free contiguous blocks */
  pa_vec_pool_free_contiguous(pool, contig4, 4);
  pa_vec_pool_free_contiguous(pool, contig8, 8);

  pa_vec_pool_destroy(pool);
  printf("  Test: Dense Bitmap Vector Pool & Contiguous Allocation OK!\n");
}

/* Cross-thread batch deallocation test structure */
typedef struct {
  void**  ptrs;
  size_t  count;
  bool    done;
} cross_thread_payload_t;

#if defined(_WIN32)
static unsigned __stdcall cross_thread_alloc_worker(void* arg) {
  cross_thread_payload_t* p = (cross_thread_payload_t*)arg;
  p->ptrs = pa_vector_batch_alloc(p->count, 512);
  p->done = true;
  return 0;
}
#else
static void* cross_thread_alloc_worker(void* arg) {
  cross_thread_payload_t* p = (cross_thread_payload_t*)arg;
  p->ptrs = pa_vector_batch_alloc(p->count, 512);
  p->done = true;
  return NULL;
}
#endif

static void test_vector_cross_thread_batch_free(void) {
  printf("  Test: Radix-Masked Page Binning & Single-CAS Batch Free...\n");

  const size_t batch_size = 256;
  cross_thread_payload_t payload;
  payload.ptrs = NULL;
  payload.count = batch_size;
  payload.done = false;

#if defined(_WIN32)
  uintptr_t th = _beginthreadex(NULL, 0, cross_thread_alloc_worker, &payload, 0, NULL);
  assert(th != 0);
  WaitForSingleObject((HANDLE)th, INFINITE);
  CloseHandle((HANDLE)th);
#else
  pthread_t th;
  int err = pthread_create(&th, NULL, cross_thread_alloc_worker, &payload);
  assert(err == 0);
  pthread_join(th, NULL);
#endif

  assert(payload.done);
  assert(payload.ptrs != NULL);

  /* Free all cross-thread allocated objects using single-CAS batch splicer */
  pa_vector_batch_free(payload.ptrs, batch_size);

  printf("  Test: Radix-Masked Page Binning & Single-CAS Batch Free OK!\n");
}

static void test_vector_edge_cases(void) {
  printf("  Test: Edge Cases and Overflows...\n");

  assert(pa_vector_alloc_floats(0) == NULL);
  assert(pa_vector_batch_alloc(0, 64) == NULL);
  assert(pa_vector_batch_alloc_floats(0, 100) == NULL);
  assert(pa_vector_batch_alloc_floats(100, 0) == NULL);

  assert(pa_vector_alloc_floats(SIZE_MAX / 2) == NULL);
  assert(pa_vector_batch_alloc_floats(SIZE_MAX / 2, sizeof(float)) == NULL);

  assert(pa_vec_arena_create(0, 64) == NULL);
  assert(pa_vec_arena_create(1024, 0) == NULL);
  assert(pa_vec_arena_create(1024, 63) == NULL);

  pa_vec_arena_t* arena = pa_vec_arena_create(1024 * 1024, 64);
  assert(arena != NULL);
  pa_vec_arena_set_max_size(arena, 128);
  void* p1 = pa_vec_arena_alloc(arena, 64);
  assert(p1 != NULL);
  void* p2 = pa_vec_arena_alloc(arena, 64);
  assert(p2 != NULL);
  void* p3 = pa_vec_arena_alloc(arena, 64);
  assert(p3 == NULL);
  (void)p1; (void)p2; (void)p3;
  pa_vec_arena_destroy(arena);

  printf("  Test: Edge Cases and Overflows OK!\n");
}

int main(void) {
  printf("Starting palloc Vector Test Suite (Full High-Performance Engine)...\n");

  test_vector_4gb_tight_memory();
  test_vector_simd_batch_bumping();
  test_vector_bitmap_pool_dense_and_contiguous();
  test_vector_cross_thread_batch_free();
  test_vector_edge_cases();

  printf("palloc Vector Test Suite PASSED!\n");
  return 0;
}
