/* ----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // for realpath() on Linux
#endif

#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/atomic.h"
#include "palloc/prim.h"   // _pa_prim_thread_id()

#include <string.h>      // memset, strlen (for pa_strdup)
#include <stdlib.h>      // malloc, abort

#define PA_IN_ALLOC_C
#include "alloc-override.c"
#include "free.c"
#undef PA_IN_ALLOC_C

// ------------------------------------------------------
// Allocation
// ------------------------------------------------------

// Fast allocation in a page: just pop from the free list.
// Fall back to generic allocation only if the list is empty.
// Note: in release mode the (inlined) routine is about 7 instructions with a single test.
extern inline void* _pa_page_malloc_zero(pa_heap_t* heap, pa_page_t* page, size_t size, bool zero, size_t* usable) pa_attr_noexcept
{
  pa_assert_internal(size >= PA_PADDING_SIZE);
  pa_assert_internal(page->block_size == 0 /* empty heap */ || pa_page_block_size(page) >= size);

  // check the free list
  pa_block_t* const block = page->free;
  if pa_unlikely(block == NULL) {
    return _pa_malloc_generic(heap, size, zero, 0, usable);
  }
  pa_assert_internal(block != NULL && _pa_ptr_page(block) == page);
  // pop from the free list
  pa_block_t* const next = pa_block_next(page, block);
  page->free = next;
  page->used++;
  if (next != NULL) { pa_prefetch(next); }
  pa_assert_internal(page->free == NULL || _pa_ptr_page(page->free) == page);
  pa_assert_internal(page->block_size < PA_MAX_ALIGN_SIZE || _pa_is_aligned(block, PA_MAX_ALIGN_SIZE));
  _pa_page_finish_alloc(heap, page, block, size, zero, usable);
  return block;
}

// Shared post-pop setup: usable size, track, zero, stats, padding (used by page free list and thread cache).
void _pa_page_finish_alloc(pa_heap_t* heap, pa_page_t* page, pa_block_t* block, size_t size, bool zero, size_t* usable) {
  PA_UNUSED(heap);
  PA_UNUSED(size);
  if (usable != NULL) { *usable = pa_page_usable_block_size(page); }
  #if PA_DEBUG>3
  if (page->free_is_zero && size > sizeof(*block)) {
    pa_assert_expensive(pa_mem_is_zero(block+1,size - sizeof(*block)));
  }
  #endif
  pa_track_mem_undefined(block, pa_page_usable_block_size(page));
  if pa_unlikely(zero) {
    pa_assert_internal(page->block_size != 0);
    #if PA_PADDING
    pa_assert_internal(page->block_size >= PA_PADDING_SIZE);
    #endif
    if (page->free_is_zero) {
      block->next = 0;
      pa_track_mem_defined(block, page->block_size - PA_PADDING_SIZE);
    }
    else {
      _pa_memzero_aligned(block, page->block_size - PA_PADDING_SIZE);
    }
  }
  #if (PA_DEBUG>0) && !PA_TRACK_ENABLED && !PA_TSAN
  if (!zero && !pa_page_is_huge(page)) {
    memset(block, PA_DEBUG_UNINIT, pa_page_usable_block_size(page));
  }
  #elif (PA_SECURE!=0)
  if (!zero) { block->next = 0; }
  #endif
  #if (PA_STAT>0)
  const size_t bsize = pa_page_usable_block_size(page);
  if (bsize <= PA_MEDIUM_OBJ_SIZE_MAX) {
    pa_heap_stat_increase(heap, malloc_normal, bsize);
    pa_heap_stat_counter_increase(heap, malloc_normal_count, 1);
    #if (PA_STAT>1)
    pa_heap_stat_increase(heap, malloc_bins[_pa_bin(bsize)], 1);
    pa_heap_stat_increase(heap, malloc_requested, size - PA_PADDING_SIZE);
    #endif
  }
  #endif
  heap->used_bytes += pa_page_usable_block_size(page);
  #if PA_PADDING
  {
    pa_padding_t* const padding = (pa_padding_t*)((uint8_t*)block + pa_page_usable_block_size(page));
    ptrdiff_t delta = ((uint8_t*)padding - (uint8_t*)block - (size - PA_PADDING_SIZE));
    #if (PA_DEBUG>=2)
    pa_assert_internal(delta >= 0 && pa_page_usable_block_size(page) >= (size - PA_PADDING_SIZE + delta));
    #endif
    pa_track_mem_defined(padding,sizeof(pa_padding_t));
    padding->canary = pa_ptr_encode_canary(page,block,page->keys);
    padding->delta  = (uint32_t)(delta);
    #if PA_PADDING_CHECK
    if (!pa_page_is_huge(page)) {
      uint8_t* fill = (uint8_t*)padding - delta;
      const size_t maxpad = (delta > PA_MAX_ALIGN_SIZE ? PA_MAX_ALIGN_SIZE : (size_t)delta);
      for (size_t i = 0; i < maxpad; i++) { fill[i] = PA_DEBUG_PADDING; }
    }
    #endif
  }
  #endif
}

