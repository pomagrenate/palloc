/* ----------------------------------------------------------------------------
Copyright (c) 2018-2025, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/atomic.h"
#include "palloc/prim.h"

#define pa_os_stat_increase(stat,amount)      _pa_stat_increase(&_pa_stats_main.stat, amount)
#define pa_os_stat_decrease(stat,amount)      _pa_stat_decrease(&_pa_stats_main.stat, amount)
#define pa_os_stat_counter_increase(stat,inc) _pa_stat_counter_increase(&_pa_stats_main.stat, inc)

/* -----------------------------------------------------------
  Initialization.
----------------------------------------------------------- */
#ifndef PA_DEFAULT_VIRTUAL_ADDRESS_BITS
#if PA_INTPTR_SIZE < 8
#define PA_DEFAULT_VIRTUAL_ADDRESS_BITS     32
#else
#define PA_DEFAULT_VIRTUAL_ADDRESS_BITS     48
#endif
#endif

#ifndef PA_DEFAULT_PHYSICAL_MEMORY_IN_KIB
#if PA_INTPTR_SIZE < 8
#define PA_DEFAULT_PHYSICAL_MEMORY_IN_KIB   4*PA_MiB    // 4 GiB
#else
#define PA_DEFAULT_PHYSICAL_MEMORY_IN_KIB   32*PA_MiB   // 32 GiB
#endif
#endif

static pa_os_mem_config_t pa_os_mem_config = {
  4096,     // page size
  0,        // large page size (usually 2MiB)
  4096,     // allocation granularity
  PA_DEFAULT_PHYSICAL_MEMORY_IN_KIB,
  PA_DEFAULT_VIRTUAL_ADDRESS_BITS,
  true,     // has overcommit?  (if true we use MAP_NORESERVE on mmap systems)
  false,    // can we partially free allocated blocks? (on mmap systems we can free anywhere in a mapped range, but on Windows we must free the entire span)
  true      // has virtual reserve? (if true we can reserve virtual address space without using commit or physical memory)
};

bool _pa_os_has_overcommit(void) {
  return pa_os_mem_config.has_overcommit;
}

bool _pa_os_has_virtual_reserve(void) {
  return pa_os_mem_config.has_virtual_reserve;
}


// OS (small) page size
size_t _pa_os_page_size(void) {
  return pa_os_mem_config.page_size;
}

// if large OS pages are supported (2 or 4MiB), then return the size, otherwise return the small page size (4KiB)
size_t _pa_os_large_page_size(void) {
  return (pa_os_mem_config.large_page_size != 0 ? pa_os_mem_config.large_page_size : _pa_os_page_size());
}

bool _pa_os_canuse_large_page(size_t size, size_t alignment) {
  // if we have access, check the size and alignment requirements
  if (pa_os_mem_config.large_page_size == 0) return false;
  return ((size % pa_os_mem_config.large_page_size) == 0 && (alignment % pa_os_mem_config.large_page_size) == 0);
}

// round to a good OS allocation size (bounded by max 12.5% waste)
size_t _pa_os_good_alloc_size(size_t size) {
  size_t align_size;
  if (size < 512*PA_KiB) align_size = _pa_os_page_size();
  else if (size < 2*PA_MiB) align_size = 64*PA_KiB;
  else if (size < 8*PA_MiB) align_size = 256*PA_KiB;
  else if (size < 32*PA_MiB) align_size = 1*PA_MiB;
  else align_size = 4*PA_MiB;
  if pa_unlikely(size >= (SIZE_MAX - align_size)) return size; // possible overflow?
  return _pa_align_up(size, align_size);
}

void _pa_os_init(void) {
  _pa_prim_mem_init(&pa_os_mem_config);
}


/* -----------------------------------------------------------
  Util
-------------------------------------------------------------- */
bool _pa_os_decommit(void* addr, size_t size);
bool _pa_os_commit(void* addr, size_t size, bool* is_zero);


/* -----------------------------------------------------------
  aligned hinting
-------------------------------------------------------------- */

