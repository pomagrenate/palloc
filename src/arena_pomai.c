/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/atomic.h"
#include "palloc/prim.h"
#include "palloc/arena_pomai.h"

/*
 * Vector arena: single contiguous OS reservation via _pa_prim_alloc(commit=false).
 * Header at start of block; payload follows at SIMD-aligned offset.
 *
 * Physical memory model:
 *   Creation : one commit-slab (2 MiB) committed; rest is virtual-only.
 *   Alloc    : lazy per-slab commit advances alongside the bump pointer.
 *   Reset    : decommit all payload pages; RAM returned to OS immediately.
 *   Destroy  : _pa_prim_free releases the full reservation in one call.
 *
 * Zero-OOM: hard capacity fuse; alloc returns NULL when full.
 * NEVER aborts. Graceful NULL on commit failure (e.g. OOM from OS).
 *
 * Thread safety:
 *   shared=false : single-threaded; bump and commit use plain fields.
 *   shared=true  : multi-threaded; bump uses lock-free CAS; commit uses
 *                  a monotone committed_atomic protected by commit_lock.
 */

/* ---------------------------------------------------------------------------
 * Commit slab: 2 MiB - one large-page boundary.
 * Physical RAM is committed in increments of this size.
 * At most one extra slab beyond actual used bytes is committed at any time.
 * ----------------------------------------------------------------------- */
#define PA_ARENA_COMMIT_GRAIN  (2u * 1024u * 1024u)

typedef struct pa_arena_vector_s {
  /* Bump pointer (shared path uses atomic; single-thread path uses plain). */
  _Atomic(uintptr_t) offset_atomic;
  uintptr_t          offset_plain;

  /* Committed watermark: bytes committed from arena base.
   * Shared path: committed_atomic is the authoritative value.
   * Single path: committed_plain is used (no atomics needed). */
  _Atomic(uintptr_t) committed_atomic;
  size_t             committed_plain;

  /* Lock for serializing OS commit calls in shared mode */
  _Atomic(uintptr_t) commit_lock;

  size_t  payload_start;       /* first byte available for allocations (aligned offset from arena base) */
  size_t  payload_capacity;    /* usable bytes (requested capacity, page-aligned) */
  size_t  max_capacity_bytes;  /* STRICT HARD LIMIT (fuse): never exceed this */
  size_t  total_size;          /* full OS reservation size */
  size_t  commit_grain;        /* commit slab size (page-aligned, e.g. 2 MiB) */
  bool    shared;
  bool    use_os_reserve;      /* true = lazy commit in use; false = fully committed fallback */
  uint8_t _pad[6];
} pa_arena_vector_t;

#define PA_ARENA_VECTOR_HEADER_SIZE  (sizeof(pa_arena_vector_t))

/* pa_arena_t is the public opaque type. */
typedef pa_arena_vector_t pa_arena_impl_t;

/* Payload starts at the first address >= header that satisfies both
 * PA_ARENA_VECTOR_ALIGN and OS page size alignment. */
static inline size_t pa_arena_payload_start(void) {
  const size_t page_size = _pa_os_page_size();
  const size_t align = (page_size > PA_ARENA_VECTOR_ALIGN) ? page_size : PA_ARENA_VECTOR_ALIGN;
  return (size_t)_pa_align_up((uintptr_t)PA_ARENA_VECTOR_HEADER_SIZE, (uintptr_t)align);
}

/* Hard limit: bump pointer must not exceed payload_start + max_capacity_bytes. */
static inline uintptr_t pa_arena_hard_limit(const pa_arena_vector_t* arena) {
  return (uintptr_t)arena->payload_start + (uintptr_t)arena->max_capacity_bytes;
}

/* ---------------------------------------------------------------------------
 * Creation
 * ----------------------------------------------------------------------- */

pa_arena_t* p_arena_create_for_vector(size_t capacity_bytes) pa_attr_noexcept {
  return p_arena_create_for_vector_ex(capacity_bytes, false);
}

