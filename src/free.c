/* ----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#if !defined(PA_IN_ALLOC_C)
#error "this file should be included from 'alloc.c' (so aliases can work from alloc-override)"
// add includes help an IDE
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/prim.h"   // _pa_prim_thread_id()
#endif

// forward declarations
static void   pa_check_padding(const pa_page_t* page, const pa_block_t* block);
static bool   pa_check_is_double_free(const pa_page_t* page, const pa_block_t* block);
static size_t pa_page_usable_size_of(const pa_page_t* page, const pa_block_t* block);
static void   pa_stat_free(const pa_page_t* page, const pa_block_t* block);


// ------------------------------------------------------
// Free
// ------------------------------------------------------

// forward declaration of multi-threaded free (`_mt`) (or free in huge block if compiled with PA_HUGE_PAGE_ABANDON)
static pa_decl_noinline void pa_free_block_mt(pa_page_t* page, pa_segment_t* segment, pa_block_t* block);

// regular free of a (thread local) block pointer
// fast path written carefully to prevent spilling on the stack
static inline void pa_free_block_local(pa_page_t* page, pa_block_t* block, bool track_stats, bool check_full)
{
  // checks
  if pa_unlikely(pa_check_is_double_free(page, block)) return;
  pa_check_padding(page, block);
  if (track_stats) { pa_stat_free(page, block); }
  #if (PA_DEBUG>0) && !PA_TRACK_ENABLED  && !PA_TSAN && !PA_GUARDED
  if (!pa_page_is_huge(page)) {   // huge page content may be already decommitted
    memset(block, PA_DEBUG_FREED, pa_page_block_size(page));
  }
  #endif
  if (track_stats) { pa_track_free_size(block, pa_page_usable_size_of(page, block)); } // faster then pa_usable_size as we already know the page and that p is unaligned

  // Thread cache (tcmalloc-style): push to per-bin cache when under limit, else to page.
  // Only cache when page->used > 1 so the last block goes to local_free; we flush this page from cache before retire.
  pa_heap_t* const heap = pa_page_heap(page);
  const size_t bsize = pa_page_block_size(page);
  if (heap != NULL) {
    const size_t usable = pa_page_usable_block_size(page);
    if (heap->used_bytes >= usable) heap->used_bytes -= usable;
    else heap->used_bytes = 0;
  }
  const size_t bin = _pa_bin(bsize);
  if (bin < PA_CACHE_BINS && heap != NULL && page->used > 1 && heap->cache_count[bin] < PA_CACHE_MAX_PER_BIN) {
    pa_block_set_nextx(heap, block, heap->cache_head[bin], heap->keys);
    heap->cache_head[bin] = block;
    heap->cache_count[bin]++;
  } else {
    pa_block_set_next(page, block, page->local_free);
    page->local_free = block;
  }
  if pa_unlikely(--page->used == 0) {
    // Flush any blocks from this page that are still in the thread cache so we don't retire with dangling cache entries
    if (bin < PA_CACHE_BINS && heap->cache_count[bin] > 0)
      pa_heap_cache_flush_page(heap, page);
    _pa_page_free_collect(page, false);
    _pa_page_retire(page);
  }
  else if pa_unlikely(check_full && pa_page_is_in_full(page)) {
    _pa_page_unfull(page);
  }
}

// Adjust a block that was allocated aligned, to the actual start of the block in the page.
// note: this can be called from `pa_free_generic_mt` where a non-owning thread accesses the
// `page_start` and `block_size` fields; however these are constant and the page won't be
// deallocated (as the block we are freeing keeps it alive) and thus safe to read concurrently.
pa_block_t* _pa_page_ptr_unalign(const pa_page_t* page, const void* p) {
  pa_assert_internal(page!=NULL && p!=NULL);

  size_t diff = (uint8_t*)p - page->page_start;
  size_t adjust;
  if pa_likely(page->block_size_shift != 0) {
    adjust = diff & (((size_t)1 << page->block_size_shift) - 1);
  }
  else {
    adjust = diff % pa_page_block_size(page);
  }

  return (pa_block_t*)((uintptr_t)p - adjust);
}

// forward declaration for a PA_GUARDED build
#if PA_GUARDED
static void pa_block_unguard(pa_page_t* page, pa_block_t* block, void* p); // forward declaration
static inline void pa_block_check_unguard(pa_page_t* page, pa_block_t* block, void* p) {
  if (pa_block_ptr_is_guarded(block, p)) { pa_block_unguard(page, block, p); }
}
#else
static inline void pa_block_check_unguard(pa_page_t* page, pa_block_t* block, void* p) {
  PA_UNUSED(page); PA_UNUSED(block); PA_UNUSED(p);
}
#endif

// free a local pointer  (page parameter comes first for better codegen)
static void pa_decl_noinline pa_free_generic_local(pa_page_t* page, pa_segment_t* segment, void* p) pa_attr_noexcept {
  PA_UNUSED(segment);
  pa_block_t* const block = (pa_page_has_aligned(page) ? _pa_page_ptr_unalign(page, p) : (pa_block_t*)p);
  pa_block_check_unguard(page, block, p);
  pa_free_block_local(page, block, true /* track stats */, true /* check for a full page */);
}

