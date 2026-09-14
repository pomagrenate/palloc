/* ----------------------------------------------------------------------------
 * Vector-oriented allocator extension. Built when PA_VECTOR=ON.
 * Uses only public pa_* APIs (pa_heap_malloc, pa_heap_malloc_aligned, pa_free)
 * and internal OS primitives (_pa_prim_alloc, _pa_prim_commit, _pa_prim_free).
 *
 * High-Throughput Features:
 *   1. Vectorized Pointer Bumping (AVX2 SIMD materialized pointer streams)
 *   2. Radix-Masked Page Binning & Single-CAS Splicing for batch cross-thread frees
 *   3. Dense 64-Bit Word Bitmask Tracking for fixed-size vector pools (zero cache pollution)
 * --------------------------------------------------------------------------*/
#include "palloc_vector.h"
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/prim.h"

#if defined(PA_VECTOR) && PA_VECTOR

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

/* ---------------------------------------------------------------------------
 * Hardware Bit-Twiddling Intrinsics
 * ----------------------------------------------------------------------- */

static inline int pa_ctz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_ctzll(x);
#elif defined(_MSC_VER)
  unsigned long idx;
  _BitScanForward64(&idx, x);
  return (int)idx;
#else
  int n = 0;
  if ((x & 0xFFFFFFFF) == 0) { n += 32; x >>= 32; }
  if ((x & 0x0000FFFF) == 0) { n += 16; x >>= 16; }
  if ((x & 0x000000FF) == 0) { n += 8;  x >>= 8;  }
  if ((x & 0x0000000F) == 0) { n += 4;  x >>= 4;  }
  if ((x & 0x00000003) == 0) { n += 2;  x >>= 2;  }
  if ((x & 0x00000001) == 0) { n += 1; }
  return n;
#endif
}

static inline int pa_popcnt64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_popcountll(x);
#elif defined(_MSC_VER)
  return (int)__popcnt64(x);
#else
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
  return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