// extra entries for improved efficiency in `alloc-aligned.c`.
extern void* _pa_page_malloc(pa_heap_t* heap, pa_page_t* page, size_t size) pa_attr_noexcept {
  return _pa_page_malloc_zero(heap,page,size,false,NULL);
}
extern void* _pa_page_malloc_zeroed(pa_heap_t* heap, pa_page_t* page, size_t size) pa_attr_noexcept {
  return _pa_page_malloc_zero(heap,page,size,true,NULL);
}

#if PA_GUARDED
pa_decl_restrict void* _pa_heap_malloc_guarded(pa_heap_t* heap, size_t size, bool zero) pa_attr_noexcept;
#endif

static inline pa_decl_restrict void* pa_heap_malloc_small_zero(pa_heap_t* heap, size_t size, bool zero, size_t* usable) pa_attr_noexcept {
  pa_assert(heap != NULL);
  pa_assert(size <= PA_SMALL_SIZE_MAX);
  #if PA_DEBUG
  const uintptr_t tid = _pa_thread_id();
  pa_assert(heap->thread_id == 0 || heap->thread_id == tid); // heaps are thread local
  #endif
  #if (PA_PADDING || PA_GUARDED)
  if (size == 0) { size = sizeof(void*); }
  #endif
  #if PA_GUARDED
  if (pa_heap_malloc_use_guarded(heap,size)) {
    return _pa_heap_malloc_guarded(heap, size, zero);
  }
  #endif

  if (heap->max_size != 0 && (size + PA_PADDING_SIZE) > heap->max_size - heap->used_bytes) {
    if (heap->pressure_cb != NULL)
      heap->pressure_cb(heap->used_bytes, heap->max_size, heap->pressure_arg);
    return NULL;
  }

  // get page in constant time, and allocate from it
  pa_page_t* page = _pa_heap_get_free_small_page(heap, size + PA_PADDING_SIZE);
  void* const p = _pa_page_malloc_zero(heap, page, size + PA_PADDING_SIZE, zero, usable);
  pa_track_malloc(p,size,zero);

  #if PA_DEBUG>3
  if (p != NULL && zero) {
    pa_assert_expensive(pa_mem_is_zero(p, size));
  }
  #endif
  return p;
}

// allocate a small block
pa_decl_nodiscard extern inline pa_decl_restrict void* pa_heap_malloc_small(pa_heap_t* heap, size_t size) pa_attr_noexcept {
  return pa_heap_malloc_small_zero(heap, size, false, NULL);
}

pa_decl_nodiscard extern inline pa_decl_restrict void* pa_malloc_small(size_t size) pa_attr_noexcept {
  return pa_heap_malloc_small(pa_prim_get_default_heap(), size);
}