// free a pointer owned by another thread (page parameter comes first for better codegen)
static void pa_decl_noinline pa_free_generic_mt(pa_page_t* page, pa_segment_t* segment, void* p) pa_attr_noexcept {
  pa_block_t* const block = _pa_page_ptr_unalign(page, p); // don't check `has_aligned` flag to avoid a race (issue #865)
  pa_block_check_unguard(page, block, p);
  pa_free_block_mt(page, segment, block);
}

// generic free (for runtime integration)
void pa_decl_noinline _pa_free_generic(pa_segment_t* segment, pa_page_t* page, bool is_local, void* p) pa_attr_noexcept {
  if (is_local) pa_free_generic_local(page,segment,p);
           else pa_free_generic_mt(page,segment,p);
}

// Get the segment data belonging to a pointer
// This is just a single `and` in release mode but does further checks in debug mode
// (and secure mode) to see if this was a valid pointer.
static inline pa_segment_t* pa_checked_ptr_segment(const void* p, const char* msg)
{
  PA_UNUSED(msg);

  #if (PA_DEBUG>0)
  if pa_unlikely(((uintptr_t)p & (PA_INTPTR_SIZE - 1)) != 0 && !pa_option_is_enabled(pa_option_guarded_precise)) {
    _pa_error_message(EINVAL, "%s: invalid (unaligned) pointer: %p\n", msg, p);
    return NULL;
  }
  #endif

  pa_segment_t* const segment = _pa_ptr_segment(p);
  if pa_unlikely(segment==NULL) return segment;

  #if (PA_DEBUG>0)
  if pa_unlikely(!pa_is_in_heap_region(p)) {
  #if (PA_INTPTR_SIZE == 8 && defined(__linux__))
    if (((uintptr_t)p >> 40) != 0x7F) { // linux tends to align large blocks above 0x7F000000000 (issue #640)
  #else
    {
  #endif
      _pa_warning_message("%s: pointer might not point to a valid heap region: %p\n"
        "(this may still be a valid very large allocation (over 64MiB))\n", msg, p);
      if pa_likely(_pa_ptr_cookie(segment) == segment->cookie) {
        _pa_warning_message("(yes, the previous pointer %p was valid after all)\n", p);
      }
    }
  }
  #endif
  #if (PA_DEBUG>0 || PA_SECURE>=4)
  if pa_unlikely(_pa_ptr_cookie(segment) != segment->cookie) {
    _pa_error_message(EINVAL, "%s: pointer does not point to a valid heap space: %p\n", msg, p);
    return NULL;
  }
  #endif

  return segment;
}

// Free a block
// Fast path written carefully to prevent register spilling on the stack
static inline void pa_free_ex(void* p, size_t* usable) pa_attr_noexcept
{
  pa_segment_t* const segment = pa_checked_ptr_segment(p,"pa_free");
  if pa_unlikely(segment==NULL) return;

  const bool is_local = (_pa_prim_thread_id() == pa_atomic_load_relaxed(&segment->thread_id));
  pa_page_t* const page = _pa_segment_page_of(segment, p);
  pa_prefetch(page);
  if (usable!=NULL) { *usable = pa_page_usable_block_size(page); }
  
  if pa_likely(is_local) {                        // thread-local free?
    if pa_likely(page->flags.full_aligned == 0) { // and it is not a full page (full pages need to move from the full bin), nor has aligned blocks (aligned blocks need to be unaligned)
      // thread-local, aligned, and not a full page
      pa_block_t* const block = (pa_block_t*)p;
      pa_free_block_local(page, block, true /* track stats */, false /* no need to check if the page is full */);
    }
    else {
      // page is full or contains (inner) aligned blocks; use generic path
      pa_free_generic_local(page, segment, p);
    }
  }
  else {
    // not thread-local; use generic path
    pa_free_generic_mt(page, segment, p);
  }
}

void pa_free(void* p) pa_attr_noexcept {
  pa_free_ex(p,NULL);
}

void pa_ufree(void* p, size_t* usable) pa_attr_noexcept {
  pa_free_ex(p,usable);
}

// return true if successful
bool _pa_free_delayed_block(pa_block_t* block) {
  // get segment and page
  pa_assert_internal(block!=NULL);
  const pa_segment_t* const segment = _pa_ptr_segment(block);
  pa_assert_internal(_pa_ptr_cookie(segment) == segment->cookie);
  pa_assert_internal(_pa_thread_id() == segment->thread_id);
  pa_page_t* const page = _pa_segment_page_of(segment, block);

  // Clear the no-delayed flag so delayed freeing is used again for this page.
  // This must be done before collecting the free lists on this page -- otherwise
  // some blocks may end up in the page `thread_free` list with no blocks in the
  // heap `thread_delayed_free` list which may cause the page to be never freed!
  // (it would only be freed if we happen to scan it in `pa_page_queue_find_free_ex`)
  if (!_pa_page_try_use_delayed_free(page, PA_USE_DELAYED_FREE, false /* dont overwrite never delayed */)) {
    return false;
  }

  // collect all other non-local frees (move from `thread_free` to `free`) to ensure up-to-date `used` count
  _pa_page_free_collect(page, false);

  // and free the block (possibly freeing the page as well since `used` is updated)
  pa_free_block_local(page, block, false /* stats have already been adjusted */, true /* check for a full page */);
  return true;
}

// ------------------------------------------------------
// Multi-threaded Free (`_mt`)
// ------------------------------------------------------

// Push a block that is owned by another thread on its page-local thread free
// list or it's heap delayed free list. Such blocks are later collected by
// the owning thread in `_pa_free_delayed_block`.
static void pa_decl_noinline pa_free_block_delayed_mt( pa_page_t* page, pa_block_t* block )
{
  // Try to put the block on either the page-local thread free list,
  // or the heap delayed free list (if this is the first non-local free in that page)
  pa_thread_free_t tfreex;
  bool use_delayed;
  pa_thread_free_t tfree = pa_atomic_load_relaxed(&page->xthread_free);
  do {
    use_delayed = (pa_tf_delayed(tfree) == PA_USE_DELAYED_FREE);
    if pa_unlikely(use_delayed) {
      // unlikely: this only happens on the first concurrent free in a page that is in the full list
      tfreex = pa_tf_set_delayed(tfree,PA_DELAYED_FREEING);
    }
    else {
      // usual: directly add to page thread_free list
      pa_block_set_next(page, block, pa_tf_block(tfree));
      tfreex = pa_tf_set_block(tfree,block);
    }
  } while (!pa_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));

  // If this was the first non-local free, we need to push it on the heap delayed free list instead
  if pa_unlikely(use_delayed) {
    // racy read on `heap`, but ok because PA_DELAYED_FREEING is set (see `pa_heap_delete` and `pa_heap_collect_abandon`)
    pa_heap_t* const heap = (pa_heap_t*)(pa_atomic_load_acquire(&page->xheap)); //pa_page_heap(page);
    pa_assert_internal(heap != NULL);
    if (heap != NULL) {
      // add to the delayed free list of this heap. (do this atomically as the lock only protects heap memory validity)
      pa_block_t* dfree = pa_atomic_load_ptr_relaxed(pa_block_t, &heap->thread_delayed_free);
      do {
        pa_block_set_nextx(heap,block,dfree, heap->keys);
      } while (!pa_atomic_cas_ptr_weak_release(pa_block_t,&heap->thread_delayed_free, &dfree, block));
    }

    // and reset the PA_DELAYED_FREEING flag
    tfree = pa_atomic_load_relaxed(&page->xthread_free);
    do {
      tfreex = tfree;
      pa_assert_internal(pa_tf_delayed(tfree) == PA_DELAYED_FREEING);
      tfreex = pa_tf_set_delayed(tfree,PA_NO_DELAYED_FREE);
    } while (!pa_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));
  }
}