static inline bool pa_cpu_has_avx2(void) {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

#define PA_VEC_ALIGN_UP(sz, a)  (((sz) + (a) - 1) & ~((size_t)(a) - 1))

/* Commit slab: 2 MiB - one large-page boundary. */
#define PA_VEC_COMMIT_GRAIN  (2u * 1024u * 1024u)

/* ---------------------------------------------------------------------------
 * pa_vec_arena - single OS reservation + demand-commit bump allocator
 * ----------------------------------------------------------------------- */

struct pa_vec_arena_s {
  pa_heap_t*  heap;           /* for the arena struct itself */
  void*       os_block;       /* raw OS reservation start */
  size_t      os_block_size;  /* total reservation (page-aligned) */
  char*       base;           /* aligned payload start within os_block */
  size_t      alignment;      /* user vector alignment (power of 2) */
  size_t      capacity;       /* usable bytes from base */
  size_t      used;           /* bump pointer offset from base */
  size_t      committed;      /* bytes committed from base (grain-aligned) */
  size_t      commit_grain;   /* slab size for lazy commit (page-aligned) */
  size_t      max_size;       /* soft cap: 0 = unlimited */
  bool        use_os_reserve; /* true = virtual reserve; false = fully committed fallback */
};

/* ---------------------------------------------------------------------------
 * Arena creation
 * ----------------------------------------------------------------------- */

pa_vec_arena_t* pa_vec_arena_create(size_t initial_size, size_t alignment) pa_attr_noexcept {
  return pa_vec_arena_create_ex(initial_size, alignment, pa_heap_get_default());
}

pa_vec_arena_t* pa_vec_arena_create_ex(size_t initial_size, size_t alignment, pa_heap_t* heap) pa_attr_noexcept {
  if (heap == NULL || initial_size == 0 || alignment == 0) return NULL;
  if ((alignment & (alignment - 1)) != 0) return NULL; /* must be power of two */

  const size_t page_size = _pa_os_page_size();

  if (initial_size > SIZE_MAX - page_size) return NULL;
  const size_t capacity = PA_VEC_ALIGN_UP(initial_size, page_size);

  size_t extra = 0;
  if (alignment > page_size) {
    if (alignment > SIZE_MAX - capacity) return NULL;
    extra = alignment;
  }
  if (extra > SIZE_MAX - capacity) return NULL;
  const size_t os_block_size = capacity + extra;

  pa_vec_arena_t* arena = (pa_vec_arena_t*)pa_heap_malloc(heap, sizeof(pa_vec_arena_t));
  if (!arena) return NULL;

  size_t commit_grain = PA_VEC_COMMIT_GRAIN;
  if (commit_grain < page_size) commit_grain = page_size;
  commit_grain = PA_VEC_ALIGN_UP(commit_grain, page_size);

  const bool can_reserve = _pa_os_has_virtual_reserve();
  bool is_large = false;
  bool is_zero  = false;
  void* os_block = NULL;

  const size_t try_align = (alignment > page_size) ? alignment : page_size;
  int err;

  if (can_reserve) {
    err = _pa_prim_alloc(NULL, os_block_size, try_align,
                         /*commit=*/false, /*allow_large=*/false,
                         &is_large, &is_zero, &os_block);
  } else {
    err = _pa_prim_alloc(NULL, os_block_size, try_align,
                         /*commit=*/true, /*allow_large=*/false,
                         &is_large, &is_zero, &os_block);
  }

  if (err != 0 || os_block == NULL) {
    pa_free(arena);
    return NULL;
  }

  char* base = (char*)os_block;
  if (extra > 0 && ((uintptr_t)os_block & (alignment - 1)) != 0) {
    base = (char*)_pa_align_up((uintptr_t)os_block, (uintptr_t)alignment);
  }
  const size_t slop = (size_t)(base - (char*)os_block);
  const size_t actual_capacity = (slop < os_block_size) ? (os_block_size - slop) : 0;
  const size_t final_capacity  = (actual_capacity < capacity) ? actual_capacity : capacity;

  arena->heap           = heap;
  arena->os_block       = os_block;
  arena->os_block_size  = os_block_size;
  arena->base           = base;
  arena->alignment      = alignment;
  arena->capacity       = final_capacity;
  arena->used           = 0;
  arena->committed      = can_reserve ? 0 : final_capacity;
  arena->commit_grain   = commit_grain;
  arena->max_size       = 0;
  arena->use_os_reserve = can_reserve;

  /* Eagerly commit the first slab */
  if (can_reserve && final_capacity > 0) {
    const size_t first = (commit_grain < final_capacity) ? commit_grain : final_capacity;
    bool iz = false;
    _pa_prim_commit(base, first, &iz);
    arena->committed = first;
  }

  return arena;
}

/* ---------------------------------------------------------------------------
 * Vectorized Pointer Materialization Kernels (AVX2 & Scalar Fallback)
 * ----------------------------------------------------------------------- */

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx2")))
static void pa_vec_materialize_ptrs_avx2(void** out, uintptr_t base, size_t step, size_t count) {
  size_t i = 0;
  const size_t simd_stride = 4;
  const size_t simd_limit = count & ~(simd_stride - 1);

  const __m256i vstep = _mm256_set_epi64x(
    (long long)(3 * step),
    (long long)(2 * step),
    (long long)(1 * step),
    0
  );
  const __m256i vstride_advance = _mm256_set1_epi64x((long long)(simd_stride * step));
  __m256i vcur = _mm256_add_epi64(_mm256_set1_epi64x((long long)base), vstep);

  /* Dual-issue loop unrolling (8 pointers per iteration) */
  for (; i + 2 * simd_stride <= simd_limit; i += 2 * simd_stride) {
    __m256i vnext = _mm256_add_epi64(vcur, vstride_advance);
    _mm256_storeu_si256((__m256i*)&out[i], vcur);
    _mm256_storeu_si256((__m256i*)&out[i + simd_stride], vnext);
    vcur = _mm256_add_epi64(vnext, vstride_advance);
  }

  /* Single SIMD step */
  for (; i < simd_limit; i += simd_stride) {
    _mm256_storeu_si256((__m256i*)&out[i], vcur);
    vcur = _mm256_add_epi64(vcur, vstride_advance);
  }

  /* Tail scalar fallback */
  uintptr_t cur = base + i * step;
  for (; i < count; i++) {
    out[i] = (void*)cur;
    cur += step;
  }
}
#endif

static void pa_vec_materialize_ptrs_scalar(void** out, uintptr_t base, size_t step, size_t count) {
  uintptr_t cur = base;
  size_t i = 0;
  /* 4x loop unrolling */
  for (; i + 4 <= count; i += 4) {
    out[i]     = (void*)cur;
    out[i + 1] = (void*)(cur + step);
    out[i + 2] = (void*)(cur + 2 * step);
    out[i + 3] = (void*)(cur + 3 * step);
    cur += 4 * step;
  }
  for (; i < count; i++) {
    out[i] = (void*)cur;
    cur += step;
  }
}

/* AVX2 setup cost: ~12-15 cycles (vpbroadcastq + vmovdqu + first vpaddq).
 * Scalar 4x-unrolled: ~2-4 cycles/ptr with in-cache bump pointer.
 * Empirical cross-over (bench_vector_engine): N=16 → AVX2 is 0.20x (scalar wins).
 *                                             N=32 → AVX2 pays for itself.
 * Use N=32 (power-of-2) so the compiler folds the modulo into bit-masking. */
#define PA_VEC_ADAPTIVE_SCALAR_THRESHOLD 32

static inline void pa_vec_materialize_ptrs(void** out, uintptr_t base, size_t step, size_t count) {
  /* Hot fast-path: tiny batches bypass SIMD setup penalty entirely */
  if (pa_likely(count < PA_VEC_ADAPTIVE_SCALAR_THRESHOLD)) {
    pa_vec_materialize_ptrs_scalar(out, base, step, count);
    return;
  }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
  if (pa_cpu_has_avx2()) {
    pa_vec_materialize_ptrs_avx2(out, base, step, count);
    return;
  }
#endif
  pa_vec_materialize_ptrs_scalar(out, base, step, count);
}

/* ---------------------------------------------------------------------------
 * Allocation - Single & Bulk
 * ----------------------------------------------------------------------- */

void* pa_vec_arena_alloc(pa_vec_arena_t* arena, size_t size) pa_attr_noexcept {
  if (arena == NULL || size == 0) return NULL;

  const size_t aligned_size = PA_VEC_ALIGN_UP(size, arena->alignment);
  if (aligned_size < size) return NULL;

  if (aligned_size > SIZE_MAX - arena->used) return NULL;
  const size_t new_used = arena->used + aligned_size;

  if (new_used > arena->capacity) return NULL;
  if (arena->max_size != 0 && new_used > arena->max_size) return NULL;

  if (new_used > arena->committed) {
    size_t need = PA_VEC_ALIGN_UP(new_used, arena->commit_grain);
    if (need < new_used) return NULL;
    if (need > arena->capacity) need = arena->capacity;

    const size_t delta = need - arena->committed;
    if (delta > 0) {
      bool iz = false;
      const int cerr = _pa_prim_commit(arena->base + arena->committed, delta, &iz);
      if (cerr != 0) return NULL;
      arena->committed = need;
    }
  }

  void* p = arena->base + arena->used;
  arena->used = new_used;
  return p;
}

size_t pa_vec_arena_alloc_batch(pa_vec_arena_t* arena, void** out, size_t count, size_t size) pa_attr_noexcept {
  if (arena == NULL || out == NULL || count == 0 || size == 0) return 0;

  const size_t aligned_size = PA_VEC_ALIGN_UP(size, arena->alignment);
  if (aligned_size < size) return 0;

  size_t total_bytes;
  if (pa_count_size_overflow(count, aligned_size, &total_bytes)) return 0;
  if (total_bytes > SIZE_MAX - arena->used) return 0;

  const size_t new_used = arena->used + total_bytes;
  if (new_used > arena->capacity) return 0;
  if (arena->max_size != 0 && new_used > arena->max_size) return 0;

  /* Lazy commit: commit all slabs required for the batch at once */
  if (new_used > arena->committed) {
    size_t need = PA_VEC_ALIGN_UP(new_used, arena->commit_grain);
    if (need < new_used) return 0;
    if (need > arena->capacity) need = arena->capacity;

    const size_t delta = need - arena->committed;
    if (delta > 0) {
      bool iz = false;
      const int cerr = _pa_prim_commit(arena->base + arena->committed, delta, &iz);
      if (cerr != 0) return 0;
      arena->committed = need;
    }
  }

  const uintptr_t current_base = (uintptr_t)(arena->base + arena->used);
  arena->used = new_used;

  /* Materialize all pointers via SIMD or unrolled scalar into destination array */
  pa_vec_materialize_ptrs(out, current_base, aligned_size, count);
  return count;
}

/* ---------------------------------------------------------------------------
 * Reset & Destroy
 * ----------------------------------------------------------------------- */

void pa_vec_arena_reset(pa_vec_arena_t* arena) pa_attr_noexcept {
  if (arena == NULL) return;

  if (arena->committed > 0 && arena->use_os_reserve) {
    bool unused = false;
    _pa_prim_decommit(arena->base, arena->committed, &unused);
    arena->committed = 0;
  }

  arena->used = 0;
}

void pa_vec_arena_clear(pa_vec_arena_t* arena) pa_attr_noexcept {
  if (arena == NULL) return;
  arena->used = 0;
}

void pa_vec_arena_destroy(pa_vec_arena_t* arena) pa_attr_noexcept {
  if (arena == NULL) return;

  if (arena->os_block != NULL && arena->os_block_size > 0) {
    _pa_prim_free(arena->os_block, arena->os_block_size);
  }
  pa_free(arena);
}

void pa_vec_arena_set_max_size(pa_vec_arena_t* arena, size_t max_bytes) pa_attr_noexcept {
  if (arena != NULL) arena->max_size = max_bytes;
}

void pa_vec_arena_get_stats(const pa_vec_arena_t* arena, pa_vec_arena_stats_t* out) pa_attr_noexcept {
  if (out == NULL) return;
  if (arena == NULL) {
    out->capacity_bytes = out->committed_bytes = out->used_bytes = 0;
    return;
  }
  out->capacity_bytes  = arena->capacity;
  out->committed_bytes = arena->committed;
  out->used_bytes      = arena->used;
}

/* ---------------------------------------------------------------------------
 * NUMA-Aware 2-Tier Radix-Masked Page Binning & Single-CAS Splicer
 *
 * Tier 0 [0..31]  — local-NUMA pages  (CAS hits same NUMA node's LLC)
 * Tier 1 [32..63] — remote-NUMA pages (CAS incurs UPI/Infinity-Fabric hop)
 *
 * Flush local tier first to compact the local-coherency window, then flush
 * remote tier to batch cross-socket atomic traffic into the fewest round trips.
 * ----------------------------------------------------------------------- */

/* Half the bucket space per NUMA tier (power of 2 for bitmask folding) */
#define PALLOC_PAGE_BUCKET_HALF  32
#define PALLOC_PAGE_BUCKET_COUNT (PALLOC_PAGE_BUCKET_HALF * 2)  /* 64 total */
#define PALLOC_PAGE_BUCKET_MASK  (PALLOC_PAGE_BUCKET_HALF - 1)  /* 0x1F */

typedef struct palloc_page_bucket_s {
  pa_page_t*  page;
  pa_block_t* head;
  pa_block_t* tail;
} palloc_page_bucket_t;

static void pa_flush_page_bucket(palloc_page_bucket_t* bucket) {
  if (bucket == NULL || bucket->page == NULL) return;

  pa_page_t* const page = bucket->page;
  pa_block_t* const head = bucket->head;
  pa_block_t* const tail = bucket->tail;

  pa_thread_free_t tfreex;
  bool use_delayed = false;
  pa_thread_free_t tfree = pa_atomic_load_relaxed(&page->xthread_free);

  /* Single-CAS atomic splice of the entire sub-list into page->xthread_free */
  do {
    use_delayed = (pa_tf_delayed(tfree) == PA_USE_DELAYED_FREE);
    if pa_unlikely(use_delayed) {
      tfreex = pa_tf_set_delayed(tfree, PA_DELAYED_FREEING);
    } else {
      pa_block_set_next(page, tail, pa_tf_block(tfree));
      tfreex = pa_tf_set_block(tfree, head);
    }
  } while (!pa_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));

  /* Rare fallback: page requires delayed-free handling through heap list */
  if pa_unlikely(use_delayed) {
    pa_heap_t* const heap = (pa_heap_t*)(pa_atomic_load_acquire(&page->xheap));
    if (heap != NULL) {
      pa_block_t* dfree = pa_atomic_load_ptr_relaxed(pa_block_t, &heap->thread_delayed_free);
      do {
        pa_block_set_nextx(heap, tail, dfree, heap->keys);
      } while (!pa_atomic_cas_ptr_weak_release(pa_block_t, &heap->thread_delayed_free, &dfree, head));
    }

    tfree = pa_atomic_load_relaxed(&page->xthread_free);
    do {
      tfreex = tfree;
      tfreex = pa_tf_set_delayed(tfree, PA_NO_DELAYED_FREE);
    } while (!pa_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));
  }

  bucket->page = NULL;
  bucket->head = NULL;
  bucket->tail = NULL;
}