// The main allocation function
extern inline void* _pa_heap_malloc_zero_ex(pa_heap_t* heap, size_t size, bool zero, size_t huge_alignment, size_t* usable) pa_attr_noexcept {
  // fast path for small objects
  if pa_likely(size <= PA_SMALL_SIZE_MAX) {
    pa_assert_internal(huge_alignment == 0);
    return pa_heap_malloc_small_zero(heap, size, zero, usable);
  }
  #if PA_GUARDED
  else if (huge_alignment==0 && pa_heap_malloc_use_guarded(heap,size)) {
    return _pa_heap_malloc_guarded(heap, size, zero);
  }
  #endif
  else {
    // regular allocation
    pa_assert(heap!=NULL);
    pa_assert(heap->thread_id == 0 || heap->thread_id == _pa_thread_id());   // heaps are thread local
    void* const p = _pa_malloc_generic(heap, size + PA_PADDING_SIZE, zero, huge_alignment, usable);  // note: size can overflow but it is detected in malloc_generic
    pa_track_malloc(p,size,zero);

    #if PA_DEBUG>3
    if (p != NULL && zero) {
      pa_assert_expensive(pa_mem_is_zero(p, size));
    }
    #endif
    return p;
  }
}

extern inline void* _pa_heap_malloc_zero(pa_heap_t* heap, size_t size, bool zero) pa_attr_noexcept {
  return _pa_heap_malloc_zero_ex(heap, size, zero, 0, NULL);
}

pa_decl_nodiscard extern inline pa_decl_restrict void* pa_heap_malloc(pa_heap_t* heap, size_t size) pa_attr_noexcept {
  return _pa_heap_malloc_zero(heap, size, false);
}

pa_decl_nodiscard extern inline pa_decl_restrict void* pa_malloc(size_t size) pa_attr_noexcept {
  pa_heap_t* heap = pa_prim_get_default_heap();
  if pa_likely(size <= PA_SMALL_SIZE_MAX) {
    return pa_heap_malloc_small_zero(heap, size, false, NULL);
  }
  return _pa_heap_malloc_zero(heap, size, false);
}

// zero initialized small block
pa_decl_nodiscard pa_decl_restrict void* pa_zalloc_small(size_t size) pa_attr_noexcept {
  return pa_heap_malloc_small_zero(pa_prim_get_default_heap(), size, true, NULL);
}

pa_decl_nodiscard extern inline pa_decl_restrict void* pa_heap_zalloc(pa_heap_t* heap, size_t size) pa_attr_noexcept {
  return _pa_heap_malloc_zero(heap, size, true);
}

pa_decl_nodiscard pa_decl_restrict void* pa_zalloc(size_t size) pa_attr_noexcept {
  return pa_heap_zalloc(pa_prim_get_default_heap(),size);
}


pa_decl_nodiscard extern inline pa_decl_restrict void* pa_heap_calloc(pa_heap_t* heap, size_t count, size_t size) pa_attr_noexcept {
  size_t total;
  if (pa_count_size_overflow(count,size,&total)) return NULL;
  if pa_likely(total <= PA_SMALL_SIZE_MAX) {
    return pa_heap_malloc_small_zero(heap, total, true, NULL);
  }
  return _pa_heap_malloc_zero(heap, total, true);
}

pa_decl_nodiscard pa_decl_restrict void* pa_calloc(size_t count, size_t size) pa_attr_noexcept {
  return pa_heap_calloc(pa_prim_get_default_heap(),count,size);
}

// Return usable size
pa_decl_nodiscard pa_decl_restrict void* pa_umalloc_small(size_t size, size_t* usable) pa_attr_noexcept {
  return pa_heap_malloc_small_zero(pa_prim_get_default_heap(), size, false, usable);
}

pa_decl_nodiscard pa_decl_restrict void* pa_heap_umalloc(pa_heap_t* heap, size_t size, size_t* usable) pa_attr_noexcept {
  return _pa_heap_malloc_zero_ex(heap, size, false, 0, usable);
}

pa_decl_nodiscard pa_decl_restrict void* pa_umalloc(size_t size, size_t* usable) pa_attr_noexcept {
  return pa_heap_umalloc(pa_prim_get_default_heap(), size, usable);
}

pa_decl_nodiscard pa_decl_restrict void* pa_uzalloc(size_t size, size_t* usable) pa_attr_noexcept {
  return _pa_heap_malloc_zero_ex(pa_prim_get_default_heap(), size, true, 0, usable);
}

pa_decl_nodiscard pa_decl_restrict void* pa_ucalloc(size_t count, size_t size, size_t* usable) pa_attr_noexcept {
  size_t total;
  if (pa_count_size_overflow(count,size,&total)) return NULL;
  return pa_uzalloc(total, usable);
}

