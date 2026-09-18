/* ----------------------------------------------------------------------------
Copyright (c) 2018-2022, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/prim.h"

#include <string.h>  // memcpy, memset
#include <stdlib.h>  // atexit
#include <stdatomic.h>

#if defined(PA_USE_PTHREADS)
#include <pthread.h>
#include <unistd.h>
#endif


// Empty page used to initialize the small free pages array
const pa_page_t _pa_page_empty = {
  .slice_count = 0,
  .slice_offset = 0,
  .is_committed = false,
  .is_huge = false,
  .is_zero_init = false,
  .retire_expire = 0,
  .capacity = 0,
  .used = 0,
  .reserved = 0,
  .flags = { 0 },
  .free_is_zero = false,
  .block_size_shift = 0,
  .free = NULL,
  .local_free = NULL,
  .block_size = 0,
  .page_start = NULL,
  #if (PA_PADDING || PA_ENCODE_FREELIST)
  .keys = { 0, 0 },
  #endif
  .xthread_free = PA_ATOMIC_VAR_INIT(0),
  .xheap = PA_ATOMIC_VAR_INIT(0),
  .next = NULL,
  .prev = NULL,
  .padding = { 0 }
};

#define PA_PAGE_EMPTY() ((pa_page_t*)&_pa_page_empty)

#if (PA_SMALL_WSIZE_MAX==128)
#if (PA_PADDING>0) && (PA_INTPTR_SIZE >= 8)
#define PA_SMALL_PAGES_EMPTY  { PA_INIT128(PA_PAGE_EMPTY), PA_PAGE_EMPTY(), PA_PAGE_EMPTY() }
#elif (PA_PADDING>0)
#define PA_SMALL_PAGES_EMPTY  { PA_INIT128(PA_PAGE_EMPTY), PA_PAGE_EMPTY(), PA_PAGE_EMPTY(), PA_PAGE_EMPTY() }
#else
#define PA_SMALL_PAGES_EMPTY  { PA_INIT128(PA_PAGE_EMPTY), PA_PAGE_EMPTY() }
#endif
#else
#error "define right initialization sizes corresponding to PA_SMALL_WSIZE_MAX"
#endif

// Empty page queues for every bin
#define QNULL(sz)  { NULL, NULL, (sz)*sizeof(uintptr_t) }
#define PA_PAGE_QUEUES_EMPTY \
  { QNULL(1), \
    QNULL(     1), QNULL(     2), QNULL(     3), QNULL(     4), QNULL(     5), QNULL(     6), QNULL(     7), QNULL(     8), /* 8 */ \
    QNULL(    10), QNULL(    12), QNULL(    14), QNULL(    16), QNULL(    20), QNULL(    24), QNULL(    28), QNULL(    32), /* 16 */ \
    QNULL(    40), QNULL(    48), QNULL(    56), QNULL(    64), QNULL(    80), QNULL(    96), QNULL(   112), QNULL(   128), /* 24 */ \
    QNULL(   160), QNULL(   192), QNULL(   224), QNULL(   256), QNULL(   320), QNULL(   384), QNULL(   448), QNULL(   512), /* 32 */ \
    QNULL(   640), QNULL(   768), QNULL(   896), QNULL(  1024), QNULL(  1280), QNULL(  1536), QNULL(  1792), QNULL(  2048), /* 40 */ \
    QNULL(  2560), QNULL(  3072), QNULL(  3584), QNULL(  4096), QNULL(  5120), QNULL(  6144), QNULL(  7168), QNULL(  8192), /* 48 */ \
    QNULL( 10240), QNULL( 12288), QNULL( 14336), QNULL( 16384), QNULL( 20480), QNULL( 24576), QNULL( 28672), QNULL( 32768), /* 56 */ \
    QNULL( 40960), QNULL( 49152), QNULL( 57344), QNULL( 65536), QNULL( 81920), QNULL( 98304), QNULL(114688), QNULL(131072), /* 64 */ \
    QNULL(163840), QNULL(196608), QNULL(229376), QNULL(262144), QNULL(327680), QNULL(393216), QNULL(458752), QNULL(524288), /* 72 */ \
    QNULL(PA_MEDIUM_OBJ_WSIZE_MAX + 1  /* 655360, Huge queue */), \
    QNULL(PA_MEDIUM_OBJ_WSIZE_MAX + 2) /* Full queue */ }

#define PA_STAT_COUNT_NULL()  {0,0,0}