void pa_vector_batch_free(void** ptrs, size_t count) pa_attr_noexcept {
  if (ptrs == NULL || count == 0) return;

  /* 2-Tier NUMA scratchpad: [0..31] = local node, [32..63] = remote node.
   * Stack-allocated → zero heap pressure, fits in 2 cache lines per tier. */
  palloc_page_bucket_t buckets[PALLOC_PAGE_BUCKET_COUNT];
  for (size_t b = 0; b < PALLOC_PAGE_BUCKET_COUNT; b++) {
    buckets[b].page = NULL;
    buckets[b].head = NULL;
    buckets[b].tail = NULL;
  }

  const uintptr_t tid        = _pa_prim_thread_id();
  const size_t    local_node = _pa_prim_numa_node();  /* read once; stable for thread lifetime */

  for (size_t i = 0; i < count; i++) {
    void* p = ptrs[i];
    if (p == NULL) continue;

    pa_segment_t* const segment = _pa_ptr_segment(p);
    if pa_unlikely(segment == NULL || segment->kind == PA_SEGMENT_HUGE) {
      pa_free(p);
      continue;
    }

    const bool is_local_thread = (tid == pa_atomic_load_relaxed(&segment->thread_id));
    if (is_local_thread) {
      /* Thread-local free uses palloc's own fast path */
      pa_free(p);
      continue;
    }

    /* Cross-thread free: group by page to collapse N atomic ops → O(pages) */
    pa_page_t* const page  = _pa_segment_page_of(segment, p);
    pa_block_t* const block = (pa_page_has_aligned(page)
                                ? _pa_page_ptr_unalign(page, p)
                                : (pa_block_t*)p);

    /* 2-Tier hash:
     *   Bits [11..6] of page addr → 5-bit bucket index within tier [0..31]
     *   NUMA tier offset: +PALLOC_PAGE_BUCKET_HALF if segment is on a remote node */
    const size_t tier_base = (segment->node_id != (uint16_t)local_node)
                               ? PALLOC_PAGE_BUCKET_HALF  /* remote-NUMA tier */
                               : 0;                       /* local-NUMA  tier */
    const size_t h = tier_base + ((((uintptr_t)page) >> 6) & PALLOC_PAGE_BUCKET_MASK);

    if (buckets[h].page != NULL && buckets[h].page != page) {
      /* Hash collision within tier: flush victim chain first */
      pa_flush_page_bucket(&buckets[h]);
    }

    if (buckets[h].page == NULL) {
      buckets[h].page = page;
      buckets[h].head = block;
      buckets[h].tail = block;
      pa_block_set_next(page, block, NULL);
    } else {
      /* Prepend: constant-time in-place chaining via freed block payload */
      pa_block_set_next(page, block, buckets[h].head);
      buckets[h].head = block;
    }
  }

  /* Flush Tier 0 (local-NUMA) first: keeps coherency traffic on local interconnect.
   * Then Tier 1 (remote-NUMA): batches cross-socket atomic writes for minimal UPI round trips. */
  for (size_t b = 0; b < PALLOC_PAGE_BUCKET_COUNT; b++) {
    if (buckets[b].page != NULL) {
      pa_flush_page_bucket(&buckets[b]);
    }
  }

  pa_free(ptrs);
}