// Uninitialized `calloc`
pa_decl_nodiscard extern pa_decl_restrict void* pa_heap_mallocn(pa_heap_t* heap, size_t count, size_t size) pa_attr_noexcept {
  size_t total;
  if (pa_count_size_overflow(count, size, &total)) return NULL;
  return pa_heap_malloc(heap, total);
}

pa_decl_nodiscard pa_decl_restrict void* pa_mallocn(size_t count, size_t size) pa_attr_noexcept {
  return pa_heap_mallocn(pa_prim_get_default_heap(),count,size);
}

// Expand (or shrink) in place (or fail)
void* pa_expand(void* p, size_t newsize) pa_attr_noexcept {
  #if PA_PADDING
  // we do not shrink/expand with padding enabled
  PA_UNUSED(p); PA_UNUSED(newsize);
  return NULL;
  #else
  if (p == NULL) return NULL;
  const pa_page_t* const page = pa_validate_ptr_page(p,"pa_expand");  
  const size_t size = _pa_usable_size(p,page);
  if (newsize > size) return NULL;
  return p; // it fits
  #endif
}

void* _pa_heap_realloc_zero(pa_heap_t* heap, void* p, size_t newsize, bool zero, size_t* usable_pre, size_t* usable_post) pa_attr_noexcept {
  // if p == NULL then behave as malloc.
  // else if size == 0 then reallocate to a zero-sized block (and don't return NULL, just as pa_malloc(0)).
  // (this means that returning NULL always indicates an error, and `p` will not have been freed in that case.)
  const pa_page_t* page;
  size_t size;
  if (p==NULL) {
    page = NULL;
    size = 0;
    if (usable_pre!=NULL) { *usable_pre = 0; }
  }
  else {    
    page = pa_validate_ptr_page(p,"pa_realloc");  
    size = _pa_usable_size(p,page);
    if (usable_pre!=NULL) { *usable_pre = pa_page_usable_block_size(page); }    
  }
  /* In-place: same pointer if shrink or moderate shrink (up to 25% waste). */
  if pa_unlikely(newsize <= size && newsize > 0 && newsize >= (size - (size >> 2))) {  // newsize >= 3/4 * size
    pa_assert_internal(p!=NULL);
    if (usable_post!=NULL) { *usable_post = pa_page_usable_block_size(page); }
    return p;
  }
  /* In-place expand when the block is large and the segment has contiguous free space. */
  if (p != NULL && newsize > size && newsize <= PA_LARGE_OBJ_SIZE_MAX && page->slice_count > 1) {
    const size_t new_slices = (newsize + PA_SEGMENT_SLICE_SIZE - 1) / PA_SEGMENT_SLICE_SIZE;
    const size_t need_slices = (new_slices > page->slice_count) ? (new_slices - page->slice_count) : 0;
    if (need_slices > 0) {
      pa_segment_t* const segment = _pa_ptr_segment(p);
      if (_pa_segment_try_extend_span(segment, (pa_page_t*)page, need_slices, &heap->tld->segments)) {
        if (zero)
          _pa_memzero((uint8_t*)p + size, newsize - size);
        if (usable_post != NULL) { *usable_post = pa_page_usable_block_size(page); }
        return p;
      }
    }
  }
  void* newp = pa_heap_umalloc(heap,newsize,usable_post);
  if pa_likely(newp != NULL) {
    if (zero && newsize > size) {
      _pa_memzero((uint8_t*)newp + size, newsize - size);
    }
    else if (newsize == 0) {
      ((uint8_t*)newp)[0] = 0; // work around for applications that expect zero-reallocation to be zero initialized (issue #725)
    }
    if pa_likely(p != NULL) {
      const size_t copysize = (newsize > size ? size : newsize);
      pa_track_mem_defined(p,copysize);
      if (copysize >= sizeof(intptr_t) && ((uintptr_t)p % PA_INTPTR_SIZE == 0) && ((uintptr_t)newp % PA_INTPTR_SIZE == 0))
        _pa_memcpy_aligned(newp, p, copysize);
      else
        _pa_memcpy(newp, p, copysize);
      pa_free(p);
    }
  }
  return newp;
}

