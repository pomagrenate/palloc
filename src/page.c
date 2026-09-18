/*----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  The core of the allocator. Every segment contains
  pages of a certain block size. The main function
  exported is `pa_malloc_generic`.
----------------------------------------------------------- */

#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/atomic.h"

/* -----------------------------------------------------------
  Definition of page queues for each block size
----------------------------------------------------------- */

#define PA_IN_PAGE_C
#include "page-queue.c"
#undef PA_IN_PAGE_C


/* -----------------------------------------------------------
  Page helpers
----------------------------------------------------------- */

// Index a block in a page
static inline pa_block_t* pa_page_block_at(const pa_page_t* page, void* page_start, size_t block_size, size_t i) {
  PA_UNUSED(page);
  pa_assert_internal(page != NULL);
  pa_assert_internal(i <= page->reserved);
  return (pa_block_t*)((uint8_t*)page_start + (i * block_size));
}

static void pa_page_init(pa_heap_t* heap, pa_page_t* page, size_t size, pa_tld_t* tld);
static bool pa_page_extend_free(pa_heap_t* heap, pa_page_t* page, pa_tld_t* tld);

#if (PA_DEBUG>=3)
static size_t pa_page_list_count(pa_page_t* page, pa_block_t* head) {
  size_t count = 0;
  while (head != NULL) {
    pa_assert_internal(page == _pa_ptr_page(head));
    count++;
    head = pa_block_next(page, head);
  }
  return count;
}

/*
// Start of the page available memory
static inline uint8_t* pa_page_area(const pa_page_t* page) {
  return _pa_page_start(_pa_page_segment(page), page, NULL);
}
*/

static bool pa_page_list_is_valid(pa_page_t* page, pa_block_t* p) {
  size_t psize;
  uint8_t* page_area = _pa_segment_page_start(_pa_page_segment(page), page, &psize);
  pa_block_t* start = (pa_block_t*)page_area;
  pa_block_t* end   = (pa_block_t*)(page_area + psize);
  while(p != NULL) {
    if (p < start || p >= end) return false;
    p = pa_block_next(page, p);
  }
#if PA_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = pa_page_usable_block_size(page);
    for (pa_block_t* block = page->free; block != NULL; block = pa_block_next(page, block)) {
      pa_assert_expensive(pa_mem_is_zero(block + 1, ubsize - sizeof(pa_block_t)));
    }
  }
#endif
  return true;
}

static bool pa_page_is_valid_init(pa_page_t* page) {
  pa_assert_internal(pa_page_block_size(page) > 0);
  pa_assert_internal(page->used <= page->capacity);
  pa_assert_internal(page->capacity <= page->reserved);

  uint8_t* start = pa_page_start(page);
  pa_assert_internal(start == _pa_segment_page_start(_pa_page_segment(page), page, NULL));
  pa_assert_internal(page->is_huge == (_pa_page_segment(page)->kind == PA_SEGMENT_HUGE));
  //pa_assert_internal(start + page->capacity*page->block_size == page->top);

  pa_assert_internal(pa_page_list_is_valid(page,page->free));
  pa_assert_internal(pa_page_list_is_valid(page,page->local_free));

  #if PA_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = pa_page_usable_block_size(page);
    for(pa_block_t* block = page->free; block != NULL; block = pa_block_next(page,block)) {
      pa_assert_expensive(pa_mem_is_zero(block + 1, ubsize - sizeof(pa_block_t)));
    }
  }
  #endif

  #if !PA_TRACK_ENABLED && !PA_TSAN
  pa_block_t* tfree = pa_page_thread_free(page);
  pa_assert_internal(pa_page_list_is_valid(page, tfree));
  //size_t tfree_count = pa_page_list_count(page, tfree);
  //pa_assert_internal(tfree_count <= page->thread_freed + 1);
  #endif

  size_t free_count = pa_page_list_count(page, page->free) + pa_page_list_count(page, page->local_free);
  pa_assert_internal(page->used + free_count == page->capacity);

  return true;
}

extern pa_decl_hidden bool _pa_process_is_initialized;             // has pa_process_init been called?

bool _pa_page_is_valid(pa_page_t* page) {
  pa_assert_internal(pa_page_is_valid_init(page));
  #if PA_SECURE
  pa_assert_internal(page->keys[0] != 0);
  #endif
  if (pa_page_heap(page)!=NULL) {
    pa_segment_t* segment = _pa_page_segment(page);

    pa_assert_internal(!_pa_process_is_initialized || segment->thread_id==0 || segment->thread_id == pa_page_heap(page)->thread_id);
    #if PA_HUGE_PAGE_ABANDON
    if (segment->kind != PA_SEGMENT_HUGE)
    #endif
    {
      pa_page_queue_t* pq = pa_page_queue_of(page);
      pa_assert_internal(pa_page_queue_contains(pq, page));
      pa_assert_internal(pq->block_size==pa_page_block_size(page) || pa_page_block_size(page) > PA_MEDIUM_OBJ_SIZE_MAX || pa_page_is_in_full(page));
      pa_assert_internal(pa_heap_contains_queue(pa_page_heap(page),pq));
    }
  }
  return true;
}
#endif

void _pa_page_use_delayed_free(pa_page_t* page, pa_delayed_t delay, bool override_never) {
  while (!_pa_page_try_use_delayed_free(page, delay, override_never)) {
    pa_atomic_yield();
  }
}