// Empty statistics
#define PA_STATS_NULL  \
  PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), \
  { 0 }, { 0 }, \
  PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), \
  PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), PA_STAT_COUNT_NULL(), \
  { 0 }, { 0 }, { 0 }, { 0 }, \
  { 0 }, { 0 }, { 0 }, { 0 }, \
  \
  { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, \
  PA_INIT4(PA_STAT_COUNT_NULL), \
  { 0 }, { 0 }, { 0 }, { 0 },  \
  \
  { PA_INIT4(PA_STAT_COUNT_NULL) }, \
  { { 0 }, { 0 }, { 0 }, { 0 } }, \
  \
  { PA_INIT74(PA_STAT_COUNT_NULL) }, \
  { PA_INIT74(PA_STAT_COUNT_NULL) }


// Empty slice span queues for every bin
#define SQNULL(sz)  { NULL, NULL, sz }
#define PA_SEGMENT_SPAN_QUEUES_EMPTY \
  { SQNULL(1), \
    SQNULL(     1), SQNULL(     2), SQNULL(     3), SQNULL(     4), SQNULL(     5), SQNULL(     6), SQNULL(     7), SQNULL(    10), /*  8 */ \
    SQNULL(    12), SQNULL(    14), SQNULL(    16), SQNULL(    20), SQNULL(    24), SQNULL(    28), SQNULL(    32), SQNULL(    40), /* 16 */ \
    SQNULL(    48), SQNULL(    56), SQNULL(    64), SQNULL(    80), SQNULL(    96), SQNULL(   112), SQNULL(   128), SQNULL(   160), /* 24 */ \
    SQNULL(   192), SQNULL(   224), SQNULL(   256), SQNULL(   320), SQNULL(   384), SQNULL(   448), SQNULL(   512), SQNULL(   640), /* 32 */ \
    SQNULL(   768), SQNULL(   896), SQNULL(  1024) /* 35 */ }


// --------------------------------------------------------
// Statically allocate an empty heap as the initial
// thread local value for the default heap,
// and statically allocate the backing heap for the main
// thread so it can function without doing any allocation
// itself (as accessing a thread local for the first time
// may lead to allocation itself on some platforms)
// --------------------------------------------------------

pa_decl_cache_align const pa_heap_t _pa_heap_empty = {
  NULL,
  PA_ATOMIC_VAR_INIT(NULL),
  0,                // tid
  0,                // cookie
  0,                // arena id
  { 0, 0 },         // keys
  { {0}, {0}, 0, true }, // random
  0,                // page count
  PA_BIN_FULL, 0,   // page retired min/max
  0, 0,             // generic count
  NULL,             // next
  false,            // can reclaim
  0,                // tag
  0,                // max_size
  0,                // used_bytes
  NULL, NULL,       // pressure_cb, pressure_arg
  #if PA_GUARDED
  0, 0, 0, 1,       // count is 1 so we never write to it (see `internal.h:pa_heap_malloc_use_guarded`)
  #endif
  PA_SMALL_PAGES_EMPTY,
  PA_PAGE_QUEUES_EMPTY,
  { 0 },   // cache_head
  { 0 }    // cache_count
};

static pa_decl_cache_align pa_subproc_t pa_subproc_default;

#define tld_empty_stats  ((pa_stats_t*)((uint8_t*)&tld_empty + offsetof(pa_tld_t,stats)))

pa_decl_cache_align static const pa_tld_t tld_empty = {
  0,
  false,
  NULL, NULL,
  { PA_SEGMENT_SPAN_QUEUES_EMPTY, 0, 0, 0, 0, 0, &pa_subproc_default, tld_empty_stats }, // segments
  { sizeof(pa_stats_t), PA_STAT_VERSION, PA_STATS_NULL }       // stats
};

pa_threadid_t _pa_thread_id(void) pa_attr_noexcept {
  return _pa_prim_thread_id();
}

// the thread-local default heap for allocation
pa_decl_thread pa_heap_t* _pa_heap_default = (pa_heap_t*)&_pa_heap_empty;

extern pa_decl_hidden pa_heap_t _pa_heap_main;

static pa_decl_cache_align pa_tld_t tld_main = {
  0, false,
  &_pa_heap_main, & _pa_heap_main,
  { PA_SEGMENT_SPAN_QUEUES_EMPTY, 0, 0, 0, 0, 0, &pa_subproc_default, &tld_main.stats }, // segments
  { sizeof(pa_stats_t), PA_STAT_VERSION, PA_STATS_NULL }       // stats
};

pa_decl_cache_align pa_heap_t _pa_heap_main = {
  &tld_main,
  PA_ATOMIC_VAR_INIT(NULL),
  0,                // thread id
  0,                // initial cookie
  0,                // arena id
  { 0, 0 },         // the key of the main heap can be fixed (unlike page keys that need to be secure!)
  { {0x846ca68b}, {0}, 0, true },  // random
  0,                // page count
  PA_BIN_FULL, 0,   // page retired min/max
  0, 0,             // generic count
  NULL,             // next heap
  false,            // can reclaim
  0,                // tag
  0,                // max_size
  0,                // used_bytes
  NULL, NULL,       // pressure_cb, pressure_arg
  #if PA_GUARDED
  0, 0, 0, 0,
  #endif
  PA_SMALL_PAGES_EMPTY,
  PA_PAGE_QUEUES_EMPTY,
  { 0 },   // cache_head
  { 0 }    // cache_count
};

bool _pa_process_is_initialized = false;  // set to `true` in `pa_process_init`.

pa_stats_t _pa_stats_main = { sizeof(pa_stats_t), PA_STAT_VERSION, PA_STATS_NULL };

#if PA_GUARDED
pa_decl_export void pa_heap_guarded_set_sample_rate(pa_heap_t* heap, size_t sample_rate, size_t seed) {
  heap->guarded_sample_rate  = sample_rate;
  heap->guarded_sample_count = sample_rate;  // count down samples
  if (heap->guarded_sample_rate > 1) {
    if (seed == 0) {
      seed = _pa_heap_random_next(heap);
    }
    heap->guarded_sample_count = (seed % heap->guarded_sample_rate) + 1;  // start at random count between 1 and `sample_rate`
  }
}

pa_decl_export void pa_heap_guarded_set_size_bound(pa_heap_t* heap, size_t min, size_t max) {
  heap->guarded_size_min = min;
  heap->guarded_size_max = (min > max ? min : max);
}

void _pa_heap_guarded_init(pa_heap_t* heap) {
  pa_heap_guarded_set_sample_rate(heap,
    (size_t)pa_option_get_clamp(pa_option_guarded_sample_rate, 0, LONG_MAX),
    (size_t)pa_option_get(pa_option_guarded_sample_seed));
  pa_heap_guarded_set_size_bound(heap,
    (size_t)pa_option_get_clamp(pa_option_guarded_min, 0, LONG_MAX),
    (size_t)pa_option_get_clamp(pa_option_guarded_max, 0, LONG_MAX) );
}
#else
pa_decl_export void pa_heap_guarded_set_sample_rate(pa_heap_t* heap, size_t sample_rate, size_t seed) {
  PA_UNUSED(heap); PA_UNUSED(sample_rate); PA_UNUSED(seed);
}

pa_decl_export void pa_heap_guarded_set_size_bound(pa_heap_t* heap, size_t min, size_t max) {
  PA_UNUSED(heap); PA_UNUSED(min); PA_UNUSED(max);
}
void _pa_heap_guarded_init(pa_heap_t* heap) {
  PA_UNUSED(heap);
}
#endif

static _Atomic(bool) pa_process_done_started = false;

#if defined(PA_USE_PTHREADS)
static pthread_t pa_background_thread;
static _Atomic(bool) pa_background_thread_running = false;

static void _pa_purge_background(void) {
  pa_heap_t* heap = pa_heap_get_default();
  pa_segments_tld_t* tld = &heap->tld->segments;
  
  // Purge abandoned segments
  _pa_abandoned_collect(heap, true /* force */, tld);
  
  // Purge arenas
  _pa_arenas_collect(true /* force */);
}

static void* pa_background_purge_worker(void* arg) {
  PA_UNUSED(arg);
  while (!pa_atomic_load_relaxed(&pa_process_done_started)) {
    _pa_purge_background();
    long delay = pa_option_get_clamp(pa_option_purge_delay, 1, 10000);
    usleep((useconds_t)delay * 1000);
  }
  return NULL;
}
#endif

static void pa_heap_main_init(void) {
  if (_pa_heap_main.cookie == 0) {
    _pa_heap_main.thread_id = _pa_thread_id();
    _pa_heap_main.cookie = 1;
    #if defined(_WIN32) && !defined(PA_SHARED_LIB)
      _pa_random_init_weak(&_pa_heap_main.random);    // prevent allocation failure during bcrypt dll initialization with static linking
    #else
      _pa_random_init(&_pa_heap_main.random);
    #endif
    _pa_heap_main.cookie  = _pa_heap_random_next(&_pa_heap_main);
    _pa_heap_main.keys[0] = _pa_heap_random_next(&_pa_heap_main);
    _pa_heap_main.keys[1] = _pa_heap_random_next(&_pa_heap_main);
    pa_lock_init(&pa_subproc_default.abandoned_os_lock);
    pa_lock_init(&pa_subproc_default.abandoned_os_visit_lock);
    _pa_heap_guarded_init(&_pa_heap_main);
  }
}

pa_heap_t* _pa_heap_main_get(void) {
  pa_heap_main_init();
  return &_pa_heap_main;
}

/* -----------------------------------------------------------
  Sub process
----------------------------------------------------------- */

pa_subproc_id_t pa_subproc_main(void) {
  return NULL;
}

pa_subproc_id_t pa_subproc_new(void) {
  pa_memid_t memid = _pa_memid_none();
  pa_subproc_t* subproc = (pa_subproc_t*)_pa_arena_meta_zalloc(sizeof(pa_subproc_t), &memid);
  if (subproc == NULL) return NULL;
  subproc->memid = memid;
  subproc->abandoned_os_list = NULL;
  pa_lock_init(&subproc->abandoned_os_lock);
  pa_lock_init(&subproc->abandoned_os_visit_lock);
  return subproc;
}

pa_subproc_t* _pa_subproc_from_id(pa_subproc_id_t subproc_id) {
  return (subproc_id == NULL ? &pa_subproc_default : (pa_subproc_t*)subproc_id);
}

void pa_subproc_delete(pa_subproc_id_t subproc_id) {
  if (subproc_id == NULL) return;
  pa_subproc_t* subproc = _pa_subproc_from_id(subproc_id);
  // check if there are no abandoned segments still..
  bool safe_to_delete = false;
  pa_lock(&subproc->abandoned_os_lock) {
    if (subproc->abandoned_os_list == NULL) {
      safe_to_delete = true;
    }
  }
  if (!safe_to_delete) return;
  // safe to release
  // todo: should we refcount subprocesses?
  pa_lock_done(&subproc->abandoned_os_lock);
  pa_lock_done(&subproc->abandoned_os_visit_lock);
  _pa_arena_meta_free(subproc, subproc->memid, sizeof(pa_subproc_t));
}

void pa_subproc_add_current_thread(pa_subproc_id_t subproc_id) {
  pa_heap_t* heap = pa_heap_get_default();
  if (heap == NULL) return;
  pa_assert(heap->tld->segments.subproc == &pa_subproc_default);
  if (heap->tld->segments.subproc != &pa_subproc_default) return;
  heap->tld->segments.subproc = _pa_subproc_from_id(subproc_id);
}



/* -----------------------------------------------------------
  Initialization and freeing of the thread local heaps
----------------------------------------------------------- */

// note: in x64 in release build `sizeof(pa_thread_data_t)` is under 4KiB (= OS page size).
typedef struct pa_thread_data_s {
  pa_heap_t  heap;   // must come first due to cast in `_pa_heap_done`
  pa_tld_t   tld;
  pa_memid_t memid;  // must come last due to zero'ing
} pa_thread_data_t;


// Thread meta-data is allocated directly from the OS. For
// some programs that do not use thread pools and allocate and
// destroy many OS threads, this may causes too much overhead
// per thread so we maintain a small cache of recently freed metadata.

#define TD_CACHE_SIZE (32)
static _Atomic(pa_thread_data_t*) td_cache[TD_CACHE_SIZE];

static pa_thread_data_t* pa_thread_data_zalloc(void) {
  // try to find thread metadata in the cache
  pa_thread_data_t* td = NULL;
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    td = pa_atomic_load_ptr_relaxed(pa_thread_data_t, &td_cache[i]);
    if (td != NULL) {
      // found cached allocation, try use it
      td = pa_atomic_exchange_ptr_acq_rel(pa_thread_data_t, &td_cache[i], NULL);
      if (td != NULL) {
        _pa_memzero(td, offsetof(pa_thread_data_t,memid));
        return td;
      }
    }
  }

  // if that fails, allocate as meta data
  pa_memid_t memid;
  td = (pa_thread_data_t*)_pa_os_zalloc(sizeof(pa_thread_data_t), &memid);
  if (td == NULL) {
    // if this fails, try once more. (issue #257)
    td = (pa_thread_data_t*)_pa_os_zalloc(sizeof(pa_thread_data_t), &memid);
    if (td == NULL) {
      // really out of memory
      _pa_error_message(ENOMEM, "unable to allocate thread local heap metadata (%zu bytes)\n", sizeof(pa_thread_data_t));
      return NULL;
    }
  }
  td->memid = memid;
  return td;
}