// On systems with enough virtual address bits, we can do efficient aligned allocation by using
// the 2TiB to 30TiB area to allocate those. If we have at least 46 bits of virtual address
// space (64TiB) we use this technique. (but see issue #939)
#if (PA_INTPTR_SIZE >= 8) && !defined(PA_NO_ALIGNED_HINT)
static pa_decl_cache_align _Atomic(uintptr_t)aligned_base;

// Return a PA_SEGMENT_SIZE aligned address that is probably available.
// If this returns NULL, the OS will determine the address but on some OS's that may not be
// properly aligned which can be more costly as it needs to be adjusted afterwards.
// For a size > 1GiB this always returns NULL in order to guarantee good ASLR randomization;
// (otherwise an initial large allocation of say 2TiB has a 50% chance to include (known) addresses
//  in the middle of the 2TiB - 6TiB address range (see issue #372))

#define PA_HINT_BASE ((uintptr_t)2 << 40)  // 2TiB start
#define PA_HINT_AREA ((uintptr_t)4 << 40)  // upto 6TiB   (since before win8 there is "only" 8TiB available to processes)
#define PA_HINT_MAX  ((uintptr_t)30 << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pages)

void* _pa_os_get_aligned_hint(size_t try_alignment, size_t size)
{
  if (try_alignment <= 1 || try_alignment > PA_SEGMENT_SIZE) return NULL;
  if (pa_os_mem_config.virtual_address_bits < 46) return NULL;  // < 64TiB virtual address space
  size = _pa_align_up(size, PA_SEGMENT_SIZE);
  if (size > 1*PA_GiB) return NULL;  // guarantee the chance of fixed valid address is at most 1/(PA_HINT_AREA / 1<<30) = 1/4096.
  #if (PA_SECURE>0)
  size += PA_SEGMENT_SIZE;        // put in `PA_SEGMENT_SIZE` virtual gaps between hinted blocks; this splits VLA's but increases guarded areas.
  #endif

  uintptr_t hint = pa_atomic_add_acq_rel(&aligned_base, size);
  if (hint == 0 || hint > PA_HINT_MAX) {   // wrap or initialize
    uintptr_t init = PA_HINT_BASE;
    #if (PA_SECURE>0 || PA_DEBUG==0)       // security: randomize start of aligned allocations unless in debug mode
    pa_heap_t* heap = pa_prim_get_default_heap();
    uintptr_t r = pa_heap_is_initialized(heap) ? _pa_heap_random_next(heap) : _pa_os_random_weak(37);
    init = init + ((PA_SEGMENT_SIZE * ((r>>17) & 0xFFFFF)) % PA_HINT_AREA);  // (randomly 20 bits)*4MiB == 0 to 4TiB
    #endif
    uintptr_t expected = hint + size;
    pa_atomic_cas_strong_acq_rel(&aligned_base, &expected, init);
    hint = pa_atomic_add_acq_rel(&aligned_base, size); // this may still give 0 or > PA_HINT_MAX but that is ok, it is a hint after all
  }
  if (hint%try_alignment != 0) return NULL;
  return (void*)hint;
}
#else
void* _pa_os_get_aligned_hint(size_t try_alignment, size_t size) {
  PA_UNUSED(try_alignment); PA_UNUSED(size);
  return NULL;
}
#endif

/* -----------------------------------------------------------
  Free memory
-------------------------------------------------------------- */

static void pa_os_free_huge_os_pages(void* p, size_t size);

static void pa_os_prim_free(void* addr, size_t size, size_t commit_size) {
  pa_assert_internal((size % _pa_os_page_size()) == 0);
  if (addr == NULL) return; // || _pa_os_is_huge_reserved(addr)
  int err = _pa_prim_free(addr, size);  // allow size==0 (issue #1041)
  if (err != 0) {
    _pa_warning_message("unable to free OS memory (error: %d (0x%x), size: 0x%zx bytes, address: %p)\n", err, err, size, addr);
  }
  if (commit_size > 0) {
    pa_os_stat_decrease(committed, commit_size);
  }
  pa_os_stat_decrease(reserved, size);
}