pa_arena_t* p_arena_create_for_vector_ex(size_t capacity_bytes, bool shared) pa_attr_noexcept {
  if (capacity_bytes == 0) return NULL;

  const size_t page_size  = _pa_os_page_size();
  const size_t payload_start = pa_arena_payload_start();

  /* Round total_size to page granularity (not large_page_size).
   * This avoids wasting up to 4 MiB of address space on large-page alignment. */
  if (capacity_bytes > SIZE_MAX - payload_start) return NULL;
  const size_t total_size = _pa_align_up(payload_start + capacity_bytes, page_size);

  /* Determine commit grain: max(PA_ARENA_COMMIT_GRAIN, page_size). */
  size_t commit_grain = PA_ARENA_COMMIT_GRAIN;
  if (commit_grain < page_size) commit_grain = page_size;
  commit_grain = _pa_align_up(commit_grain, page_size);

  /* Reserve (but do not commit) the virtual address space.
   * On Linux  : mmap(PROT_NONE) - no pages touched.
   * On Windows : VirtualAlloc(MEM_RESERVE) - no RAM consumed.
   * Fallback   : full commit if platform has no virtual reserve. */
  const bool can_reserve = _pa_os_has_virtual_reserve();
  bool is_large = false;
  bool is_zero  = false;
  void* p = NULL;
  const int err = _pa_prim_alloc(NULL, total_size, page_size,
                                  /*commit=*/!can_reserve,
                                  /*allow_large=*/false,
                                  &is_large, &is_zero, &p);
  if (err != 0 || p == NULL) return NULL;

  /* Eagerly commit the first slab (header + first grain of payload) BEFORE touching p!
   * Accessing arena struct fields requires memory at p to be committed first. */
  size_t committed_size = total_size;
  if (can_reserve) {
    const size_t first_commit_end  = _pa_align_up(payload_start + commit_grain, page_size);
    const size_t first_commit_size = (first_commit_end < total_size) ? first_commit_end : total_size;
    bool iz = false;
    const int cerr = _pa_prim_commit(p, first_commit_size, &iz);
    if (cerr != 0) {
      _pa_prim_free(p, total_size);
      return NULL;
    }
    committed_size = first_commit_size;
  }

  pa_arena_vector_t* arena = (pa_arena_vector_t*)p;
  arena->payload_start      = payload_start;
  arena->payload_capacity   = capacity_bytes;
  arena->max_capacity_bytes = capacity_bytes;
  arena->total_size         = total_size;
  arena->commit_grain       = commit_grain;
  arena->shared             = shared;
  arena->use_os_reserve     = can_reserve;

  pa_atomic_store_release(&arena->commit_lock, 0);

  if (shared) {
    pa_atomic_store_release(&arena->offset_atomic,    (uintptr_t)payload_start);
    pa_atomic_store_release(&arena->committed_atomic, (uintptr_t)committed_size);
    arena->offset_plain    = 0;
    arena->committed_plain = 0;
  } else {
    arena->offset_plain    = payload_start;
    arena->committed_plain = committed_size;
    pa_atomic_store_release(&arena->offset_atomic,    (uintptr_t)payload_start);
    pa_atomic_store_release(&arena->committed_atomic, (uintptr_t)committed_size);
  }

  return (pa_arena_t*)arena;
}

/* ---------------------------------------------------------------------------
 * Internal: ensure [arena_base, arena_base + need_committed) is committed.
 * Called after the bump pointer has been reserved in the alloc fast-path.
 *
 * Single-thread path: straightforward watermark advance.
 * Shared path       : lock-free fast-path; commit_lock serializes actual OS commit.
 * _pa_prim_commit is idempotent so double-committing is safe.
 * ----------------------------------------------------------------------- */