// Multi-threaded free (`_mt`) (or free in huge block if compiled with PA_HUGE_PAGE_ABANDON)
static void pa_decl_noinline pa_free_block_mt(pa_page_t* page, pa_segment_t* segment, pa_block_t* block)
{
  // first see if the segment was abandoned and if we can reclaim it into our thread
  if (_pa_option_get_fast(pa_option_abandoned_reclaim_on_free) != 0 &&
      #if PA_HUGE_PAGE_ABANDON
      segment->page_kind != PA_PAGE_HUGE &&
      #endif
      pa_atomic_load_relaxed(&segment->thread_id) == 0 &&  // segment is abandoned?
      pa_prim_get_default_heap() != (pa_heap_t*)&_pa_heap_empty) // and we did not already exit this thread (without this check, a fresh heap will be initalized (issue #944))
  {
    // the segment is abandoned, try to reclaim it into our heap
    if (_pa_segment_attempt_reclaim(pa_heap_get_default(), segment)) {
      pa_assert_internal(_pa_thread_id() == pa_atomic_load_relaxed(&segment->thread_id));
      pa_assert_internal(pa_heap_get_default()->tld->segments.subproc == segment->subproc);
      pa_free(block);  // recursively free as now it will be a local free in our heap
      return;
    }
  }

  // The padding check may access the non-thread-owned page for the key values.
  // that is safe as these are constant and the page won't be freed (as the block is not freed yet).
  pa_check_padding(page, block);

  // adjust stats (after padding check and potentially recursive `pa_free` above)
  pa_stat_free(page, block);    // stat_free may access the padding
  pa_track_free_size(block, pa_page_usable_size_of(page,block));

  // for small size, ensure we can fit the delayed thread pointers without triggering overflow detection
  _pa_padding_shrink(page, block, sizeof(pa_block_t));

  if (segment->kind == PA_SEGMENT_HUGE) {
    #if PA_HUGE_PAGE_ABANDON
    // huge page segments are always abandoned and can be freed immediately
    _pa_segment_huge_page_free(segment, page, block);
    return;
    #else
    // huge pages are special as they occupy the entire segment
    // as these are large we reset the memory occupied by the page so it is available to other threads
    // (as the owning thread needs to actually free the memory later).
    _pa_segment_huge_page_reset(segment, page, block);
    #endif
  }
  else {
    #if (PA_DEBUG>0) && !PA_TRACK_ENABLED  && !PA_TSAN       // note: when tracking, cannot use pa_usable_size with multi-threading
    memset(block, PA_DEBUG_FREED, pa_usable_size(block));
    #endif
  }

  // and finally free the actual block by pushing it on the owning heap
  // thread_delayed free list (or heap delayed free list)
  pa_free_block_delayed_mt(page,block);
}