pa_decl_nodiscard void* pa_heap_realloc(pa_heap_t* heap, void* p, size_t newsize) pa_attr_noexcept {
  return _pa_heap_realloc_zero(heap, p, newsize, false, NULL, NULL);
}

pa_decl_nodiscard void* pa_heap_reallocn(pa_heap_t* heap, void* p, size_t count, size_t size) pa_attr_noexcept {
  size_t total;
  if (pa_count_size_overflow(count, size, &total)) return NULL;
  return pa_heap_realloc(heap, p, total);
}


// Reallocate but free `p` on errors
pa_decl_nodiscard void* pa_heap_reallocf(pa_heap_t* heap, void* p, size_t newsize) pa_attr_noexcept {
  void* newp = pa_heap_realloc(heap, p, newsize);
  if (newp==NULL && p!=NULL) pa_free(p);
  return newp;
}

pa_decl_nodiscard void* pa_heap_rezalloc(pa_heap_t* heap, void* p, size_t newsize) pa_attr_noexcept {
  return _pa_heap_realloc_zero(heap, p, newsize, true, NULL, NULL);
}

pa_decl_nodiscard void* pa_heap_recalloc(pa_heap_t* heap, void* p, size_t count, size_t size) pa_attr_noexcept {
  size_t total;
  if (pa_count_size_overflow(count, size, &total)) return NULL;
  return pa_heap_rezalloc(heap, p, total);
}


pa_decl_nodiscard void* pa_realloc(void* p, size_t newsize) pa_attr_noexcept {
  return pa_heap_realloc(pa_prim_get_default_heap(),p,newsize);
}

pa_decl_nodiscard void* pa_reallocn(void* p, size_t count, size_t size) pa_attr_noexcept {
  return pa_heap_reallocn(pa_prim_get_default_heap(),p,count,size);
}

pa_decl_nodiscard void* pa_urealloc(void* p, size_t newsize, size_t* usable_pre, size_t* usable_post) pa_attr_noexcept {
  return _pa_heap_realloc_zero(pa_prim_get_default_heap(),p,newsize, false, usable_pre, usable_post);
}

// Reallocate but free `p` on errors
pa_decl_nodiscard void* pa_reallocf(void* p, size_t newsize) pa_attr_noexcept {
  return pa_heap_reallocf(pa_prim_get_default_heap(),p,newsize);
}

pa_decl_nodiscard void* pa_rezalloc(void* p, size_t newsize) pa_attr_noexcept {
  return pa_heap_rezalloc(pa_prim_get_default_heap(), p, newsize);
}

pa_decl_nodiscard void* pa_recalloc(void* p, size_t count, size_t size) pa_attr_noexcept {
  return pa_heap_recalloc(pa_prim_get_default_heap(), p, count, size);
}



// ------------------------------------------------------
// strdup, strndup, and realpath
// ------------------------------------------------------

// `strdup` using pa_malloc
pa_decl_nodiscard pa_decl_restrict char* pa_heap_strdup(pa_heap_t* heap, const char* s) pa_attr_noexcept {
  if (s == NULL) return NULL;
  size_t len = _pa_strlen(s);
  char* t = (char*)pa_heap_malloc(heap,len+1);
  if (t == NULL) return NULL;
  _pa_memcpy(t, s, len);
  t[len] = 0;
  return t;
}

pa_decl_nodiscard pa_decl_restrict char* pa_strdup(const char* s) pa_attr_noexcept {
  return pa_heap_strdup(pa_prim_get_default_heap(), s);
}

// `strndup` using pa_malloc
pa_decl_nodiscard pa_decl_restrict char* pa_heap_strndup(pa_heap_t* heap, const char* s, size_t n) pa_attr_noexcept {
  if (s == NULL) return NULL;
  const size_t len = _pa_strnlen(s,n);  // len <= n
  char* t = (char*)pa_heap_malloc(heap, len+1);
  if (t == NULL) return NULL;
  _pa_memcpy(t, s, len);
  t[len] = 0;
  return t;
}

pa_decl_nodiscard pa_decl_restrict char* pa_strndup(const char* s, size_t n) pa_attr_noexcept {
  return pa_heap_strndup(pa_prim_get_default_heap(),s,n);
}