bool _pa_page_try_use_delayed_free(pa_page_t* page, pa_delayed_t delay, bool override_never) {
  pa_thread_free_t tfreex;
  pa_delayed_t     old_delay;
  pa_thread_free_t tfree;
  size_t yield_count = 0;
  do {
    tfree = pa_atomic_load_acquire(&page->xthread_free); // note: must acquire as we can break/repeat this loop and not do a CAS;
    tfreex = pa_tf_set_delayed(tfree, delay);
    old_delay = pa_tf_delayed(tfree);
    if pa_unlikely(old_delay == PA_DELAYED_FREEING) {
      if (yield_count >= 4) return false;  // give up after 4 tries
      yield_count++;
      pa_atomic_yield(); // delay until outstanding PA_DELAYED_FREEING are done.
      // tfree = pa_tf_set_delayed(tfree, PA_NO_DELAYED_FREE); // will cause CAS to busy fail
    }
    else if (delay == old_delay) {
      break; // avoid atomic operation if already equal
    }
    else if (!override_never && old_delay == PA_NEVER_DELAYED_FREE) {
      break; // leave never-delayed flag set
    }
  } while ((old_delay == PA_DELAYED_FREEING) ||
           !pa_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));

  return true; // success
}

/* -----------------------------------------------------------
  Page collect the `local_free` and `thread_free` lists
----------------------------------------------------------- */

// Collect the local `thread_free` list using an atomic exchange.
// Note: The exchange must be done atomically as this is used right after
// moving to the full list in `pa_page_collect_ex` and we need to
// ensure that there was no race where the page became unfull just before the move.
static void _pa_page_thread_free_collect(pa_page_t* page)
{
  pa_block_t* head;
  pa_thread_free_t tfreex;
  pa_thread_free_t tfree = pa_atomic_load_relaxed(&page->xthread_free);
  do {
    head = pa_tf_block(tfree);
    tfreex = pa_tf_set_block(tfree,NULL);
  } while (!pa_atomic_cas_weak_acq_rel(&page->xthread_free, &tfree, tfreex));

  // return if the list is empty
  if (head == NULL) return;

  // find the tail -- also to get a proper count (without data races)
  size_t max_count = page->capacity; // cannot collect more than capacity
  size_t count = 1;
  pa_block_t* tail = head;
  pa_block_t* next;
  while ((next = pa_block_next(page,tail)) != NULL && count <= max_count) {
    count++;
    tail = next;
  }
  // if `count > max_count` there was a memory corruption (possibly infinite list due to double multi-threaded free)
  if (count > max_count) {
    _pa_error_message(EFAULT, "corrupted thread-free list\n");
    return; // the thread-free items cannot be freed
  }

  // and append the current local free list
  pa_block_set_next(page,tail, page->local_free);
  page->local_free = head;

  // update counts now
  page->used -= (uint16_t)count;
}