static void pa_thread_data_free( pa_thread_data_t* tdfree ) {
  // try to add the thread metadata to the cache
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    pa_thread_data_t* td = pa_atomic_load_ptr_relaxed(pa_thread_data_t, &td_cache[i]);
    if (td == NULL) {
      pa_thread_data_t* expected = NULL;
      if (pa_atomic_cas_ptr_weak_acq_rel(pa_thread_data_t, &td_cache[i], &expected, tdfree)) {
        return;
      }
    }
  }
  // if that fails, just free it directly
  _pa_os_free(tdfree, sizeof(pa_thread_data_t), tdfree->memid);
}

void _pa_thread_data_collect(void) {
  // free all thread metadata from the cache
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    pa_thread_data_t* td = pa_atomic_load_ptr_relaxed(pa_thread_data_t, &td_cache[i]);
    if (td != NULL) {
      td = pa_atomic_exchange_ptr_acq_rel(pa_thread_data_t, &td_cache[i], NULL);
      if (td != NULL) {
        _pa_os_free(td, sizeof(pa_thread_data_t), td->memid);
      }
    }
  }
}

// Initialize the thread local default heap, called from `pa_thread_init`
static bool _pa_thread_heap_init(void) {
  if (pa_heap_is_initialized(pa_prim_get_default_heap())) return true;
  if (_pa_is_main_thread()) {
    // pa_assert_internal(_pa_heap_main.thread_id != 0);  // can happen on freeBSD where alloc is called before any initialization
    // the main heap is statically allocated
    pa_heap_main_init();
    _pa_heap_set_default_direct(&_pa_heap_main);
    //pa_assert_internal(_pa_heap_default->tld->heap_backing == pa_prim_get_default_heap());
  }
  else {
    // use `_pa_os_alloc` to allocate directly from the OS
    pa_thread_data_t* td = pa_thread_data_zalloc();
    if (td == NULL) return false;

    pa_tld_t*  tld = &td->tld;
    pa_heap_t* heap = &td->heap;
    _pa_tld_init(tld, heap);  // must be before `_pa_heap_init`
    _pa_heap_init(heap, tld, _pa_arena_id_none(), false /* can reclaim */, 0 /* default tag */);
    _pa_heap_set_default_direct(heap);
  }
  return false;
}