// ------------------------------------------------------
// Usable size
// ------------------------------------------------------

// Bytes available in a block
static size_t pa_decl_noinline pa_page_usable_aligned_size_of(const pa_page_t* page, const void* p) pa_attr_noexcept {
  const pa_block_t* block = _pa_page_ptr_unalign(page, p);
  const size_t size = pa_page_usable_size_of(page, block);
  const ptrdiff_t adjust = (uint8_t*)p - (uint8_t*)block;
  pa_assert_internal(adjust >= 0 && (size_t)adjust <= size);
  const size_t aligned_size = (size - adjust);
  #if PA_GUARDED
  if (pa_block_ptr_is_guarded(block, p)) {
    return aligned_size - _pa_os_page_size();
  }
  #endif
  return aligned_size;
}

static inline pa_page_t* pa_validate_ptr_page(const void* p, const char* msg) {
  const pa_segment_t* const segment = pa_checked_ptr_segment(p, msg);
  if pa_unlikely(segment==NULL) return NULL;
  pa_page_t* const page = _pa_segment_page_of(segment, p);
  return page;
}

static inline size_t _pa_usable_size(const void* p, const pa_page_t* page) pa_attr_noexcept {
  if pa_unlikely(page==NULL) return 0;
  if pa_likely(!pa_page_has_aligned(page)) {
    const pa_block_t* block = (const pa_block_t*)p;
    return pa_page_usable_size_of(page, block);
  }
  else {
    // split out to separate routine for improved code generation
    return pa_page_usable_aligned_size_of(page, p);
  }
}