#ifndef __wasi__
// `realpath` using pa_malloc
#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

pa_decl_nodiscard pa_decl_restrict char* pa_heap_realpath(pa_heap_t* heap, const char* fname, char* resolved_name) pa_attr_noexcept {
  // todo: use GetFullPathNameW to allow longer file names
  char buf[PATH_MAX];
  DWORD res = GetFullPathNameA(fname, PATH_MAX, (resolved_name == NULL ? buf : resolved_name), NULL);
  if (res == 0) {
    errno = GetLastError(); return NULL;
  }
  else if (res > PATH_MAX) {
    errno = EINVAL; return NULL;
  }
  else if (resolved_name != NULL) {
    return resolved_name;
  }
  else {
    return pa_heap_strndup(heap, buf, PATH_MAX);
  }
}
#else
/*
#include <unistd.h>  // pathconf
static size_t pa_path_max(void) {
  static size_t path_max = 0;
  if (path_max <= 0) {
    long m = pathconf("/",_PC_PATH_MAX);
    if (m <= 0) path_max = 4096;      // guess
    else if (m < 256) path_max = 256; // at least 256
    else path_max = m;
  }
  return path_max;
}
*/
char* pa_heap_realpath(pa_heap_t* heap, const char* fname, char* resolved_name) pa_attr_noexcept {
  if (resolved_name != NULL) {
    return realpath(fname,resolved_name);
  }
  else {
    char* rname = realpath(fname, NULL);
    if (rname == NULL) return NULL;
    char* result = pa_heap_strdup(heap, rname);
    pa_cfree(rname);  // use checked free (which may be redirected to our free but that's ok)
    // note: with ASAN realpath is intercepted and pa_cfree may leak the returned pointer :-(
    return result;
  }
  /*
    const size_t n  = pa_path_max();
    char* buf = (char*)pa_malloc(n+1);
    if (buf == NULL) {
      errno = ENOMEM;
      return NULL;
    }
    char* rname  = realpath(fname,buf);
    char* result = pa_heap_strndup(heap,rname,n); // ok if `rname==NULL`
    pa_free(buf);
    return result;
  }
  */
}
#endif

pa_decl_nodiscard pa_decl_restrict char* pa_realpath(const char* fname, char* resolved_name) pa_attr_noexcept {
  return pa_heap_realpath(pa_prim_get_default_heap(),fname,resolved_name);
}
#endif

/*-------------------------------------------------------
C++ new and new_aligned
The standard requires calling into `get_new_handler` and
throwing the bad_alloc exception on failure. If we compile
with a C++ compiler we can implement this precisely. If we
use a C compiler we cannot throw a `bad_alloc` exception
but we call `exit` instead (i.e. not returning).
-------------------------------------------------------*/

#ifdef __cplusplus
#include <new>
static bool pa_try_new_handler(bool nothrow) {
  #if defined(_MSC_VER) || (__cplusplus >= 201103L)
    std::new_handler h = std::get_new_handler();
  #else
    std::new_handler h = std::set_new_handler();
    std::set_new_handler(h);
  #endif
  if (h==NULL) {
    _pa_error_message(ENOMEM, "out of memory in 'new'");
    #if defined(_CPPUNWIND) || defined(__cpp_exceptions)  // exceptions are not always enabled
    if (!nothrow) {
      throw std::bad_alloc();
    }
    #else
    PA_UNUSED(nothrow);
    #endif
    return false;
  }
  else {
    h();
    return true;
  }
}
#else
typedef void (*std_new_handler_t)(void);

#if (defined(__GNUC__) || (defined(__clang__) && !defined(_MSC_VER)))  // exclude clang-cl, see issue #631
std_new_handler_t __attribute__((weak)) _ZSt15get_new_handlerv(void) {
  return NULL;
}
static std_new_handler_t pa_get_new_handler(void) {
  return _ZSt15get_new_handlerv();
}
#else
// note: on windows we could dynamically link to `?get_new_handler@std@@YAP6AXXZXZ`.
static std_new_handler_t pa_get_new_handler(void) {
  return NULL;
}
#endif