// initialize thread local data
void _pa_tld_init(pa_tld_t* tld, pa_heap_t* bheap) {
  _pa_memcpy_aligned(tld, &tld_empty, sizeof(pa_tld_t));
  tld->heap_backing = bheap;
  tld->heaps = NULL;
  tld->segments.subproc = &pa_subproc_default;
  tld->segments.stats = &tld->stats;
}

// Free the thread local default heap (called from `pa_thread_done`)
static bool _pa_thread_heap_done(pa_heap_t* heap) {
  if (!pa_heap_is_initialized(heap)) return true;

  // reset default heap
  _pa_heap_set_default_direct(_pa_is_main_thread() ? &_pa_heap_main : (pa_heap_t*)&_pa_heap_empty);

  // switch to backing heap
  heap = heap->tld->heap_backing;
  if (!pa_heap_is_initialized(heap)) return false;

  // delete all non-backing heaps in this thread
  pa_heap_t* curr = heap->tld->heaps;
  while (curr != NULL) {
    pa_heap_t* next = curr->next; // save `next` as `curr` will be freed
    if (curr != heap) {
      pa_assert_internal(!pa_heap_is_backing(curr));
      pa_heap_delete(curr);
    }
    curr = next;
  }
  pa_assert_internal(heap->tld->heaps == heap && heap->next == NULL);
  pa_assert_internal(pa_heap_is_backing(heap));

  // collect if not the main thread
  if (heap != &_pa_heap_main) {
    _pa_heap_collect_abandon(heap);
  }

  // merge stats
  _pa_stats_done(&heap->tld->stats);

  // free if not the main thread
  if (heap != &_pa_heap_main) {
    // the following assertion does not always hold for huge segments as those are always treated
    // as abondened: one may allocate it in one thread, but deallocate in another in which case
    // the count can be too large or negative. todo: perhaps not count huge segments? see issue #363
    // pa_assert_internal(heap->tld->segments.count == 0 || heap->thread_id != _pa_thread_id());
    pa_thread_data_free((pa_thread_data_t*)heap);
  }
  else {
    #if 0
    // never free the main thread even in debug mode; if a dll is linked statically with palloc,
    // there may still be delete/free calls after the pa_fls_done is called. Issue #207
    _pa_heap_destroy_pages(heap);
    pa_assert_internal(heap->tld->heap_backing == &_pa_heap_main);
    #endif
  }
  return false;
}



