/* ----------------------------------------------------------------------------
 * Vector-oriented allocator extension for palloc.
 * Optional module: build with -DPA_VECTOR=ON. Uses core pa_* APIs only.
 * --------------------------------------------------------------------------*/
#pragma once
#ifndef PALLOC_VECTOR_H
#define PALLOC_VECTOR_H

#include "palloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default alignment for SIMD (AVX2/AVX-512). Vector APIs use this when applicable. */
#define PA_VECTOR_ALIGNMENT_DEFAULT  64

/**
 * Allocate size bytes with the given alignment (e.g. for SIMD).
 * Uses the default heap. Thin wrapper around pa_malloc_aligned.
 */
pa_decl_nodiscard static inline void* pa_vector_alloc_aligned(size_t size, size_t alignment) pa_attr_noexcept {
  return pa_malloc_aligned(size, alignment);
}

/**
 * Allocate a vector of dim floats, aligned to PA_VECTOR_ALIGNMENT_DEFAULT.
 * Returns (float*) to aligned memory; caller can use as float[dim].
 * Returns NULL if dim is 0 or if dim*sizeof(float) would overflow.
 */
pa_decl_nodiscard static inline float* pa_vector_alloc_floats(size_t dim) pa_attr_noexcept {
  if (dim == 0 || dim > (size_t)-1 / sizeof(float)) return NULL;
  return (float*)pa_vector_alloc_aligned(dim * sizeof(float), PA_VECTOR_ALIGNMENT_DEFAULT);
}

#if defined(PA_VECTOR) && PA_VECTOR
/* --------------------------------------------------------------------------
 * Optional vector module APIs (built when PA_VECTOR=ON)
 * ----------------------------------------------------------------------- */

/** Opaque heap-backed bump arena for contiguous vector allocations. */
typedef struct pa_vec_arena_s pa_vec_arena_t;

pa_decl_nodiscard pa_decl_export pa_vec_arena_t* pa_vec_arena_create(size_t initial_size, size_t alignment) pa_attr_noexcept;
pa_decl_nodiscard pa_decl_export pa_vec_arena_t* pa_vec_arena_create_ex(size_t initial_size, size_t alignment, pa_heap_t* heap) pa_attr_noexcept;
pa_decl_nodiscard pa_decl_export void* pa_vec_arena_alloc(pa_vec_arena_t* arena, size_t size) pa_attr_noexcept pa_attr_malloc;
pa_decl_nodiscard pa_decl_export size_t pa_vec_arena_alloc_batch(pa_vec_arena_t* arena, void** out, size_t count, size_t size) pa_attr_noexcept;
pa_decl_export void pa_vec_arena_reset(pa_vec_arena_t* arena) pa_attr_noexcept;
pa_decl_export void pa_vec_arena_clear(pa_vec_arena_t* arena) pa_attr_noexcept;
pa_decl_export void pa_vec_arena_destroy(pa_vec_arena_t* arena) pa_attr_noexcept;
pa_decl_export void pa_vec_arena_set_max_size(pa_vec_arena_t* arena, size_t max_bytes) pa_attr_noexcept;

/** Diagnostic statistics: capacity / committed / used bytes. */
typedef struct pa_vec_arena_stats_s {
  size_t capacity_bytes;   /**< Total virtual bytes reserved for vectors. */
  size_t committed_bytes;  /**< Physical RAM currently committed (≤ capacity). */
  size_t used_bytes;       /**< Bytes allocated via the bump pointer. */
} pa_vec_arena_stats_t;

/** Fill *out with current arena memory statistics. Safe to call at any time. */
pa_decl_export void pa_vec_arena_get_stats(const pa_vec_arena_t* arena, pa_vec_arena_stats_t* out) pa_attr_noexcept;

pa_decl_nodiscard pa_decl_export void** pa_vector_batch_alloc(size_t count, size_t size) pa_attr_noexcept pa_attr_malloc;
pa_decl_export void pa_vector_batch_free(void** ptrs, size_t count) pa_attr_noexcept;
pa_decl_nodiscard pa_decl_export float** pa_vector_batch_alloc_floats(size_t count, size_t dim) pa_attr_noexcept pa_attr_malloc;

/** Opaque fixed-size object pool for vectors (dense 64-bit bitmap tracked). */
typedef struct pa_vec_pool_s pa_vec_pool_t;

pa_decl_nodiscard pa_decl_export pa_vec_pool_t* pa_vec_pool_create(size_t object_size, size_t initial_count) pa_attr_noexcept;
pa_decl_nodiscard pa_decl_export void* pa_vec_pool_alloc(pa_vec_pool_t* pool) pa_attr_noexcept pa_attr_malloc;
pa_decl_export void pa_vec_pool_free(pa_vec_pool_t* pool, void* ptr) pa_attr_noexcept;

/**
 * Allocate `count` contiguous vector slots from the pool.
 * Uses hardware bit-twiddling intrinsics (O(1)) without touching object memory.
 * Returns pointer to the first contiguous slot, or NULL if capacity unavailable.
 */
pa_decl_nodiscard pa_decl_export void* pa_vec_pool_alloc_contiguous(pa_vec_pool_t* pool, size_t count) pa_attr_noexcept pa_attr_malloc;

/**
 * Free `count` contiguous vector slots previously allocated by pa_vec_pool_alloc_contiguous.
 */
pa_decl_export void pa_vec_pool_free_contiguous(pa_vec_pool_t* pool, void* ptr, size_t count) pa_attr_noexcept;

pa_decl_export void pa_vec_pool_destroy(pa_vec_pool_t* pool) pa_attr_noexcept;

#endif /* PA_VECTOR */

#ifdef __cplusplus
}
#endif

#endif /* PALLOC_VECTOR_H */