static bool pa_try_new_handler(bool nothrow) {
  std_new_handler_t h = pa_get_new_handler();
  if (h==NULL) {
    _pa_error_message(ENOMEM, "out of memory in 'new'");
    if (!nothrow) {
      abort();  // cannot throw in plain C, use abort
    }
    return false;
  }
  else {
    h();
    return true;
  }
}
#endif

pa_decl_export pa_decl_noinline void* pa_heap_try_new(pa_heap_t* heap, size_t size, bool nothrow ) {
  void* p = NULL;
  while(p == NULL && pa_try_new_handler(nothrow)) {
    p = pa_heap_malloc(heap,size);
  }
  return p;
}

static pa_decl_noinline void* pa_try_new(size_t size, bool nothrow) {
  return pa_heap_try_new(pa_prim_get_default_heap(), size, nothrow);
}


pa_decl_nodiscard pa_decl_restrict void* pa_heap_alloc_new(pa_heap_t* heap, size_t size) {
  void* p = pa_heap_malloc(heap,size);
  if pa_unlikely(p == NULL) return pa_heap_try_new(heap, size, false);
  return p;
}

pa_decl_nodiscard pa_decl_restrict void* pa_new(size_t size) {
  return pa_heap_alloc_new(pa_prim_get_default_heap(), size);
}


pa_decl_nodiscard pa_decl_restrict void* pa_heap_alloc_new_n(pa_heap_t* heap, size_t count, size_t size) {
  size_t total;
  if pa_unlikely(pa_count_size_overflow(count, size, &total)) {
    pa_try_new_handler(false);  // on overflow we invoke the try_new_handler once to potentially throw std::bad_alloc
    return NULL;
  }
  else {
    return pa_heap_alloc_new(heap,total);
  }
}

pa_decl_nodiscard pa_decl_restrict void* pa_new_n(size_t count, size_t size) {
  return pa_heap_alloc_new_n(pa_prim_get_default_heap(), count, size);
}


pa_decl_nodiscard pa_decl_restrict void* pa_new_nothrow(size_t size) pa_attr_noexcept {
  void* p = pa_malloc(size);
  if pa_unlikely(p == NULL) return pa_try_new(size, true);
  return p;
}

pa_decl_nodiscard pa_decl_restrict void* pa_new_aligned(size_t size, size_t alignment) {
  void* p;
  do {
    p = pa_malloc_aligned(size, alignment);
  }
  while(p == NULL && pa_try_new_handler(false));
  return p;
}

pa_decl_nodiscard pa_decl_restrict void* pa_new_aligned_nothrow(size_t size, size_t alignment) pa_attr_noexcept {
  void* p;
  do {
    p = pa_malloc_aligned(size, alignment);
  }
  while(p == NULL && pa_try_new_handler(true));
  return p;
}

pa_decl_nodiscard void* pa_new_realloc(void* p, size_t newsize) {
  void* q;
  do {
    q = pa_realloc(p, newsize);
  } while (q == NULL && pa_try_new_handler(false));
  return q;
}

pa_decl_nodiscard void* pa_new_reallocn(void* p, size_t newcount, size_t size) {
  size_t total;
  if pa_unlikely(pa_count_size_overflow(newcount, size, &total)) {
    pa_try_new_handler(false);  // on overflow we invoke the try_new_handler once to potentially throw std::bad_alloc
    return NULL;
  }
  else {
    return pa_new_realloc(p, total);
  }
}

#if PA_GUARDED
// We always allocate a guarded allocation at an offset (`pa_page_has_aligned` will be true).
// We then set the first word of the block to `0` for regular offset aligned allocations (in `alloc-aligned.c`)
// and the first word to `~0` for guarded allocations to have a correct `pa_usable_size`