void _pa_os_free_ex(void* addr, size_t size, bool still_committed, pa_memid_t memid) {
  if (pa_memkind_is_os(memid.memkind)) {
    size_t csize = memid.mem.os.size;
    if (csize==0) { csize = _pa_os_good_alloc_size(size); }
    pa_assert_internal(csize >= size);
    size_t commit_size = (still_committed ? csize : 0);
    void* base = addr;
    // different base? (due to alignment)
    if (memid.mem.os.base != base) {
      pa_assert(memid.mem.os.base <= addr);
      base = memid.mem.os.base;
      const size_t diff = (uint8_t*)addr - (uint8_t*)memid.mem.os.base;
      if (memid.mem.os.size==0) {
        csize += diff;
      }
      if (still_committed) {
        commit_size -= diff;  // the (addr-base) part was already un-committed
      }
    }
    // free it
    if (memid.memkind == PA_MEM_OS_HUGE) {
      pa_assert(memid.is_pinned);
      pa_os_free_huge_os_pages(base, csize);
    }
    else {
      pa_os_prim_free(base, csize, (still_committed ? commit_size : 0));
    }
  }
  else {
    // nothing to do
    pa_assert(memid.memkind < PA_MEM_OS);
  }
}

void  _pa_os_free(void* p, size_t size, pa_memid_t memid) {
  _pa_os_free_ex(p, size, true, memid);
}


/* -----------------------------------------------------------
   Primitive allocation from the OS.
-------------------------------------------------------------- */

// Note: the `try_alignment` is just a hint and the returned pointer is not guaranteed to be aligned.
// Also `hint_addr` is a hint and may be ignored.
static void* pa_os_prim_alloc_at(void* hint_addr, size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero) {
  pa_assert_internal(size > 0 && (size % _pa_os_page_size()) == 0);
  pa_assert_internal(is_zero != NULL);
  pa_assert_internal(is_large != NULL);
  if (size == 0) return NULL;
  if (!commit) { allow_large = false; }
  if (try_alignment == 0) { try_alignment = 1; } // avoid 0 to ensure there will be no divide by zero when aligning
  *is_zero = false;
  void* p = NULL;
  int err = _pa_prim_alloc(hint_addr, size, try_alignment, commit, allow_large, is_large, is_zero, &p);
  if (err != 0) {
    _pa_warning_message("unable to allocate OS memory (error: %d (0x%x), addr: %p, size: 0x%zx bytes, align: 0x%zx, commit: %d, allow large: %d)\n", err, err, hint_addr, size, try_alignment, commit, allow_large);
  }



  pa_os_stat_counter_increase(mmap_calls, 1);
  if (p != NULL) {
    pa_os_stat_increase(reserved, size);
    if (commit) {
      pa_os_stat_increase(committed, size);
      // seems needed for asan (or `palloc-test-api` fails)
      #ifdef PA_TRACK_ASAN
      if (*is_zero) { pa_track_mem_defined(p,size); }
               else { pa_track_mem_undefined(p,size); }
      #endif
    }
  }
  return p;
}

static void* pa_os_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero) {
  return pa_os_prim_alloc_at(NULL, size, try_alignment, commit, allow_large, is_large, is_zero);
}