void** pa_vector_batch_alloc(size_t count, size_t size) pa_attr_noexcept {
  if (count == 0) return NULL;
  size_t total_ptrs;
  if (pa_count_size_overflow(count, sizeof(void*), &total_ptrs)) return NULL;
  pa_heap_t* heap = pa_heap_get_default();
  void** ptrs = (void**)pa_heap_malloc(heap, total_ptrs);
  if (!ptrs) return NULL;

  for (size_t i = 0; i < count; i++) {
    ptrs[i] = pa_heap_malloc(heap, size);
    if (ptrs[i] == NULL) {
      while (i > 0) pa_free(ptrs[--i]);
      pa_free(ptrs);
      return NULL;
    }
  }
  return ptrs;
}

float** pa_vector_batch_alloc_floats(size_t count, size_t dim) pa_attr_noexcept {
  if (count == 0) return NULL;
  size_t vec_size;
  if (pa_count_size_overflow(dim, sizeof(float), &vec_size)) return NULL;
  size_t total_ptrs;
  if (pa_count_size_overflow(count, sizeof(float*), &total_ptrs)) return NULL;
  pa_heap_t* heap = pa_heap_get_default();
  float** ptrs = (float**)pa_heap_malloc(heap, total_ptrs);
  if (!ptrs) return NULL;

  for (size_t i = 0; i < count; i++) {
    ptrs[i] = (float*)pa_heap_malloc_aligned(heap, vec_size, PA_VECTOR_ALIGNMENT_DEFAULT);
    if (ptrs[i] == NULL) {
      while (i > 0) pa_free(ptrs[--i]);
      pa_free(ptrs);
      return NULL;
    }
  }
  return ptrs;
}