// --------------------------------------------------------
// Try to run `pa_thread_done()` automatically so any memory
// owned by the thread but not yet released can be abandoned
// and re-owned by another thread.
//
// 1. windows dynamic library:
//     call from DllMain on DLL_THREAD_DETACH
// 2. windows static library:
//     use `FlsAlloc` to call a destructor when the thread is done
// 3. unix, pthreads:
//     use a pthread key to call a destructor when a pthread is done
//
// In the last two cases we also need to call `pa_process_init`
// to set up the thread local keys.
// --------------------------------------------------------

// Set up handlers so `pa_thread_done` is called automatically
static void pa_process_setup_auto_thread_done(void) {
  static bool tls_initialized = false; // fine if it races
  if (tls_initialized) return;
  tls_initialized = true;
  _pa_prim_thread_init_auto_done();
  _pa_heap_set_default_direct(&_pa_heap_main);
}


bool _pa_is_main_thread(void) {
  return (_pa_heap_main.thread_id==0 || _pa_heap_main.thread_id == _pa_thread_id());
}

static _Atomic(size_t) thread_count = PA_ATOMIC_VAR_INIT(1);

size_t  _pa_current_thread_count(void) {
  return pa_atomic_load_relaxed(&thread_count);
}

// This is called from the `pa_malloc_generic`
void pa_thread_init(void) pa_attr_noexcept
{
  // ensure our process has started already
  pa_process_init();

  // initialize the thread local default heap
  // (this will call `_pa_heap_set_default_direct` and thus set the
  //  fiber/pthread key to a non-zero value, ensuring `_pa_thread_done` is called)
  if (_pa_thread_heap_init()) return;  // returns true if already initialized

  _pa_stat_increase(&_pa_stats_main.threads, 1);
  pa_atomic_increment_relaxed(&thread_count);
  //_pa_verbose_message("thread init: 0x%zx\n", _pa_thread_id());
}