pa_decl_nodiscard size_t pa_usable_size(const void* p) pa_attr_noexcept {
  const pa_page_t* const page = pa_validate_ptr_page(p,"pa_usable_size");
  return _pa_usable_size(p,page);
}


// ------------------------------------------------------
// Free variants
// ------------------------------------------------------

void pa_free_size(void* p, size_t size) pa_attr_noexcept {
  PA_UNUSED_RELEASE(size);
  #if PA_DEBUG
  const pa_page_t* const page = pa_validate_ptr_page(p,"pa_free_size");  
  const size_t available = _pa_usable_size(p,page);
  pa_assert(p == NULL || size <= available || available == 0 /* invalid pointer */ );
  #endif
  pa_free(p);
}

void pa_free_size_aligned(void* p, size_t size, size_t alignment) pa_attr_noexcept {
  PA_UNUSED_RELEASE(alignment);
  pa_assert(((uintptr_t)p % alignment) == 0);
  pa_free_size(p,size);
}

void pa_free_aligned(void* p, size_t alignment) pa_attr_noexcept {
  PA_UNUSED_RELEASE(alignment);
  pa_assert(((uintptr_t)p % alignment) == 0);
  pa_free(p);
}


// ------------------------------------------------------
// Check for double free in secure and debug mode
// This is somewhat expensive so only enabled for secure mode 4
// ------------------------------------------------------

#if (PA_ENCODE_FREELIST && (PA_SECURE>=4 || PA_DEBUG!=0))
// linear check if the free list contains a specific element
static bool pa_list_contains(const pa_page_t* page, const pa_block_t* list, const pa_block_t* elem) {
  while (list != NULL) {
    if (elem==list) return true;
    list = pa_block_next(page, list);
  }
  return false;
}

static pa_decl_noinline bool pa_check_is_double_freex(const pa_page_t* page, const pa_block_t* block) {
  // The decoded value is in the same page (or NULL).
  // Walk the free lists to verify positively if it is already freed
  if (pa_list_contains(page, page->free, block) ||
      pa_list_contains(page, page->local_free, block) ||
      pa_list_contains(page, pa_page_thread_free(page), block))
  {
    _pa_error_message(EAGAIN, "double free detected of block %p with size %zu\n", block, pa_page_block_size(page));
    return true;
  }
  return false;
}