void _pa_page_free_collect(pa_page_t* page, bool force) {
  pa_assert_internal(page!=NULL);

  // collect the thread free list
  if (force || pa_page_thread_free(page) != NULL) {  // quick test to avoid an atomic operation
    _pa_page_thread_free_collect(page);
  }

  // and the local free list
  if (page->local_free != NULL) {
    if pa_likely(page->free == NULL) {
      // usual case
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
    else if (force) {
      // append -- only on shutdown (force) as this is a linear operation
      pa_block_t* tail = page->local_free;
      pa_block_t* next;
      while ((next = pa_block_next(page, tail)) != NULL) {
        tail = next;
      }
      pa_block_set_next(page, tail, page->free);
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
  }

  pa_assert_internal(!force || page->local_free == NULL);
}



/* -----------------------------------------------------------
  Page fresh and retire
----------------------------------------------------------- */

// called from segments when reclaiming abandoned pages
void _pa_page_reclaim(pa_heap_t* heap, pa_page_t* page) {
  pa_assert_expensive(pa_page_is_valid_init(page));

  pa_assert_internal(pa_page_heap(page) == heap);
  pa_assert_internal(pa_page_thread_free_flag(page) != PA_NEVER_DELAYED_FREE);
  #if PA_HUGE_PAGE_ABANDON
  pa_assert_internal(_pa_page_segment(page)->kind != PA_SEGMENT_HUGE);
  #endif

  // TODO: push on full queue immediately if it is full?
  pa_page_queue_t* pq = pa_page_queue(heap, pa_page_block_size(page));
  pa_page_queue_push(heap, pq, page);
  pa_assert_expensive(_pa_page_is_valid(page));
}

// allocate a fresh page from a segment
static pa_page_t* pa_page_fresh_alloc(pa_heap_t* heap, pa_page_queue_t* pq, size_t block_size, size_t page_alignment) {
  #if !PA_HUGE_PAGE_ABANDON
  pa_assert_internal(pq != NULL);
  pa_assert_internal(pa_heap_contains_queue(heap, pq));
  pa_assert_internal(page_alignment > 0 || block_size > PA_MEDIUM_OBJ_SIZE_MAX || block_size == pq->block_size);
  #endif
  pa_page_t* page = _pa_segment_page_alloc(heap, block_size, page_alignment, &heap->tld->segments);
  if (page == NULL) {
    // this may be out-of-memory, or an abandoned page was reclaimed (and in our queue)
    return NULL;
  }
  #if PA_HUGE_PAGE_ABANDON
  pa_assert_internal(pq==NULL || _pa_page_segment(page)->page_kind != PA_PAGE_HUGE);
  #endif
  pa_assert_internal(page_alignment >0 || block_size > PA_MEDIUM_OBJ_SIZE_MAX || _pa_page_segment(page)->kind != PA_SEGMENT_HUGE);
  pa_assert_internal(pq!=NULL || pa_page_block_size(page) >= block_size);
  // a fresh page was found, initialize it
  const size_t full_block_size = (pq == NULL || pa_page_is_huge(page) ? pa_page_block_size(page) : block_size); // see also: pa_segment_huge_page_alloc
  pa_assert_internal(full_block_size >= block_size);
  pa_page_init(heap, page, full_block_size, heap->tld);
  pa_heap_stat_increase(heap, pages, 1);
  pa_heap_stat_increase(heap, page_bins[_pa_page_stats_bin(page)], 1);
  if (pq != NULL) { pa_page_queue_push(heap, pq, page); }
  pa_assert_expensive(_pa_page_is_valid(page));
  return page;
}

// Get a fresh page to use
static pa_page_t* pa_page_fresh(pa_heap_t* heap, pa_page_queue_t* pq) {
  pa_assert_internal(pa_heap_contains_queue(heap, pq));
  pa_page_t* page = pa_page_fresh_alloc(heap, pq, pq->block_size, 0);
  if (page==NULL) return NULL;
  pa_assert_internal(pq->block_size==pa_page_block_size(page));
  pa_assert_internal(pq==pa_page_queue(heap, pa_page_block_size(page)));
  return page;
}

/* -----------------------------------------------------------
   Do any delayed frees
   (put there by other threads if they deallocated in a full page)
----------------------------------------------------------- */
void _pa_heap_delayed_free_all(pa_heap_t* heap) {
  while (!_pa_heap_delayed_free_partial(heap)) {
    pa_atomic_yield();
  }
}

// returns true if all delayed frees were processed
bool _pa_heap_delayed_free_partial(pa_heap_t* heap) {
  // take over the list (note: no atomic exchange since it is often NULL)
  pa_block_t* block = pa_atomic_load_ptr_relaxed(pa_block_t, &heap->thread_delayed_free);
  while (block != NULL && !pa_atomic_cas_ptr_weak_acq_rel(pa_block_t, &heap->thread_delayed_free, &block, NULL)) { /* nothing */ };
  bool all_freed = true;

  // and free them all
  while (block != NULL) {
    pa_block_t* next = pa_block_nextx(heap, block, heap->keys);
    // use internal free instead of regular one to keep stats etc correct
    if (!_pa_free_delayed_block(block)) {
      // we might already start delayed freeing while another thread has not yet
      // reset the delayed_freeing flag; in that case delay it further by reinserting the current block
      // into the delayed free list
      all_freed = false;
      pa_block_t* dfree = pa_atomic_load_ptr_relaxed(pa_block_t, &heap->thread_delayed_free);
      do {
        pa_block_set_nextx(heap, block, dfree, heap->keys);
      } while (!pa_atomic_cas_ptr_weak_release(pa_block_t, &heap->thread_delayed_free, &dfree, block));
    }
    block = next;
  }
  return all_freed;
}

/* -----------------------------------------------------------
  Unfull, abandon, free and retire
----------------------------------------------------------- */

// Move a page from the full list back to a regular list
void _pa_page_unfull(pa_page_t* page) {
  pa_assert_internal(page != NULL);
  pa_assert_expensive(_pa_page_is_valid(page));
  pa_assert_internal(pa_page_is_in_full(page));
  if (!pa_page_is_in_full(page)) return;

  pa_heap_t* heap = pa_page_heap(page);
  pa_page_queue_t* pqfull = &heap->pages[PA_BIN_FULL];
  pa_page_set_in_full(page, false); // to get the right queue
  pa_page_queue_t* pq = pa_heap_page_queue_of(heap, page);
  pa_page_set_in_full(page, true);
  pa_page_queue_enqueue_from_full(pq, pqfull, page);
}

static void pa_page_to_full(pa_page_t* page, pa_page_queue_t* pq) {
  pa_assert_internal(pq == pa_page_queue_of(page));
  pa_assert_internal(!pa_page_immediate_available(page));
  pa_assert_internal(!pa_page_is_in_full(page));

  if (pa_page_is_in_full(page)) return;
  pa_page_queue_enqueue_from(&pa_page_heap(page)->pages[PA_BIN_FULL], pq, page);
  _pa_page_free_collect(page,false);  // try to collect right away in case another thread freed just before PA_USE_DELAYED_FREE was set
}


// Abandon a page with used blocks at the end of a thread.
// Note: only call if it is ensured that no references exist from
// the `page->heap->thread_delayed_free` into this page.
// Currently only called through `pa_heap_collect_ex` which ensures this.
void _pa_page_abandon(pa_page_t* page, pa_page_queue_t* pq) {
  pa_assert_internal(page != NULL);
  pa_assert_expensive(_pa_page_is_valid(page));
  pa_assert_internal(pq == pa_page_queue_of(page));
  pa_assert_internal(pa_page_heap(page) != NULL);

  pa_heap_t* pheap = pa_page_heap(page);

  // remove from our page list
  pa_segments_tld_t* segments_tld = &pheap->tld->segments;
  pa_page_queue_remove(pq, page);

  // page is no longer associated with our heap
  pa_assert_internal(pa_page_thread_free_flag(page)==PA_NEVER_DELAYED_FREE);
  pa_page_set_heap(page, NULL);

#if (PA_DEBUG>1) && !PA_TRACK_ENABLED
  // check there are no references left..
  for (pa_block_t* block = (pa_block_t*)pheap->thread_delayed_free; block != NULL; block = pa_block_nextx(pheap, block, pheap->keys)) {
    pa_assert_internal(_pa_ptr_page(block) != page);
  }
#endif

  // and abandon it
  pa_assert_internal(pa_page_heap(page) == NULL);
  _pa_segment_page_abandon(page,segments_tld);
}

// force abandon a page
void _pa_page_force_abandon(pa_page_t* page) {
  pa_heap_t* heap = pa_page_heap(page);
  // mark page as not using delayed free
  _pa_page_use_delayed_free(page, PA_NEVER_DELAYED_FREE, false);

  // ensure this page is no longer in the heap delayed free list
  _pa_heap_delayed_free_all(heap);
  // We can still access the page meta-info even if it is freed as we ensure
  // in `pa_segment_force_abandon` that the segment is not freed (yet)
  if (page->capacity == 0) return; // it may have been freed now

  // and now unlink it from the page queue and abandon (or free)
  pa_page_queue_t* pq = pa_heap_page_queue_of(heap, page);
  if (pa_page_all_free(page)) {
    _pa_page_free(page, pq, false);
  }
  else {
    _pa_page_abandon(page, pq);
  }
}


// Free a page with no more free blocks
void _pa_page_free(pa_page_t* page, pa_page_queue_t* pq, bool force) {
  pa_assert_internal(page != NULL);
  pa_assert_expensive(_pa_page_is_valid(page));
  pa_assert_internal(pq == pa_page_queue_of(page));
  pa_assert_internal(pa_page_all_free(page));
  pa_assert_internal(pa_page_thread_free_flag(page)!=PA_DELAYED_FREEING);

  // no more aligned blocks in here
  pa_page_set_has_aligned(page, false);

  // remove from the page list
  // (no need to do _pa_heap_delayed_free first as all blocks are already free)
  pa_heap_t* heap = pa_page_heap(page);
  pa_segments_tld_t* segments_tld = &heap->tld->segments;
  pa_page_queue_remove(pq, page);

  // and free it  
  pa_page_set_heap(page,NULL);
  _pa_segment_page_free(page, force, segments_tld);
}

#define PA_MAX_RETIRE_SIZE    PA_MEDIUM_OBJ_SIZE_MAX   // should be less than size for PA_BIN_HUGE
#define PA_RETIRE_CYCLES      (16)

// Retire a page with no more used blocks
// Important to not retire too quickly though as new
// allocations might coming.
// Note: called from `pa_free` and benchmarks often
// trigger this due to freeing everything and then
// allocating again so careful when changing this.
void _pa_page_retire(pa_page_t* page) pa_attr_noexcept {
  pa_assert_internal(page != NULL);
  pa_assert_expensive(_pa_page_is_valid(page));
  pa_assert_internal(pa_page_all_free(page));

  pa_page_set_has_aligned(page, false);

  // don't retire too often..
  // (or we end up retiring and re-allocating most of the time)
  // NOTE: refine this more: we should not retire if this
  // is the only page left with free blocks. It is not clear
  // how to check this efficiently though...
  // for now, we don't retire if it is the only page left of this size class.
  pa_page_queue_t* pq = pa_page_queue_of(page);
  #if PA_RETIRE_CYCLES > 0
  const size_t bsize = pa_page_block_size(page);
  if pa_likely( /* bsize < PA_MAX_RETIRE_SIZE && */ !pa_page_queue_is_special(pq)) {  // not full or huge queue?
    if (pq->last==page && pq->first==page) { // the only page in the queue?
      pa_stat_counter_increase(_pa_stats_main.pages_retire,1);
      page->retire_expire = (bsize <= PA_SMALL_OBJ_SIZE_MAX ? PA_RETIRE_CYCLES : PA_RETIRE_CYCLES/4);
      pa_heap_t* heap = pa_page_heap(page);
      pa_assert_internal(pq >= heap->pages);
      const size_t index = pq - heap->pages;
      pa_assert_internal(index < PA_BIN_FULL && index < PA_BIN_HUGE);
      if (index < heap->page_retired_min) heap->page_retired_min = index;
      if (index > heap->page_retired_max) heap->page_retired_max = index;
      pa_assert_internal(pa_page_all_free(page));
      return; // don't free after all
    }
  }
  #endif
  _pa_page_free(page, pq, false);
}

// free retired pages: we don't need to look at the entire queues
// since we only retire pages that are at the head position in a queue.
void _pa_heap_collect_retired(pa_heap_t* heap, bool force) {
  size_t min = PA_BIN_FULL;
  size_t max = 0;
  for(size_t bin = heap->page_retired_min; bin <= heap->page_retired_max; bin++) {
    pa_page_queue_t* pq   = &heap->pages[bin];
    pa_page_t*       page = pq->first;
    if (page != NULL && page->retire_expire != 0) {
      if (pa_page_all_free(page)) {
        page->retire_expire--;
        if (force || page->retire_expire == 0) {
          _pa_page_free(pq->first, pq, force);
        }
        else {
          // keep retired, update min/max
          if (bin < min) min = bin;
          if (bin > max) max = bin;
        }
      }
      else {
        page->retire_expire = 0;
      }
    }
  }
  heap->page_retired_min = min;
  heap->page_retired_max = max;
}


/* -----------------------------------------------------------
  Initialize the initial free list in a page.
  In secure mode we initialize a randomized list by
  alternating between slices.
----------------------------------------------------------- */

#define PA_MAX_SLICE_SHIFT  (6)   // at most 64 slices
#define PA_MAX_SLICES       (1UL << PA_MAX_SLICE_SHIFT)
#define PA_MIN_SLICES       (2)

static void pa_page_free_list_extend_secure(pa_heap_t* const heap, pa_page_t* const page, const size_t bsize, const size_t extend, pa_stats_t* const stats) {
  PA_UNUSED(stats);
  #if (PA_SECURE<=2)
  pa_assert_internal(page->free == NULL);
  pa_assert_internal(page->local_free == NULL);
  #endif
  pa_assert_internal(page->capacity + extend <= page->reserved);
  pa_assert_internal(bsize == pa_page_block_size(page));
  void* const page_area = pa_page_start(page);

  // initialize a randomized free list
  // set up `slice_count` slices to alternate between
  size_t shift = PA_MAX_SLICE_SHIFT;
  while ((extend >> shift) == 0) {
    shift--;
  }
  const size_t slice_count = (size_t)1U << shift;
  const size_t slice_extend = extend / slice_count;
  pa_assert_internal(slice_extend >= 1);
  pa_block_t* blocks[PA_MAX_SLICES];   // current start of the slice
  size_t      counts[PA_MAX_SLICES];   // available objects in the slice
  for (size_t i = 0; i < slice_count; i++) {
    blocks[i] = pa_page_block_at(page, page_area, bsize, page->capacity + i*slice_extend);
    counts[i] = slice_extend;
  }
  counts[slice_count-1] += (extend % slice_count);  // final slice holds the modulus too (todo: distribute evenly?)

  // and initialize the free list by randomly threading through them
  // set up first element
  const uintptr_t r = _pa_heap_random_next(heap);
  size_t current = r % slice_count;
  counts[current]--;
  pa_block_t* const free_start = blocks[current];
  // and iterate through the rest; use `random_shuffle` for performance
  uintptr_t rnd = _pa_random_shuffle(r|1); // ensure not 0
  for (size_t i = 1; i < extend; i++) {
    // call random_shuffle only every INTPTR_SIZE rounds
    const size_t round = i%PA_INTPTR_SIZE;
    if (round == 0) rnd = _pa_random_shuffle(rnd);
    // select a random next slice index
    size_t next = ((rnd >> 8*round) & (slice_count-1));
    while (counts[next]==0) {                            // ensure it still has space
      next++;
      if (next==slice_count) next = 0;
    }
    // and link the current block to it
    counts[next]--;
    pa_block_t* const block = blocks[current];
    blocks[current] = (pa_block_t*)((uint8_t*)block + bsize);  // bump to the following block
    pa_block_set_next(page, block, blocks[next]);   // and set next; note: we may have `current == next`
    current = next;
  }
  // prepend to the free list (usually NULL)
  pa_block_set_next(page, blocks[current], page->free);  // end of the list
  page->free = free_start;
}

static pa_decl_noinline void pa_page_free_list_extend( pa_page_t* const page, const size_t bsize, const size_t extend, pa_stats_t* const stats)
{
  PA_UNUSED(stats);
  #if (PA_SECURE <= 2)
  pa_assert_internal(page->free == NULL);
  pa_assert_internal(page->local_free == NULL);
  #endif
  pa_assert_internal(page->capacity + extend <= page->reserved);
  pa_assert_internal(bsize == pa_page_block_size(page));
  void* const page_area = pa_page_start(page);

  pa_block_t* const start = pa_page_block_at(page, page_area, bsize, page->capacity);

  // initialize a sequential free list
  pa_block_t* const last = pa_page_block_at(page, page_area, bsize, page->capacity + extend - 1);
  pa_block_t* block = start;
  while(block <= last) {
    pa_block_t* next = (pa_block_t*)((uint8_t*)block + bsize);
    pa_block_set_next(page,block,next);
    block = next;
  }
  // prepend to free list (usually `NULL`)
  pa_block_set_next(page, last, page->free);
  page->free = start;
}

/* -----------------------------------------------------------
  Page initialize and extend the capacity
----------------------------------------------------------- */

#define PA_MAX_EXTEND_SIZE    (4*1024)      // heuristic, one OS page seems to work well.
#if (PA_SECURE>0)
#define PA_MIN_EXTEND         (8*PA_SECURE) // extend at least by this many
#else
#define PA_MIN_EXTEND         (4)
#endif

// Extend the capacity (up to reserved) by initializing a free list
// We do at most `PA_MAX_EXTEND` to avoid touching too much memory
// Note: we also experimented with "bump" allocation on the first
// allocations but this did not speed up any benchmark (due to an
// extra test in malloc? or cache effects?)
static bool pa_page_extend_free(pa_heap_t* heap, pa_page_t* page, pa_tld_t* tld) {
  pa_assert_expensive(pa_page_is_valid_init(page));
  #if (PA_SECURE<=2)
  pa_assert(page->free == NULL);
  pa_assert(page->local_free == NULL);
  if (page->free != NULL) return true;
  #endif
  if (page->capacity >= page->reserved) return true;

  pa_stat_counter_increase(tld->stats.pages_extended, 1);

  // calculate the extend count
  const size_t bsize = pa_page_block_size(page);
  size_t extend = page->reserved - page->capacity;
  pa_assert_internal(extend > 0);

  size_t max_extend = (bsize >= PA_MAX_EXTEND_SIZE ? PA_MIN_EXTEND : PA_MAX_EXTEND_SIZE/bsize);
  if (max_extend < PA_MIN_EXTEND) { max_extend = PA_MIN_EXTEND; }
  pa_assert_internal(max_extend > 0);

  if (extend > max_extend) {
    // ensure we don't touch memory beyond the page to reduce page commit.
    // the `lean` benchmark tests this. Going from 1 to 8 increases rss by 50%.
    extend = max_extend;
  }

  pa_assert_internal(extend > 0 && extend + page->capacity <= page->reserved);
  pa_assert_internal(extend < (1UL<<16));

  // and append the extend the free list
  if (extend < PA_MIN_SLICES || PA_SECURE==0) { //!pa_option_is_enabled(pa_option_secure)) {
    pa_page_free_list_extend(page, bsize, extend, &tld->stats );
  }
  else {
    pa_page_free_list_extend_secure(heap, page, bsize, extend, &tld->stats);
  }
  // enable the new free list
  page->capacity += (uint16_t)extend;
  pa_stat_increase(tld->stats.page_committed, extend * bsize);
  pa_assert_expensive(pa_page_is_valid_init(page));
  return true;
}

// Initialize a fresh page
static void pa_page_init(pa_heap_t* heap, pa_page_t* page, size_t block_size, pa_tld_t* tld) {
  pa_assert(page != NULL);
  pa_segment_t* segment = _pa_page_segment(page);
  pa_assert(segment != NULL);
  pa_assert_internal(block_size > 0);
  // set fields
  pa_page_set_heap(page, heap);
  page->block_size = block_size;
  size_t page_size;
  page->page_start = _pa_segment_page_start(segment, page, &page_size);
  pa_track_mem_noaccess(page->page_start,page_size);
  pa_assert_internal(pa_page_block_size(page) <= page_size);
  pa_assert_internal(page_size <= page->slice_count*PA_SEGMENT_SLICE_SIZE);
  pa_assert_internal(page_size / block_size < (1L<<16));
  page->reserved = (uint16_t)(page_size / block_size);
  pa_assert_internal(page->reserved > 0);
  #if (PA_PADDING || PA_ENCODE_FREELIST)
  page->keys[0] = _pa_heap_random_next(heap);
  page->keys[1] = _pa_heap_random_next(heap);
  #endif
  page->free_is_zero = page->is_zero_init;
  #if PA_DEBUG>2
  if (page->is_zero_init) {
    pa_track_mem_defined(page->page_start, page_size);
    pa_assert_expensive(pa_mem_is_zero(page->page_start, page_size));
  }
  #endif
  pa_assert_internal(page->is_committed);
  if (block_size > 0 && _pa_is_power_of_two(block_size)) {
    page->block_size_shift = (uint8_t)(pa_ctz((uintptr_t)block_size));
  }
  else {
    page->block_size_shift = 0;
  }

  pa_assert_internal(page->capacity == 0);
  pa_assert_internal(page->free == NULL);
  pa_assert_internal(page->used == 0);
  pa_assert_internal(page->xthread_free == 0);
  pa_assert_internal(page->next == NULL);
  pa_assert_internal(page->prev == NULL);
  pa_assert_internal(page->retire_expire == 0);
  pa_assert_internal(!pa_page_has_aligned(page));
  #if (PA_PADDING || PA_ENCODE_FREELIST)
  pa_assert_internal(page->keys[0] != 0);
  pa_assert_internal(page->keys[1] != 0);
  #endif
  pa_assert_internal(page->block_size_shift == 0 || (block_size == ((size_t)1 << page->block_size_shift)));
  pa_assert_expensive(pa_page_is_valid_init(page));

  // initialize an initial free list
  if (pa_page_extend_free(heap,page,tld)) {
    pa_assert(pa_page_immediate_available(page));
  }
  return;
}


/* -----------------------------------------------------------
  Find pages with free blocks
-------------------------------------------------------------*/

// search for a best next page to use for at most N pages (often cut short if immediate blocks are available)
#define PA_MAX_CANDIDATE_SEARCH  (4)

// is the page not yet used up to its reserved space?
static bool pa_page_is_expandable(const pa_page_t* page) {
  pa_assert_internal(page != NULL);
  pa_assert_internal(page->capacity <= page->reserved);
  return (page->capacity < page->reserved);
}


// Find a page with free blocks of `page->block_size`.
static pa_page_t* pa_page_queue_find_free_ex(pa_heap_t* heap, pa_page_queue_t* pq, bool first_try)
{
  // search through the pages in "next fit" order
  #if PA_STAT
  size_t count = 0;
  #endif
  size_t candidate_count = 0;        // we reset this on the first candidate to limit the search
  pa_page_t* page_candidate = NULL;  // a page with free space
  pa_page_t* page = pq->first;

  while (page != NULL)
  {
    pa_page_t* next = page->next; // remember next
    #if PA_STAT
    count++;
    #endif
    candidate_count++;

    // collect freed blocks by us and other threads
    _pa_page_free_collect(page, false);

  #if PA_MAX_CANDIDATE_SEARCH > 1
    // search up to N pages for a best candidate

    // is the local free list non-empty?
    const bool immediate_available = pa_page_immediate_available(page);

    // if the page is completely full, move it to the `pa_pages_full`
    // queue so we don't visit long-lived pages too often.
    if (!immediate_available && !pa_page_is_expandable(page)) {
      pa_assert_internal(!pa_page_is_in_full(page) && !pa_page_immediate_available(page));
      pa_page_to_full(page, pq);
    }
    else {
      // the page has free space, make it a candidate
      // we prefer non-expandable pages with high usage as candidates (to reduce commit, and increase chances of free-ing up pages)
      if (page_candidate == NULL) {
        page_candidate = page;
        candidate_count = 0;
      }
      // prefer to reuse fuller pages (in the hope the less used page gets freed)
      else if (page->used >= page_candidate->used && !pa_page_is_mostly_used(page) && !pa_page_is_expandable(page)) {
        page_candidate = page;
      }
      // if we find a non-expandable candidate, or searched for N pages, return with the best candidate
      if (immediate_available || candidate_count > PA_MAX_CANDIDATE_SEARCH) {
        pa_assert_internal(page_candidate!=NULL);
        break;
      }
    }
  #else
    // first-fit algorithm
    // If the page contains free blocks, we are done
    if (pa_page_immediate_available(page) || pa_page_is_expandable(page)) {
      break;  // pick this one
    }

    // If the page is completely full, move it to the `pa_pages_full`
    // queue so we don't visit long-lived pages too often.
    pa_assert_internal(!pa_page_is_in_full(page) && !pa_page_immediate_available(page));
    pa_page_to_full(page, pq);
  #endif

    page = next;
  } // for each page

  pa_heap_stat_counter_increase(heap, page_searches, count);
  pa_heap_stat_counter_increase(heap, page_searches_count, 1);

  // set the page to the best candidate
  if (page_candidate != NULL) {
    page = page_candidate;
  }
  if (page != NULL) {
    if (!pa_page_immediate_available(page)) {
      pa_assert_internal(pa_page_is_expandable(page));
      if (!pa_page_extend_free(heap, page, heap->tld)) {
        page = NULL; // failed to extend
      }
    }
    pa_assert_internal(page == NULL || pa_page_immediate_available(page));
  }

  if (page == NULL) {
    _pa_heap_collect_retired(heap, false); // perhaps make a page available?
    page = pa_page_fresh(heap, pq);
    if (page == NULL && first_try) {
      // out-of-memory _or_ an abandoned page with free blocks was reclaimed, try once again
      page = pa_page_queue_find_free_ex(heap, pq, false);
    }
  }
  else {
    // move the page to the front of the queue
    pa_page_queue_move_to_front(heap, pq, page);
    page->retire_expire = 0;
    // _pa_heap_collect_retired(heap, false); // update retire counts; note: increases rss on MemoryLoad bench so don't do this
  }
  pa_assert_internal(page == NULL || pa_page_immediate_available(page));


  return page;
}



// Find a page with free blocks of `size`.
static inline pa_page_t* pa_find_free_page(pa_heap_t* heap, size_t size) {
  pa_page_queue_t* pq = pa_page_queue(heap, size);

  // check the first page: we even do this with candidate search or otherwise we re-search every time
  pa_page_t* page = pq->first;
  if (page != NULL) {
   #if (PA_SECURE>=3) // in secure mode, we extend half the time to increase randomness
    if (page->capacity < page->reserved && ((_pa_heap_random_next(heap) & 1) == 1)) {
      pa_page_extend_free(heap, page, heap->tld);
      pa_assert_internal(pa_page_immediate_available(page));
    }
    else
   #endif
    {
      _pa_page_free_collect(page, false);
    }

    if (pa_page_immediate_available(page)) {
      page->retire_expire = 0;
      return page; // fast path
    }
  }

  return pa_page_queue_find_free_ex(heap, pq, true);
}


/* -----------------------------------------------------------
  Users can register a deferred free function called
  when the `free` list is empty. Since the `local_free`
  is separate this is deterministically called after
  a certain number of allocations.
----------------------------------------------------------- */

static pa_deferred_free_fun* volatile deferred_free = NULL;
static _Atomic(void*) deferred_arg; // = NULL

void _pa_deferred_free(pa_heap_t* heap, bool force) {
  heap->tld->heartbeat++;
  if (deferred_free != NULL && !heap->tld->recurse) {
    heap->tld->recurse = true;
    deferred_free(force, heap->tld->heartbeat, pa_atomic_load_ptr_relaxed(void,&deferred_arg));
    heap->tld->recurse = false;
  }
}

void pa_register_deferred_free(pa_deferred_free_fun* fn, void* arg) pa_attr_noexcept {
  deferred_free = fn;
  pa_atomic_store_ptr_release(void,&deferred_arg, arg);
}


/* -----------------------------------------------------------
  General allocation
----------------------------------------------------------- */

// Large and huge page allocation.
// Huge pages contain just one block, and the segment contains just that page (as `PA_SEGMENT_HUGE`).
// Huge pages are also use if the requested alignment is very large (> PA_BLOCK_ALIGNMENT_MAX)
// so their size is not always `> PA_LARGE_OBJ_SIZE_MAX`.
static pa_page_t* pa_large_huge_page_alloc(pa_heap_t* heap, size_t size, size_t page_alignment) {
  size_t block_size = _pa_os_good_alloc_size(size);
  pa_assert_internal(pa_bin(block_size) == PA_BIN_HUGE || page_alignment > 0);
  bool is_huge = (block_size > PA_LARGE_OBJ_SIZE_MAX || page_alignment > 0);
  #if PA_HUGE_PAGE_ABANDON
  pa_page_queue_t* pq = (is_huge ? NULL : pa_page_queue(heap, block_size));
  #else
  pa_page_queue_t* pq = pa_page_queue(heap, is_huge ? PA_LARGE_OBJ_SIZE_MAX+1 : block_size);
  pa_assert_internal(!is_huge || pa_page_queue_is_huge(pq));
  #endif
  pa_page_t* page = pa_page_fresh_alloc(heap, pq, block_size, page_alignment);
  if (page != NULL) {
    pa_assert_internal(pa_page_immediate_available(page));

    if (is_huge) {
      pa_assert_internal(pa_page_is_huge(page));
      pa_assert_internal(_pa_page_segment(page)->kind == PA_SEGMENT_HUGE);
      pa_assert_internal(_pa_page_segment(page)->used==1);
      #if PA_HUGE_PAGE_ABANDON
      pa_assert_internal(_pa_page_segment(page)->thread_id==0); // abandoned, not in the huge queue
      pa_page_set_heap(page, NULL);
      #endif
    }
    else {
      pa_assert_internal(!pa_page_is_huge(page));
    }

    const size_t bsize = pa_page_usable_block_size(page);  // note: not `pa_page_block_size` to account for padding
    /*if (bsize <= PA_LARGE_OBJ_SIZE_MAX) {
      pa_heap_stat_increase(heap, malloc_large, bsize);
      pa_heap_stat_counter_increase(heap, malloc_large_count, 1);
    }
    else */
    {
      _pa_stat_increase(&heap->tld->stats.malloc_huge, bsize);
      _pa_stat_counter_increase(&heap->tld->stats.malloc_huge_count, 1);
    }
  }
  return page;
}


// Allocate a page
// Note: in debug mode the size includes PA_PADDING_SIZE and might have overflowed.
static pa_page_t* pa_find_page(pa_heap_t* heap, size_t size, size_t huge_alignment) pa_attr_noexcept {
  // huge allocation?
  const size_t req_size = size - PA_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
  if pa_unlikely(req_size > (PA_MEDIUM_OBJ_SIZE_MAX - PA_PADDING_SIZE) || huge_alignment > 0) {
    if pa_unlikely(req_size > PA_MAX_ALLOC_SIZE) {
      _pa_error_message(EOVERFLOW, "allocation request is too large (%zu bytes)\n", req_size);
      return NULL;
    }
    else {
      return pa_large_huge_page_alloc(heap,size,huge_alignment);
    }
  }
  else {
    // otherwise find a page with free blocks in our size segregated queues
    #if PA_PADDING
    pa_assert_internal(size >= PA_PADDING_SIZE);
    #endif
    return pa_find_free_page(heap, size);
  }
}

// Refill thread cache from page after we took one block (tcmalloc-style batch refill).
static void pa_heap_cache_refill(pa_heap_t* heap, pa_page_t* page) {
  if (page == NULL || pa_page_is_huge(page)) return;
  const size_t bsize = pa_page_block_size(page);
  const size_t bin = _pa_bin(bsize);
  if (bin >= PA_CACHE_BINS) return;
  for (unsigned i = 0; i < PA_CACHE_REFILL && heap->cache_count[bin] < PA_CACHE_MAX_PER_BIN; i++) {
    if (page->free == NULL) _pa_page_free_collect(page, false);
    if (page->free == NULL) break;
    pa_block_t* block = page->free;
    page->free = pa_block_next(page, block);
    pa_block_set_nextx(heap, block, heap->cache_head[bin], heap->keys);
    heap->cache_head[bin] = block;
    heap->cache_count[bin]++;
  }
}

// Generic allocation routine if the fast path (`alloc.c:pa_page_malloc`) does not succeed.
// Note: in debug mode the size includes PA_PADDING_SIZE and might have overflowed.
// The `huge_alignment` is normally 0 but is set to a multiple of PA_SLICE_SIZE for
// very large requested alignments in which case we use a huge singleton page.
void* _pa_malloc_generic(pa_heap_t* heap, size_t size, bool zero, size_t huge_alignment, size_t* usable) pa_attr_noexcept
{
  pa_assert_internal(heap != NULL);

  if (heap->max_size != 0 && size > heap->max_size - heap->used_bytes) {
    if (heap->pressure_cb != NULL)
      heap->pressure_cb(heap->used_bytes, heap->max_size, heap->pressure_arg);
    return NULL;
  }

  // initialize if necessary
  if pa_unlikely(!pa_heap_is_initialized(heap)) {
    heap = pa_heap_get_default(); // calls pa_thread_init
    if pa_unlikely(!pa_heap_is_initialized(heap)) { return NULL; }
  }
  pa_assert_internal(pa_heap_is_initialized(heap));

  // Thread cache (tcmalloc/jemalloc-style): pop from per-bin cache when possible.
  // For calloc: use cache only for small sizes so large calloc can get zero pages and skip memset.
  #define PA_CALLOC_CACHE_MAX  (2048 + PA_PADDING_SIZE)  // above this, skip cache for zero so we prefer zero pages
  const bool use_cache = (huge_alignment == 0 && size <= PA_MEDIUM_OBJ_SIZE_MAX + PA_PADDING_SIZE) &&
                        (!zero || size <= PA_CALLOC_CACHE_MAX);
  if pa_likely(use_cache) {
    const size_t bin = _pa_bin_fast(size);
    if (bin < PA_BIN_HUGE) {
      pa_block_t* block = heap->cache_head[bin];
      if pa_likely(block != NULL && heap->cache_count[bin] > 0) {
        heap->cache_head[bin] = pa_block_nextx(heap, block, heap->keys);
        heap->cache_count[bin]--;
        pa_page_t* page = _pa_ptr_page(block);
        page->used++;
        _pa_page_finish_alloc(heap, page, block, size, zero, usable);
        return block;
      }
    }
  }

  // do administrative tasks every N generic mallocs
  if pa_unlikely(++heap->generic_count >= 100) {
    heap->generic_collect_count += heap->generic_count;
    heap->generic_count = 0;
    // call potential deferred free routines
    _pa_deferred_free(heap, false);

    // free delayed frees from other threads (but skip contended ones)
    _pa_heap_delayed_free_partial(heap);

    // collect every once in a while (10000 by default)
    const long generic_collect = pa_option_get_clamp(pa_option_generic_collect, 1, 1000000L);
    if (heap->generic_collect_count >= generic_collect) {
      heap->generic_collect_count = 0;
      pa_heap_collect(heap, false /* force? */);
    }
  }

  // find (or allocate) a page of the right size
  pa_page_t* page = pa_find_page(heap, size, huge_alignment);
  if pa_unlikely(page == NULL) { // first time out of memory, try to collect and retry the allocation once more
    pa_heap_collect(heap, true /* force */);
    page = pa_find_page(heap, size, huge_alignment);
  }

  if pa_unlikely(page == NULL) { // out of memory
    const size_t req_size = size - PA_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
    _pa_error_message(ENOMEM, "unable to allocate memory (%zu bytes)\n", req_size);
    return NULL;
  }

  pa_assert_internal(pa_page_immediate_available(page));
  pa_assert_internal(pa_page_block_size(page) >= size);

  // and try again, this time succeeding! (i.e. this should never recurse through _pa_page_malloc)
  void* const p = _pa_page_malloc_zero(heap, page, size, zero, usable);
  pa_assert_internal(p != NULL);

  // Batch refill thread cache from this page (tcmalloc-style) so future allocs hit cache
  if (huge_alignment == 0 && size <= PA_MEDIUM_OBJ_SIZE_MAX + PA_PADDING_SIZE)
    pa_heap_cache_refill(heap, page);

  // move singleton pages to the full queue
  if (page->reserved == page->used) {
    pa_page_to_full(page, pa_page_queue_of(page));
  }
  return p;
}