static int pa_arena_ensure_committed_single(pa_arena_vector_t* arena,
                                             size_t need_committed) {
  const size_t cur = arena->committed_plain;
  if (need_committed <= cur) return 0;

  /* Round up to next grain, clamped to total_size. */
  size_t new_committed = _pa_align_up(need_committed, arena->commit_grain);
  if (new_committed < need_committed) return EINVAL; /* overflow */
  if (new_committed > arena->total_size) new_committed = arena->total_size;

  const size_t delta = new_committed - cur;
  bool iz = false;
  const int err = _pa_prim_commit((char*)arena + cur, delta, &iz);
  if (err != 0) return err;
  arena->committed_plain = new_committed;
  return 0;
}

static int pa_arena_ensure_committed_shared(pa_arena_vector_t* arena,
                                            size_t need_committed) {
  uintptr_t cur = pa_atomic_load_acquire(&arena->committed_atomic);
  if ((size_t)cur >= need_committed) return 0;

  /* Acquire commit_lock to ensure only one thread issues _pa_prim_commit */
  while (true) {
    uintptr_t expected = 0;
    if (pa_atomic_cas_strong_acq_rel(&arena->commit_lock, &expected, 1)) {
      /* Locked */
      cur = pa_atomic_load_acquire(&arena->committed_atomic);
      if ((size_t)cur < need_committed) {
        size_t new_committed = _pa_align_up(need_committed, arena->commit_grain);
        if (new_committed < need_committed) {
          pa_atomic_store_release(&arena->commit_lock, 0);
          return EINVAL;
        }
        if (new_committed > arena->total_size) new_committed = arena->total_size;

        const size_t delta = new_committed - (size_t)cur;
        bool iz = false;
        const int err = _pa_prim_commit((char*)arena + cur, delta, &iz);
        if (err != 0) {
          pa_atomic_store_release(&arena->commit_lock, 0);
          return err;
        }
        pa_atomic_store_release(&arena->committed_atomic, (uintptr_t)new_committed);
      }
      pa_atomic_store_release(&arena->commit_lock, 0);
      return 0;
    }
    /* Another thread holds the lock, check if our range was already committed */
    cur = pa_atomic_load_acquire(&arena->committed_atomic);
    if ((size_t)cur >= need_committed) return 0;
    pa_atomic_yield();
  }
}

/* ---------------------------------------------------------------------------
 * Allocation - O(1) bump (single-thread) or lock-free CAS (shared)
 * ----------------------------------------------------------------------- */

void* p_arena_alloc_vector(pa_arena_t* arena_ptr, size_t vector_dim, size_t element_size) pa_attr_noexcept {
  if (arena_ptr == NULL) return NULL;
  if (element_size == 0 || (vector_dim > 0 && element_size > SIZE_MAX / vector_dim)) return NULL;

  const size_t size = vector_dim * element_size;
  if (size == 0) return NULL;

  pa_arena_vector_t* arena = (pa_arena_vector_t*)arena_ptr;

  /* Aligned size - guard against overflow near SIZE_MAX. */
  const size_t aligned_size = (size_t)_pa_align_up((uintptr_t)size, (uintptr_t)PA_ARENA_VECTOR_ALIGN);
  if (aligned_size < size) return NULL; /* alignment round-up wrapped */

  const uintptr_t hard_limit = pa_arena_hard_limit(arena);
  const uintptr_t base       = (uintptr_t)arena;

  if (arena->shared) {
    /* Lock-free fetch-add style bump using CAS. */
    uintptr_t old = pa_atomic_load_relaxed(&arena->offset_atomic);
    while (true) {
      /* Guard: old + aligned_size must not overflow uintptr_t. */
      if (aligned_size > hard_limit || old > hard_limit - aligned_size) {
        return NULL;  /* graceful capacity exhaustion - no abort */
      }
      const uintptr_t new_offset = old + (uintptr_t)aligned_size;
      if (pa_atomic_cas_weak_acq_rel(&arena->offset_atomic, &old, new_offset)) {
        /* Bump succeeded; ensure physical pages are committed. */
        if (arena->use_os_reserve) {
          const size_t need = (size_t)new_offset; /* offset from arena base */
          if (pa_arena_ensure_committed_shared(arena, need) != 0) {
            /* OOM from OS: roll back the bump pointer. */
            uintptr_t cur_val = new_offset;
            pa_atomic_cas_strong_acq_rel(&arena->offset_atomic, &cur_val, old);
            return NULL;
          }
        }
        return (void*)(base + old);
      }
      /* CAS spuriously failed; old was updated by the CAS - retry. */
    }
  } else {
    /* Single-thread path: no atomics needed. */
    uintptr_t cur = (uintptr_t)arena->offset_plain;
    if (aligned_size > hard_limit || cur > hard_limit - aligned_size) return NULL;
    const uintptr_t new_offset = cur + (uintptr_t)aligned_size;

    if (arena->use_os_reserve) {
      const size_t need = (size_t)new_offset;
      if (pa_arena_ensure_committed_single(arena, need) != 0) return NULL;
    }

    arena->offset_plain = new_offset;
    return (void*)(base + cur);
  }
}