/* ---------------------------------------------------------------------------
 * Bitmask Dense Tracking for Fixed-Size Vector Pools (Zero Cache Pollution)
 * ----------------------------------------------------------------------- */

#define PA_VEC_SLAB_SLOTS 64

typedef struct pa_vec_slab_s {
  struct pa_vec_slab_s* next;
  uint64_t              bitmap;      /* 1 = allocated, 0 = free */
  uint32_t              free_count;  /* available slots */
  uint32_t              capacity;    /* 64 */
  char*                 data;        /* aligned slot array */
} pa_vec_slab_t;

struct pa_vec_pool_s {
  pa_heap_t*     heap;
  size_t         object_size;  /* aligned slot size (>= 16B) */
  pa_vec_slab_t* slabs;
  pa_vec_slab_t* active_slab;  /* O(1) cursor to current non-full slab */
};

static pa_vec_slab_t* pa_vec_slab_create(pa_heap_t* heap, size_t object_size) {
  size_t data_bytes;
  if (pa_count_size_overflow(PA_VEC_SLAB_SLOTS, object_size, &data_bytes)) return NULL;

  pa_vec_slab_t* slab = (pa_vec_slab_t*)pa_heap_malloc(heap, sizeof(pa_vec_slab_t));
  if (!slab) return NULL;

  char* data = (char*)pa_heap_malloc_aligned(heap, data_bytes, PA_VECTOR_ALIGNMENT_DEFAULT);
  if (!data) {
    pa_free(slab);
    return NULL;
  }

  slab->next       = NULL;
  slab->bitmap     = 0; /* All 64 slots free */
  slab->free_count = PA_VEC_SLAB_SLOTS;
  slab->capacity   = PA_VEC_SLAB_SLOTS;
  slab->data       = data;
  return slab;
}