#define pa_track_page(page,access)  { size_t psize; void* pstart = _pa_page_start(_pa_page_segment(page),page,&psize); pa_track_mem_##access( pstart, psize); }

static inline bool pa_check_is_double_free(const pa_page_t* page, const pa_block_t* block) {
  bool is_double_free = false;
  pa_block_t* n = pa_block_nextx(page, block, page->keys); // pretend it is freed, and get the decoded first field
  if (((uintptr_t)n & (PA_INTPTR_SIZE-1))==0 &&  // quick check: aligned pointer?
      (n==NULL || pa_is_in_same_page(block, n))) // quick check: in same page or NULL?
  {
    // Suspicious: decoded value a in block is in the same page (or NULL) -- maybe a double free?
    // (continue in separate function to improve code generation)
    is_double_free = pa_check_is_double_freex(page, block);
  }
  return is_double_free;
}
#else
static inline bool pa_check_is_double_free(const pa_page_t* page, const pa_block_t* block) {
  PA_UNUSED(page);
  PA_UNUSED(block);
  return false;
}
#endif


// ---------------------------------------------------------------------------
// Check for heap block overflow by setting up padding at the end of the block
// ---------------------------------------------------------------------------

#if PA_PADDING // && !PA_TRACK_ENABLED
static bool pa_page_decode_padding(const pa_page_t* page, const pa_block_t* block, size_t* delta, size_t* bsize) {
  *bsize = pa_page_usable_block_size(page);
  const pa_padding_t* const padding = (pa_padding_t*)((uint8_t*)block + *bsize);
  pa_track_mem_defined(padding,sizeof(pa_padding_t));
  *delta = padding->delta;
  uint32_t canary = padding->canary;
  uintptr_t keys[2];
  keys[0] = page->keys[0];
  keys[1] = page->keys[1];
  bool ok = (pa_ptr_encode_canary(page,block,keys) == canary && *delta <= *bsize);
  pa_track_mem_noaccess(padding,sizeof(pa_padding_t));
  return ok;
}

// Return the exact usable size of a block.
static size_t pa_page_usable_size_of(const pa_page_t* page, const pa_block_t* block) {
  size_t bsize;
  size_t delta;
  bool ok = pa_page_decode_padding(page, block, &delta, &bsize);
  pa_assert_internal(ok); pa_assert_internal(delta <= bsize);
  return (ok ? bsize - delta : 0);
}

// When a non-thread-local block is freed, it becomes part of the thread delayed free
// list that is freed later by the owning heap. If the exact usable size is too small to
// contain the pointer for the delayed list, then shrink the padding (by decreasing delta)
// so it will later not trigger an overflow error in `pa_free_block`.
void _pa_padding_shrink(const pa_page_t* page, const pa_block_t* block, const size_t min_size) {
  size_t bsize;
  size_t delta;
  bool ok = pa_page_decode_padding(page, block, &delta, &bsize);
  pa_assert_internal(ok);
  if (!ok || (bsize - delta) >= min_size) return;  // usually already enough space
  pa_assert_internal(bsize >= min_size);
  if (bsize < min_size) return;  // should never happen
  size_t new_delta = (bsize - min_size);
  pa_assert_internal(new_delta < bsize);
  pa_padding_t* padding = (pa_padding_t*)((uint8_t*)block + bsize);
  pa_track_mem_defined(padding,sizeof(pa_padding_t));
  padding->delta = (uint32_t)new_delta;
  pa_track_mem_noaccess(padding,sizeof(pa_padding_t));
}
#else
static size_t pa_page_usable_size_of(const pa_page_t* page, const pa_block_t* block) {
  PA_UNUSED(block);
  return pa_page_usable_block_size(page);
}