void pa_thread_done(void) pa_attr_noexcept {
  _pa_thread_done(NULL);
}

void _pa_thread_done(pa_heap_t* heap)
{
  // calling with NULL implies using the default heap
  if (heap == NULL) {
    heap = pa_prim_get_default_heap();
    if (heap == NULL) return;
  }

  // prevent re-entrancy through heap_done/heap_set_default_direct (issue #699)
  if (!pa_heap_is_initialized(heap)) {
    return;
  }

  // adjust stats
  pa_atomic_decrement_relaxed(&thread_count);
  _pa_stat_decrease(&_pa_stats_main.threads, 1);

  // check thread-id as on Windows shutdown with FLS the main (exit) thread may call this on thread-local heaps...
  if (heap->thread_id != _pa_thread_id()) return;

  // abandon the thread local heap
  if (_pa_thread_heap_done(heap)) return;  // returns true if already ran
}

void _pa_heap_set_default_direct(pa_heap_t* heap)  {
  pa_assert_internal(heap != NULL);
  #if defined(PA_TLS_SLOT)
  pa_prim_tls_slot_set(PA_TLS_SLOT,heap);
  #elif defined(PA_TLS_PTHREAD_SLOT_OFS)
  *pa_prim_tls_pthread_heap_slot() = heap;
  #elif defined(PA_TLS_PTHREAD)
  // we use _pa_heap_default_key
  #else
  _pa_heap_default = heap;
  #endif

  // ensure the default heap is passed to `_pa_thread_done`
  // setting to a non-NULL value also ensures `pa_thread_done` is called.
  _pa_prim_thread_associate_default_heap(heap);
}

void pa_thread_set_in_threadpool(void) pa_attr_noexcept {
  // nothing
}

// --------------------------------------------------------
// Run functions on process init/done, and thread init/done
// --------------------------------------------------------
static bool os_preloading = true;    // true until this module is initialized

// Returns true if this module has not been initialized; Don't use C runtime routines until it returns false.
bool pa_decl_noinline _pa_preloading(void) {
  return os_preloading;
}

// Returns true if palloc was redirected
pa_decl_nodiscard bool pa_is_redirected(void) pa_attr_noexcept {
  return _pa_is_redirected();
}