pa_vec_pool_t* pa_vec_pool_create(size_t object_size, size_t initial_count) pa_attr_noexcept {
  if (object_size == 0) return NULL;
  pa_heap_t* heap = pa_heap_get_default();

  pa_vec_pool_t* pool = (pa_vec_pool_t*)pa_heap_malloc(heap, sizeof(pa_vec_pool_t));
  if (!pool) return NULL;

  /* Align slot size to at least 16 bytes for SIMD compatibility */
  size_t real_obj_size = object_size;
  if (real_obj_size < 16) real_obj_size = 16;
  const size_t align_to = 16;
  real_obj_size = (real_obj_size + align_to - 1) & ~(align_to - 1);

  pool->heap        = heap;
  pool->object_size = real_obj_size;
  pool->slabs       = NULL;
  pool->active_slab = NULL;

  /* Pre-create initial slabs if requested */
  size_t needed_slabs = (initial_count + PA_VEC_SLAB_SLOTS - 1) / PA_VEC_SLAB_SLOTS;
  for (size_t s = 0; s < needed_slabs; s++) {
    pa_vec_slab_t* slab = pa_vec_slab_create(heap, real_obj_size);
    if (slab) {
      slab->next = pool->slabs;
      pool->slabs = slab;
      pool->active_slab = slab;
    }
  }

  return pool;
}