// Primitive aligned allocation from the OS.
// This function guarantees the allocated memory is aligned.
static void* pa_os_prim_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** base) {
  pa_assert_internal(alignment >= _pa_os_page_size() && ((alignment & (alignment - 1)) == 0));
  pa_assert_internal(size > 0 && (size % _pa_os_page_size()) == 0);
  pa_assert_internal(is_large != NULL);
  pa_assert_internal(is_zero != NULL);
  pa_assert_internal(base != NULL);
  if (!commit) allow_large = false;
  if (!(alignment >= _pa_os_page_size() && ((alignment & (alignment - 1)) == 0))) return NULL;
  size = _pa_align_up(size, _pa_os_page_size());

  // try first with a requested alignment hint (this will usually be aligned directly on Win 10+ or BSD)
  void* p = pa_os_prim_alloc(size, alignment, commit, allow_large, is_large, is_zero);
  if (p == NULL) return NULL;

  // aligned already?
  if (((uintptr_t)p % alignment) == 0) {
    *base = p;
  }
  else {
    // if not aligned, free it, overallocate, and unmap around it
    #if !PA_TRACK_ASAN
    _pa_warning_message("unable to allocate aligned OS memory directly, fall back to over-allocation (size: 0x%zx bytes, address: %p, alignment: 0x%zx, commit: %d)\n", size, p, alignment, commit);
    #endif
    if (p != NULL) { pa_os_prim_free(p, size, (commit ? size : 0)); }
    if (size >= (SIZE_MAX - alignment)) return NULL; // overflow
    const size_t over_size = size + alignment;

    if (!pa_os_mem_config.has_partial_free) {  // win32 virtualAlloc cannot free parts of an allocated block
      // over-allocate uncommitted (virtual) memory
      p = pa_os_prim_alloc(over_size, 1 /*alignment*/, false /* commit? */, false /* allow_large */, is_large, is_zero);
      if (p == NULL) return NULL;

      // set p to the aligned part in the full region
      // note: this is dangerous on Windows as VirtualFree needs the actual base pointer
      // this is handled though by having the `base` field in the memid's
      *base = p; // remember the base
      p = pa_align_up_ptr(p, alignment);

      // explicitly commit only the aligned part
      if (commit) {
        if (!_pa_os_commit(p, size, NULL)) {
          pa_os_prim_free(*base, over_size, 0);
          return NULL;
        }
      }
    }
    else  { // mmap can free inside an allocation
      // overallocate...
      p = pa_os_prim_alloc(over_size, 1, commit, false, is_large, is_zero);
      if (p == NULL) return NULL;

      // and selectively unmap parts around the over-allocated area.
      void* aligned_p = pa_align_up_ptr(p, alignment);
      size_t pre_size = (uint8_t*)aligned_p - (uint8_t*)p;
      size_t mid_size = _pa_align_up(size, _pa_os_page_size());
      size_t post_size = over_size - pre_size - mid_size;
      pa_assert_internal(pre_size < over_size&& post_size < over_size&& mid_size >= size);
      if (pre_size > 0)  { pa_os_prim_free(p, pre_size, (commit ? pre_size : 0)); }
      if (post_size > 0) { pa_os_prim_free((uint8_t*)aligned_p + mid_size, post_size, (commit ? post_size : 0)); }
      // we can return the aligned pointer on `mmap` systems
      p = aligned_p;
      *base = aligned_p; // since we freed the pre part, `*base == p`.
    }
  }

  pa_assert_internal(p == NULL || (p != NULL && *base != NULL && ((uintptr_t)p % alignment) == 0));
  return p;
}


/* -----------------------------------------------------------
  OS API: alloc and alloc_aligned
----------------------------------------------------------- */

void* _pa_os_alloc(size_t size, pa_memid_t* memid) {
  *memid = _pa_memid_none();
  if (size == 0) return NULL;
  size = _pa_os_good_alloc_size(size);
  bool os_is_large = false;
  bool os_is_zero  = false;
  void* p = pa_os_prim_alloc(size, 0, true, false, &os_is_large, &os_is_zero);
  if (p == NULL) return NULL;

  *memid = _pa_memid_create_os(p, size, true, os_is_zero, os_is_large);
  pa_assert_internal(memid->mem.os.size >= size);
  pa_assert_internal(memid->initially_committed);
  return p;
}

void* _pa_os_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, pa_memid_t* memid)
{
  PA_UNUSED(&_pa_os_get_aligned_hint); // suppress unused warnings
  *memid = _pa_memid_none();
  if (size == 0) return NULL;
  size = _pa_os_good_alloc_size(size);
  alignment = _pa_align_up(alignment, _pa_os_page_size());

  bool os_is_large = false;
  bool os_is_zero  = false;
  void* os_base = NULL;
  void* p = pa_os_prim_alloc_aligned(size, alignment, commit, allow_large, &os_is_large, &os_is_zero, &os_base );
  if (p == NULL) return NULL;

  *memid = _pa_memid_create_os(p, size, commit, os_is_zero, os_is_large);
  memid->mem.os.base = os_base;
  memid->mem.os.size += ((uint8_t*)p - (uint8_t*)os_base);  // todo: return from prim_alloc_aligned?

  pa_assert_internal(memid->mem.os.size >= size);
  pa_assert_internal(_pa_is_aligned(p,alignment));
  if (commit) { pa_assert_internal(memid->initially_committed); }
  return p;
}