void _pa_padding_shrink(const pa_page_t* page, const pa_block_t* block, const size_t min_size) {
  PA_UNUSED(page);
  PA_UNUSED(block);
  PA_UNUSED(min_size);
}
#endif

#if PA_PADDING && PA_PADDING_CHECK

static bool pa_verify_padding(const pa_page_t* page, const pa_block_t* block, size_t* size, size_t* wrong) {
  size_t bsize;
  size_t delta;
  bool ok = pa_page_decode_padding(page, block, &delta, &bsize);
  *size = *wrong = bsize;
  if (!ok) return false;
  pa_assert_internal(bsize >= delta);
  *size = bsize - delta;
  if (!pa_page_is_huge(page)) {
    uint8_t* fill = (uint8_t*)block + bsize - delta;
    const size_t maxpad = (delta > PA_MAX_ALIGN_SIZE ? PA_MAX_ALIGN_SIZE : delta); // check at most the first N padding bytes
    pa_track_mem_defined(fill, maxpad);
    for (size_t i = 0; i < maxpad; i++) {
      if (fill[i] != PA_DEBUG_PADDING) {
        *wrong = bsize - delta + i;
        ok = false;
        break;
      }
    }
    pa_track_mem_noaccess(fill, maxpad);
  }
  return ok;
}

static void pa_check_padding(const pa_page_t* page, const pa_block_t* block) {
  size_t size;
  size_t wrong;
  if (!pa_verify_padding(page,block,&size,&wrong)) {
    _pa_error_message(EFAULT, "buffer overflow in heap block %p of size %zu: write after %zu bytes\n", block, size, wrong );
  }
}

#else

static void pa_check_padding(const pa_page_t* page, const pa_block_t* block) {
  PA_UNUSED(page);
  PA_UNUSED(block);
}

#endif

// only maintain stats for smaller objects if requested
#if (PA_STAT>0)
static void pa_stat_free(const pa_page_t* page, const pa_block_t* block) {
  PA_UNUSED(block);
  pa_heap_t* const heap = pa_heap_get_default();
  const size_t bsize = pa_page_usable_block_size(page);
  // #if (PA_STAT>1)
  // const size_t usize = pa_page_usable_size_of(page, block);
  // pa_heap_stat_decrease(heap, malloc_requested, usize);
  // #endif
  if (bsize <= PA_MEDIUM_OBJ_SIZE_MAX) {
    pa_heap_stat_decrease(heap, malloc_normal, bsize);
    #if (PA_STAT > 1)
    pa_heap_stat_decrease(heap, malloc_bins[_pa_bin(bsize)], 1);
    #endif
  }
  //else if (bsize <= PA_LARGE_OBJ_SIZE_MAX) {
  //  pa_heap_stat_decrease(heap, malloc_large, bsize);
  //}
  else {
    pa_heap_stat_decrease(heap, malloc_huge, bsize);
  }
}
#else
static void pa_stat_free(const pa_page_t* page, const pa_block_t* block) {
  PA_UNUSED(page); PA_UNUSED(block);
}
#endif


// Remove guard page when building with PA_GUARDED
#if PA_GUARDED
static void pa_block_unguard(pa_page_t* page, pa_block_t* block, void* p) {
  PA_UNUSED(p);
  pa_assert_internal(pa_block_ptr_is_guarded(block, p));
  pa_assert_internal(pa_page_has_aligned(page));
  pa_assert_internal((uint8_t*)p - (uint8_t*)block >= (ptrdiff_t)sizeof(pa_block_t));
  pa_assert_internal(block->next == PA_BLOCK_TAG_GUARDED);

  const size_t bsize = pa_page_block_size(page);
  const size_t psize = _pa_os_page_size();
  pa_assert_internal(bsize > psize);
  pa_assert_internal(_pa_page_segment(page)->allow_decommit);
  void* gpage = (uint8_t*)block + bsize - psize;
  pa_assert_internal(_pa_is_aligned(gpage, psize));
  _pa_os_unprotect(gpage, psize);
}
#endif