void* pa_vec_pool_alloc(pa_vec_pool_t* pool) pa_attr_noexcept {
  if (pool == NULL) return NULL;

  /* O(1) Fast-Path: check active_slab cursor */
  pa_vec_slab_t* slab = pool->active_slab;
  if pa_unlikely(slab == NULL || slab->free_count == 0) {
    /* Slow-Path: find first non-full slab or create one */
    slab = pool->slabs;
    while (slab != NULL && slab->free_count == 0) {
      slab = slab->next;
    }
    if (slab == NULL) {
      slab = pa_vec_slab_create(pool->heap, pool->object_size);
      if (!slab) return NULL;
      slab->next = pool->slabs;
      pool->slabs = slab;
    }
    pool->active_slab = slab;
  }

  /* O(1) single-cycle slot search using hardware TZCNT */
  const uint64_t free_bits = ~slab->bitmap;
  pa_assert_internal(free_bits != 0);
  const int slot = pa_ctz64(free_bits);

  slab->bitmap |= (1ULL << slot);
  slab->free_count--;

  return (void*)(slab->data + slot * pool->object_size);
}

void pa_vec_pool_free(pa_vec_pool_t* pool, void* ptr) pa_attr_noexcept {
  if (pool == NULL || ptr == NULL) return;

  const char* p = (const char*)ptr;
  const size_t total_slab_bytes = (size_t)PA_VEC_SLAB_SLOTS * pool->object_size;

  pa_vec_slab_t* slab = pool->slabs;
  while (slab != NULL) {
    if (p >= slab->data && p < slab->data + total_slab_bytes) {
      const size_t offset = (size_t)(p - slab->data);
      const size_t slot = offset / pool->object_size;
      if (slot < PA_VEC_SLAB_SLOTS) {
        slab->bitmap &= ~(1ULL << slot);
        slab->free_count++;
        /* Promote slab with freed slot as active cursor */
        pool->active_slab = slab;
      }
      return;
    }
    slab = slab->next;
  }
}