pa_decl_nodiscard static void* pa_os_ensure_zero(void* p, size_t size, pa_memid_t* memid) {
  if (p==NULL || size==0) return p;
  // ensure committed
  if (!memid->initially_committed) {
    bool is_zero = false;
    if (!_pa_os_commit(p, size, &is_zero)) {
      _pa_os_free(p, size, *memid);
      return NULL;
    }
    memid->initially_committed = true;
  }
  // ensure zero'd
  if (memid->initially_zero) return p;
  _pa_memzero_aligned(p,size);
  memid->initially_zero = true;
  return p;
}

void*  _pa_os_zalloc(size_t size, pa_memid_t* memid) {
  void* p = _pa_os_alloc(size,memid);
  return pa_os_ensure_zero(p, size, memid);
}

/* -----------------------------------------------------------
  OS aligned allocation with an offset. This is used
  for large alignments > PA_BLOCK_ALIGNMENT_MAX. We use a large palloc
  page where the object can be aligned at an offset from the start of the segment.
  As we may need to overallocate, we need to free such pointers using `pa_free_aligned`
  to use the actual start of the memory region.
----------------------------------------------------------- */

void* _pa_os_alloc_aligned_at_offset(size_t size, size_t alignment, size_t offset, bool commit, bool allow_large, pa_memid_t* memid) {
  pa_assert(offset <= PA_SEGMENT_SIZE);
  pa_assert(offset <= size);
  pa_assert((alignment % _pa_os_page_size()) == 0);
  *memid = _pa_memid_none();
  if (offset > PA_SEGMENT_SIZE) return NULL;
  if (offset == 0) {
    // regular aligned allocation
    return _pa_os_alloc_aligned(size, alignment, commit, allow_large, memid);
  }
  else {
    // overallocate to align at an offset
    const size_t extra = _pa_align_up(offset, alignment) - offset;
    const size_t oversize = size + extra;
    void* const start = _pa_os_alloc_aligned(oversize, alignment, commit, allow_large, memid);
    if (start == NULL) return NULL;

    void* const p = (uint8_t*)start + extra;
    pa_assert(_pa_is_aligned((uint8_t*)p + offset, alignment));
    // decommit the overallocation at the start
    if (commit && extra > _pa_os_page_size()) {
      _pa_os_decommit(start, extra);
    }
    return p;
  }
}

/* -----------------------------------------------------------
  OS memory API: reset, commit, decommit, protect, unprotect.
----------------------------------------------------------- */

// OS page align within a given area, either conservative (pages inside the area only),
// or not (straddling pages outside the area is possible)
static void* pa_os_page_align_areax(bool conservative, void* addr, size_t size, size_t* newsize) {
  pa_assert(addr != NULL && size > 0);
  if (newsize != NULL) *newsize = 0;
  if (size == 0 || addr == NULL) return NULL;

  // page align conservatively within the range
  void* start = (conservative ? pa_align_up_ptr(addr, _pa_os_page_size())
    : pa_align_down_ptr(addr, _pa_os_page_size()));
  void* end = (conservative ? pa_align_down_ptr((uint8_t*)addr + size, _pa_os_page_size())
    : pa_align_up_ptr((uint8_t*)addr + size, _pa_os_page_size()));
  ptrdiff_t diff = (uint8_t*)end - (uint8_t*)start;
  if (diff <= 0) return NULL;

  pa_assert_internal((conservative && (size_t)diff <= size) || (!conservative && (size_t)diff >= size));
  if (newsize != NULL) *newsize = (size_t)diff;
  return start;
}

static void* pa_os_page_align_area_conservative(void* addr, size_t size, size_t* newsize) {
  return pa_os_page_align_areax(true, addr, size, newsize);
}