// Called once by the process loader from `src/prim/prim.c`
void _pa_auto_process_init(void) {
  pa_heap_main_init();
  #if defined(__APPLE__) || defined(PA_TLS_RECURSE_GUARD)
  volatile pa_heap_t* dummy = _pa_heap_default; // access TLS to allocate it before setting tls_initialized to true;
  if (dummy == NULL) return;                    // use dummy or otherwise the access may get optimized away (issue #697)
  #endif
  os_preloading = false;
  pa_assert_internal(_pa_is_main_thread());
  _pa_options_init();
  pa_process_setup_auto_thread_done();
  pa_process_init();
  if (_pa_is_redirected()) _pa_verbose_message("malloc is redirected.\n");

  // show message from the redirector (if present)
  const char* msg = NULL;
  _pa_allocator_init(&msg);
  if (msg != NULL && (pa_option_is_enabled(pa_option_verbose) || pa_option_is_enabled(pa_option_show_errors))) {
    _pa_fputs(NULL,NULL,NULL,msg);
  }

  // reseed random
  _pa_random_reinit_if_weak(&_pa_heap_main.random);
}

#if (defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__))
#if defined(_MSC_VER)
#include <intrin.h>
static void _pa_cpuid(int32_t cpu_info[4], int32_t eax) {
  __cpuidex(cpu_info, eax, 0);
}
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <immintrin.h>
static void _pa_cpuid(int32_t cpu_info[4], int32_t eax) {
  uint32_t ex, bx, cx, dx;
  __cpuid_count((uint32_t)eax, 0, ex, bx, cx, dx);
  cpu_info[0] = (int32_t)ex; cpu_info[1] = (int32_t)bx; cpu_info[2] = (int32_t)cx; cpu_info[3] = (int32_t)dx;
}
#endif
pa_decl_cache_align bool _pa_cpu_has_fsrm = false;
pa_decl_cache_align bool _pa_cpu_has_erms = false;
pa_decl_cache_align bool _pa_cpu_has_avx2 = false;