/* O(1) contiguous multi-slot search via bit-twiddling */
static inline int pa_slab_find_contiguous(uint64_t mask, size_t k) {
  if (k == 0 || k > 64) return -1;
  if (k == 1) return (mask != 0) ? pa_ctz64(mask) : -1;
  uint64_t run = mask;
  for (size_t i = 1; i < k; i++) {
    run = run & (run >> 1);
    if (run == 0) return -1;
  }
  return pa_ctz64(run);
}

void* pa_vec_pool_alloc_contiguous(pa_vec_pool_t* pool, size_t count) pa_attr_noexcept {
  if (pool == NULL || count == 0 || count > PA_VEC_SLAB_SLOTS) return NULL;

  /* Search existing slabs for a contiguous run of count free bits */
  pa_vec_slab_t* slab = pool->slabs;
  while (slab != NULL) {
    if (slab->free_count >= count) {
      int slot = pa_slab_find_contiguous(~slab->bitmap, count);
      if (slot >= 0) {
        uint64_t alloc_mask = (count == 64) ? UINT64_MAX : (((1ULL << count) - 1ULL) << slot);
        slab->bitmap |= alloc_mask;
        slab->free_count -= (uint32_t)count;
        return (void*)(slab->data + slot * pool->object_size);
      }
    }
    slab = slab->next;
  }

  /* No slab had contiguous space: create fresh slab */
  slab = pa_vec_slab_create(pool->heap, pool->object_size);
  if (!slab) return NULL;
  slab->next = pool->slabs;
  pool->slabs = slab;

  uint64_t alloc_mask = (count == 64) ? UINT64_MAX : ((1ULL << count) - 1ULL);
  slab->bitmap |= alloc_mask;
  slab->free_count -= (uint32_t)count;
  return (void*)slab->data;
}

void pa_vec_pool_free_contiguous(pa_vec_pool_t* pool, void* ptr, size_t count) pa_attr_noexcept {
  if (pool == NULL || ptr == NULL || count == 0 || count > PA_VEC_SLAB_SLOTS) return;

  const char* p = (const char*)ptr;
  const size_t total_slab_bytes = (size_t)PA_VEC_SLAB_SLOTS * pool->object_size;

  pa_vec_slab_t* slab = pool->slabs;
  while (slab != NULL) {
    if (p >= slab->data && p < slab->data + total_slab_bytes) {
      const size_t offset = (size_t)(p - slab->data);
      const size_t slot = offset / pool->object_size;
      if (slot + count <= PA_VEC_SLAB_SLOTS) {
        uint64_t free_mask = (count == 64) ? UINT64_MAX : (((1ULL << count) - 1ULL) << slot);
        slab->bitmap &= ~free_mask;
        slab->free_count += (uint32_t)count;
      }
      return;
    }
    slab = slab->next;
  }
}

void pa_vec_pool_destroy(pa_vec_pool_t* pool) pa_attr_noexcept {
  if (pool == NULL) return;
  pa_vec_slab_t* slab = pool->slabs;
  while (slab != NULL) {
    pa_vec_slab_t* next = slab->next;
    if (slab->data != NULL) {
      pa_free(slab->data);
    }
    pa_free(slab);
    slab = next;
  }
  pa_free(pool);
}

#endif /* PA_VECTOR */