/* ---------------------------------------------------------------------------
 * Reset - O(1) decommit + bump pointer reset
 * ----------------------------------------------------------------------- */

void p_arena_reset(pa_arena_t* arena_ptr) pa_attr_noexcept {
  if (arena_ptr == NULL) return;
  pa_arena_vector_t* arena = (pa_arena_vector_t*)arena_ptr;
  const uintptr_t start = (uintptr_t)arena->payload_start;

  if (arena->shared) {
    pa_atomic_store_release(&arena->offset_atomic, start);
  } else {
    arena->offset_plain = start;
  }

  /* Decommit payload to return physical RAM to the OS.
   * We decommit from payload_start (not from 0) to keep the header accessible.
   * On Linux  : MADV_DONTNEED - pages freed immediately, mprotect if secure.
   * On Windows : VirtualFree(MEM_DECOMMIT) - RAM returned to system pool.
   * committed watermark is reset to payload_start (header only stays committed). */
  if (arena->use_os_reserve) {
    void* payload    = (char*)arena + arena->payload_start;
    size_t committed;

    if (arena->shared) {
      committed = (size_t)pa_atomic_load_acquire(&arena->committed_atomic);
    } else {
      committed = arena->committed_plain;
    }

    if (committed > arena->payload_start) {
      const size_t decommit_size = committed - arena->payload_start;
      bool unused = false;
      _pa_prim_decommit(payload, decommit_size, &unused);
    }

    /* Reset watermark to payload_start (header slab stays committed). */
    if (arena->shared) {
      pa_atomic_store_release(&arena->committed_atomic, (uintptr_t)arena->payload_start);
    } else {
      arena->committed_plain = arena->payload_start;
    }
  }
}

/* ---------------------------------------------------------------------------
 * Destroy - release the full reservation to the OS in one call
 * ----------------------------------------------------------------------- */

void p_arena_destroy(pa_arena_t* arena_ptr) pa_attr_noexcept {
  if (arena_ptr == NULL) return;
  pa_arena_vector_t* arena = (pa_arena_vector_t*)arena_ptr;
  _pa_prim_free((void*)arena, arena->total_size);
}

/* ---------------------------------------------------------------------------
 * Legacy API (byte-based, same arena layout)
 * ----------------------------------------------------------------------- */

pa_decl_export void* p_arena_create(size_t size) pa_attr_noexcept {
  return (void*)p_arena_create_for_vector(size);
}

pa_decl_export void* p_arena_alloc(void* arena_ptr, size_t size) pa_attr_noexcept {
  if (arena_ptr == NULL || size == 0) return NULL;
  return p_arena_alloc_vector((pa_arena_t*)arena_ptr, size, 1);
}