static void* pa_block_ptr_set_guarded(pa_block_t* block, size_t obj_size) {
  // TODO: we can still make padding work by moving it out of the guard page area
  pa_page_t* const page = _pa_ptr_page(block);
  pa_page_set_has_aligned(page, true);
  block->next = PA_BLOCK_TAG_GUARDED;

  // set guard page at the end of the block
  pa_segment_t* const segment = _pa_page_segment(page);
  const size_t block_size = pa_page_block_size(page);  // must use `block_size` to match `pa_free_local`
  const size_t os_page_size = _pa_os_page_size();
  pa_assert_internal(block_size >= obj_size + os_page_size + sizeof(pa_block_t));
  if (block_size < obj_size + os_page_size + sizeof(pa_block_t)) {
    // should never happen
    pa_free(block);
    return NULL;
  }
  uint8_t* guard_page = (uint8_t*)block + block_size - os_page_size;
  pa_assert_internal(_pa_is_aligned(guard_page, os_page_size));
  if pa_likely(segment->allow_decommit && _pa_is_aligned(guard_page, os_page_size)) {
    const bool ok = _pa_os_protect(guard_page, os_page_size);
    if pa_unlikely(!ok) {
      _pa_warning_message("failed to set a guard page behind an object (object %p of size %zu)\n", block, block_size);
    }
  }
  else {
    _pa_warning_message("unable to set a guard page behind an object due to pinned memory (large OS pages?) (object %p of size %zu)\n", block, block_size);
  }

  // align pointer just in front of the guard page
  size_t offset = block_size - os_page_size - obj_size;
  pa_assert_internal(offset > sizeof(pa_block_t));
  if (offset > PA_BLOCK_ALIGNMENT_MAX) {
    // give up to place it right in front of the guard page if the offset is too large for unalignment
    offset = PA_BLOCK_ALIGNMENT_MAX;
  }
  void* p = (uint8_t*)block + offset;
  pa_track_align(block, p, offset, obj_size);
  pa_track_mem_defined(block, sizeof(pa_block_t));
  return p;
}

pa_decl_restrict void* _pa_heap_malloc_guarded(pa_heap_t* heap, size_t size, bool zero) pa_attr_noexcept
{
  #if defined(PA_PADDING_SIZE)
  pa_assert(PA_PADDING_SIZE==0);
  #endif
  // allocate multiple of page size ending in a guard page
  // ensure minimal alignment requirement?
  const size_t os_page_size = _pa_os_page_size();
  const size_t obj_size = (pa_option_is_enabled(pa_option_guarded_precise) ? size : _pa_align_up(size, PA_MAX_ALIGN_SIZE));
  const size_t bsize    = _pa_align_up(_pa_align_up(obj_size, PA_MAX_ALIGN_SIZE) + sizeof(pa_block_t), PA_MAX_ALIGN_SIZE);
  const size_t req_size = _pa_align_up(bsize + os_page_size, os_page_size);
  pa_block_t* const block = (pa_block_t*)_pa_malloc_generic(heap, req_size, zero, 0 /* huge_alignment */, NULL);
  if (block==NULL) return NULL;
  void* const p   = pa_block_ptr_set_guarded(block, obj_size);

  // stats
  pa_track_malloc(p, size, zero);
  if (p != NULL) {
    if (!pa_heap_is_initialized(heap)) { heap = pa_prim_get_default_heap(); }
    #if PA_STAT>1
    pa_heap_stat_adjust_decrease(heap, malloc_requested, req_size);
    pa_heap_stat_increase(heap, malloc_requested, size);
    #endif
    _pa_stat_counter_increase(&heap->tld->stats.malloc_guarded_count, 1);
  }
  #if PA_DEBUG>3
  if (p != NULL && zero) {
    pa_assert_expensive(pa_mem_is_zero(p, size));
  }
  #endif
  return p;
}
#endif

// ------------------------------------------------------
// ensure explicit external inline definitions are emitted!
// ------------------------------------------------------

#ifdef __cplusplus
void* _pa_externs[] = {
  (void*)&_pa_page_malloc,
  (void*)&_pa_page_malloc_zero,
  (void*)&_pa_heap_malloc_zero,
  (void*)&_pa_heap_malloc_zero_ex,
  (void*)&pa_malloc,
  (void*)&pa_malloc_small,
  (void*)&pa_zalloc_small,
  (void*)&pa_heap_malloc,
  (void*)&pa_heap_zalloc,
  (void*)&pa_heap_malloc_small,
  // (void*)&pa_heap_alloc_new,
  // (void*)&pa_heap_alloc_new_n
};
#endif