bool _pa_os_commit_ex(void* addr, size_t size, bool* is_zero, size_t stat_size) {
  if (is_zero != NULL) { *is_zero = false; }
  pa_os_stat_counter_increase(commit_calls, 1);

  // page align range
  size_t csize;
  void* start = pa_os_page_align_areax(false /* conservative? */, addr, size, &csize);
  if (csize == 0) return true;

  // commit
  bool os_is_zero = false;
  int err = _pa_prim_commit(start, csize, &os_is_zero);
  if (err != 0) {
    _pa_warning_message("cannot commit OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
    return false;
  }
  if (os_is_zero && is_zero != NULL) {
    *is_zero = true;
    pa_assert_expensive(pa_mem_is_zero(start, csize));
  }
  // note: the following seems required for asan (otherwise `palloc-test-stress` fails)
  #ifdef PA_TRACK_ASAN
  if (os_is_zero) { pa_track_mem_defined(start,csize); }
             else { pa_track_mem_undefined(start,csize); }
  #endif
  pa_os_stat_increase(committed, stat_size);  // use size for precise commit vs. decommit
  return true;
}

bool _pa_os_commit(void* addr, size_t size, bool* is_zero) {
  return _pa_os_commit_ex(addr, size, is_zero, size);
}

static bool pa_os_decommit_ex(void* addr, size_t size, bool* needs_recommit, size_t stat_size) {
  pa_assert_internal(needs_recommit!=NULL);
  pa_os_stat_decrease(committed, stat_size);

  // page align
  size_t csize;
  void* start = pa_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return true;

  // decommit
  *needs_recommit = true;
  int err = _pa_prim_decommit(start,csize,needs_recommit);
  if (err != 0) {
    _pa_warning_message("cannot decommit OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
  }
  pa_assert_internal(err == 0);
  return (err == 0);
}

bool _pa_os_decommit(void* addr, size_t size) {
  bool needs_recommit;
  return pa_os_decommit_ex(addr, size, &needs_recommit, size);
}


// Signal to the OS that the address range is no longer in use
// but may be used later again. This will release physical memory
// pages and reduce swapping while keeping the memory committed.
// We page align to a conservative area inside the range to reset.
bool _pa_os_reset(void* addr, size_t size) {
  // page align conservatively within the range
  size_t csize;
  void* start = pa_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return true;  // || _pa_os_is_huge_reserved(addr)
  pa_os_stat_counter_increase(reset, csize);
  pa_os_stat_counter_increase(reset_calls, 1);

  #if (PA_DEBUG>1) && !PA_SECURE && !PA_TRACK_ENABLED // && !PA_TSAN
  memset(start, 0, csize); // pretend it is eagerly reset
  #endif

  int err = _pa_prim_reset(start, csize);
  if (err != 0) {
    _pa_warning_message("cannot reset OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
  }
  return (err == 0);
}


void _pa_os_reuse( void* addr, size_t size ) {
  // page align conservatively within the range
  size_t csize = 0;
  void* const start = pa_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return;
  const int err = _pa_prim_reuse(start, csize);
  if (err != 0) {
    _pa_warning_message("cannot reuse OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
  }
}

// either resets or decommits memory, returns true if the memory needs
// to be recommitted if it is to be re-used later on.
bool _pa_os_purge_ex(void* p, size_t size, bool allow_reset, size_t stat_size)
{
  if (pa_option_get(pa_option_purge_delay) < 0) return false;  // is purging allowed?
  pa_os_stat_counter_increase(purge_calls, 1);
  pa_os_stat_counter_increase(purged, size);

  if (pa_option_is_enabled(pa_option_purge_decommits) &&   // should decommit?
      !_pa_preloading())                                   // don't decommit during preloading (unsafe)
  {
    bool needs_recommit = true;
    pa_os_decommit_ex(p, size, &needs_recommit, stat_size);
    return needs_recommit;
  }
  else {
    if (allow_reset) {  // this can sometimes be not allowed if the range is not fully committed
      _pa_os_reset(p, size);
    }
    return false;  // needs no recommit
  }
}

// either resets or decommits memory, returns true if the memory needs
// to be recommitted if it is to be re-used later on.
bool _pa_os_purge(void* p, size_t size) {
  return _pa_os_purge_ex(p, size, true, size);
}

// Protect a region in memory to be not accessible.
static  bool pa_os_protectx(void* addr, size_t size, bool protect) {
  // page align conservatively within the range
  size_t csize = 0;
  void* start = pa_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return false;
  /*
  if (_pa_os_is_huge_reserved(addr)) {
	  _pa_warning_message("cannot mprotect memory allocated in huge OS pages\n");
  }
  */
  int err = _pa_prim_protect(start,csize,protect);
  if (err != 0) {
    _pa_warning_message("cannot %s OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", (protect ? "protect" : "unprotect"), err, err, start, csize);
  }
  return (err == 0);
}

bool _pa_os_protect(void* addr, size_t size) {
  return pa_os_protectx(addr, size, true);
}

bool _pa_os_unprotect(void* addr, size_t size) {
  return pa_os_protectx(addr, size, false);
}



/* ----------------------------------------------------------------------------
Support for allocating huge OS pages (1Gib) that are reserved up-front
and possibly associated with a specific NUMA node. (use `numa_node>=0`)
-----------------------------------------------------------------------------*/
#define PA_HUGE_OS_PAGE_SIZE  (PA_GiB)


#if (PA_INTPTR_SIZE >= 8)
// To ensure proper alignment, use our own area for huge OS pages
static pa_decl_cache_align _Atomic(uintptr_t)  pa_huge_start; // = 0

// Claim an aligned address range for huge pages
static uint8_t* pa_os_claim_huge_pages(size_t pages, size_t* total_size) {
  if (total_size != NULL) *total_size = 0;
  const size_t size = pages * PA_HUGE_OS_PAGE_SIZE;

  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t huge_start = pa_atomic_load_relaxed(&pa_huge_start);
  do {
    start = huge_start;
    if (start == 0) {
      // Initialize the start address after the 32TiB area
      start = ((uintptr_t)32 << 40);  // 32TiB virtual start address
    #if (PA_SECURE>0 || PA_DEBUG==0)      // security: randomize start of huge pages unless in debug mode
      uintptr_t r = _pa_heap_random_next(pa_prim_get_default_heap());
      start = start + ((uintptr_t)PA_HUGE_OS_PAGE_SIZE * ((r>>17) & 0x0FFF));  // (randomly 12bits)*1GiB == between 0 to 4TiB
    #endif
    }
    end = start + size;
    pa_assert_internal(end % PA_SEGMENT_SIZE == 0);
  } while (!pa_atomic_cas_strong_acq_rel(&pa_huge_start, &huge_start, end));

  if (total_size != NULL) *total_size = size;
  return (uint8_t*)start;
}
#else
static uint8_t* pa_os_claim_huge_pages(size_t pages, size_t* total_size) {
  PA_UNUSED(pages);
  if (total_size != NULL) *total_size = 0;
  return NULL;
}
#endif

// Allocate PA_SEGMENT_SIZE aligned huge pages
void* _pa_os_alloc_huge_os_pages(size_t pages, int numa_node, pa_msecs_t max_msecs, size_t* pages_reserved, size_t* psize, pa_memid_t* memid) {
  *memid = _pa_memid_none();
  if (psize != NULL) *psize = 0;
  if (pages_reserved != NULL) *pages_reserved = 0;
  size_t size = 0;
  uint8_t* const start = pa_os_claim_huge_pages(pages, &size);
  if (start == NULL) return NULL; // or 32-bit systems

  // Allocate one page at the time but try to place them contiguously
  // We allocate one page at the time to be able to abort if it takes too long
  // or to at least allocate as many as available on the system.
  pa_msecs_t start_t = _pa_clock_start();
  size_t page = 0;
  bool all_zero = true;
  while (page < pages) {
    // allocate a page
    bool is_zero = false;
    void* addr = start + (page * PA_HUGE_OS_PAGE_SIZE);
    void* p = NULL;
    int err = _pa_prim_alloc_huge_os_pages(addr, PA_HUGE_OS_PAGE_SIZE, numa_node, &is_zero, &p);
    if (!is_zero) { all_zero = false;  }
    if (err != 0) {
      _pa_warning_message("unable to allocate huge OS page (error: %d (0x%x), address: %p, size: %zx bytes)\n", err, err, addr, PA_HUGE_OS_PAGE_SIZE);
      break;
    }

    // Did we succeed at a contiguous address?
    if (p != addr) {
      // no success, issue a warning and break
      if (p != NULL) {
        _pa_warning_message("could not allocate contiguous huge OS page %zu at %p\n", page, addr);
        pa_os_prim_free(p, PA_HUGE_OS_PAGE_SIZE, PA_HUGE_OS_PAGE_SIZE);
      }
      break;
    }

    // success, record it
    page++;  // increase before timeout check (see issue #711)
    pa_os_stat_increase(committed, PA_HUGE_OS_PAGE_SIZE);
    pa_os_stat_increase(reserved, PA_HUGE_OS_PAGE_SIZE);

    // check for timeout
    if (max_msecs > 0) {
      pa_msecs_t elapsed = _pa_clock_end(start_t);
      if (page >= 1) {
        pa_msecs_t estimate = ((elapsed / (page+1)) * pages);
        if (estimate > 2*max_msecs) { // seems like we are going to timeout, break
          elapsed = max_msecs + 1;
        }
      }
      if (elapsed > max_msecs) {
        _pa_warning_message("huge OS page allocation timed out (after allocating %zu page(s))\n", page);
        break;
      }
    }
  }
  pa_assert_internal(page*PA_HUGE_OS_PAGE_SIZE <= size);
  if (pages_reserved != NULL) { *pages_reserved = page; }
  if (psize != NULL) { *psize = page * PA_HUGE_OS_PAGE_SIZE; }
  if (page != 0) {
    pa_assert(start != NULL);
    *memid = _pa_memid_create_os(start, size, true /* is committed */, all_zero, true /* is_large */);
    memid->memkind = PA_MEM_OS_HUGE;
    pa_assert(memid->is_pinned);
    #ifdef PA_TRACK_ASAN
    if (all_zero) { pa_track_mem_defined(start,size); }
    #endif
  }
  return (page == 0 ? NULL : start);
}

// free every huge page in a range individually (as we allocated per page)
// note: needed with VirtualAlloc but could potentially be done in one go on mmap'd systems.
static void pa_os_free_huge_os_pages(void* p, size_t size) {
  if (p==NULL || size==0) return;
  uint8_t* base = (uint8_t*)p;
  while (size >= PA_HUGE_OS_PAGE_SIZE) {
    pa_os_prim_free(base, PA_HUGE_OS_PAGE_SIZE, PA_HUGE_OS_PAGE_SIZE);
    size -= PA_HUGE_OS_PAGE_SIZE;
    base += PA_HUGE_OS_PAGE_SIZE;
  }
}


/* ----------------------------------------------------------------------------
Support NUMA aware allocation
-----------------------------------------------------------------------------*/

static _Atomic(size_t) pa_numa_node_count; // = 0   // cache the node count

int _pa_os_numa_node_count(void) {
  size_t count = pa_atomic_load_acquire(&pa_numa_node_count);
  if pa_unlikely(count == 0) {
    long ncount = pa_option_get(pa_option_use_numa_nodes); // given explicitly?
    if (ncount > 0 && ncount < INT_MAX) {
      count = (size_t)ncount;
    }
    else {
      const size_t n = _pa_prim_numa_node_count(); // or detect dynamically
      if (n == 0 || n > INT_MAX) { count = 1; }
                            else { count = n; }
    }
    pa_atomic_store_release(&pa_numa_node_count, count); // save it
    _pa_verbose_message("using %zd numa regions\n", count);
  }
  pa_assert_internal(count > 0 && count <= INT_MAX);
  return (int)count;
}

static int pa_os_numa_node_get(void) {
  int numa_count = _pa_os_numa_node_count();
  if (numa_count<=1) return 0; // optimize on single numa node systems: always node 0
  // never more than the node count and >= 0
  const size_t n = _pa_prim_numa_node();
  int numa_node = (n < INT_MAX ? (int)n : 0);
  if (numa_node >= numa_count) { numa_node = numa_node % numa_count; }
  return numa_node;
}

int _pa_os_numa_node(void) {
  if pa_likely(pa_atomic_load_relaxed(&pa_numa_node_count) == 1) {
    return 0;
  }
  else {
    return pa_os_numa_node_get();
  }
}