static void pa_detect_cpu_features(void) {
  // FSRM for fast short rep movsb/stosb support (AMD Zen3+ (~2020) or Intel Ice Lake+ (~2017))
  // EMRS for fast enhanced rep movsb/stosb support
  // AVX2 for fast vector operations
  int32_t cpu_info[4];
  _pa_cpuid(cpu_info, 7);
  _pa_cpu_has_fsrm = ((cpu_info[3] & (1 << 4)) != 0); // bit 4 of EDX
  _pa_cpu_has_erms = ((cpu_info[1] & (1 << 9)) != 0); // bit 9 of EBX
  _pa_cpu_has_avx2 = ((cpu_info[1] & (1 << 5)) != 0); // bit 5 of EBX
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
void _pa_memzero_aligned_avx2(void* dst, size_t n) {
  uint8_t* p = (uint8_t*)dst;
  __m256i z = _mm256_setzero_si256();
  
  // For very large blocks, use non-temporal stores to avoid cache pollution
  if (n >= 128*1024) { 
    while (n >= 64) {
      _mm256_stream_si256((__m256i*)p, z);
      _mm256_stream_si256((__m256i*)(p+32), z);
      p += 64;
      n -= 64;
    }
    _mm_sfence(); // Ensure stream stores are visible
  }
  else {
    // For smaller blocks, regular stores are better for cache locality
    while (n >= 64) {
      _mm256_store_si256((__m256i*)p, z);
      _mm256_store_si256((__m256i*)(p+32), z);
      p += 64;
      n -= 64;
    }
  }

  if (n >= 32) {
    _mm256_store_si256((__m256i*)p, z);
    p += 32;
    n -= 32;
  }
  if (n > 0) _pa_memzero(p, n);
}
#else
static void pa_detect_cpu_features(void) {
  // nothing
}
#endif

// Initialize the process; called by thread_init or the process loader
void pa_process_init(void) pa_attr_noexcept {
  static _Atomic(uintptr_t) process_init_state = PA_ATOMIC_VAR_INIT(0);
  static _Atomic(pa_threadid_t) process_init_thread = PA_ATOMIC_VAR_INIT(0);

  if (pa_atomic_load_acquire(&process_init_state) == 2) return;
  if (pa_atomic_load_relaxed(&process_init_thread) == _pa_thread_id()) return;

  uintptr_t expected = 0;
  if (pa_atomic_cas_strong_acq_rel(&process_init_state, &expected, (uintptr_t)1)) {
    pa_atomic_store_relaxed(&process_init_thread, _pa_thread_id());
	#if _MSC_VER < 1920
	pa_heap_main_init(); // vs2017 can dynamically re-initialize _pa_heap_main
	#endif
    _pa_process_is_initialized = true;
    pa_process_setup_auto_thread_done();

    pa_detect_cpu_features();
    _pa_os_init();
    pa_heap_main_init();
    _pa_size2bin_table_init();  // O(1) size->bin lookup for common sizes
    pa_thread_init();

    #if defined(_WIN32)
    // On windows, when building as a static lib the FLS cleanup happens to early for the main thread.
    // To avoid this, set the FLS value for the main thread to NULL so the fls cleanup
    // will not call _pa_thread_done on the (still executing) main thread. See issue #508.
    _pa_prim_thread_associate_default_heap(NULL);
    #endif

    pa_stats_reset();  // only call stat reset *after* thread init (or the heap tld == NULL)
    pa_track_init();

    if (pa_option_is_enabled(pa_option_reserve_huge_os_pages)) {
      size_t pages = pa_option_get_clamp(pa_option_reserve_huge_os_pages, 0, 128*1024);
      int reserve_at  = (int)pa_option_get_clamp(pa_option_reserve_huge_os_pages_at, -1, INT_MAX);
      if (reserve_at != -1) {
        pa_reserve_huge_os_pages_at(pages, reserve_at, pages*500);
      } else {
        pa_reserve_huge_os_pages_interleave(pages, 0, pages*500);
      }
    }
    if (pa_option_is_enabled(pa_option_reserve_os_memory)) {
      long ksize = pa_option_get(pa_option_reserve_os_memory);
      if (ksize > 0) {
        pa_reserve_os_memory((size_t)ksize*PA_KiB, true /* commit? */, true /* allow large pages? */);
      }
    }

  #if defined(PA_USE_PTHREADS)
    if (pa_option_is_enabled(pa_option_purge_background)) {
      int err = pthread_create(&pa_background_thread, NULL, pa_background_purge_worker, NULL);
      if (err == 0) {
        pa_atomic_store_release(&pa_background_thread_running, true);
      } else {
        _pa_error_message(err, "failed to create background purge thread\n");
      }
    } else {
    }
  #else
  #endif
    pa_atomic_store_release(&process_init_state, 2);
    return;
  }

  // Another thread is initializing; wait until it finishes:
  while (pa_atomic_load_acquire(&process_init_state) != 2) {
    pa_atomic_yield();
  }
}

// Called when the process is done (cdecl as it is used with `at_exit` on some platforms)
void pa_cdecl pa_process_done(void) pa_attr_noexcept {
  // only shutdown if we were initialized
  if (!_pa_process_is_initialized) return;
  // ensure we are called once
  static bool process_done = false;
  if (process_done) return;
  process_done = true;
  pa_atomic_store_release(&pa_process_done_started, true);

#if defined(PA_USE_PTHREADS)
  if (pa_atomic_load_relaxed(&pa_background_thread_running)) {
    pthread_join(pa_background_thread, NULL);
    pa_atomic_store_release(&pa_background_thread_running, false);
  }
#endif

  // get the default heap so we don't need to acces thread locals anymore
  pa_heap_t* heap = pa_prim_get_default_heap();  // use prim to not initialize any heap
  pa_assert_internal(heap != NULL);

  // release any thread specific resources and ensure _pa_thread_done is called on all but the main thread
  _pa_prim_thread_done_auto_done();


  #ifndef PA_SKIP_COLLECT_ON_EXIT
    #if (PA_DEBUG || !defined(PA_SHARED_LIB))
    // free all memory if possible on process exit. This is not needed for a stand-alone process
    // but should be done if palloc is statically linked into another shared library which
    // is repeatedly loaded/unloaded, see issue #281.
    pa_heap_collect(heap, true /* force */ );
    #endif
  #endif

  // Forcefully release all retained memory; this can be dangerous in general if overriding regular malloc/free
  // since after process_done there might still be other code running that calls `free` (like at_exit routines,
  // or C-runtime termination code.
  if (pa_option_is_enabled(pa_option_destroy_on_exit)) {
    pa_heap_collect(heap, true /* force */);
    _pa_heap_unsafe_destroy_all(heap);     // forcefully release all memory held by all heaps (of this thread only!)
    _pa_arena_unsafe_destroy_all();
    _pa_segment_map_unsafe_destroy();
  }

  if (pa_option_is_enabled(pa_option_show_stats) || pa_option_is_enabled(pa_option_verbose)) {
    pa_stats_print(NULL);
  }
  _pa_allocator_done();
  os_preloading = true; // don't call the C runtime anymore
}

void pa_cdecl _pa_auto_process_done(void) pa_attr_noexcept {
  if (_pa_option_get_fast(pa_option_destroy_on_exit)>1) return;
  pa_process_done();
}
