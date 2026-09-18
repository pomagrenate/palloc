/* ----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "palloc.h"
#include "palloc/internal.h"
#include "palloc/atomic.h"

#include <string.h>  // memset
#include <stdio.h>

// -------------------------------------------------------------------
// Segments
// palloc pages reside in segments. See `pa_segment_valid` for invariants.
// -------------------------------------------------------------------


static void pa_segment_try_purge(pa_segment_t* segment, bool force);


// -------------------------------------------------------------------
// commit mask
// -------------------------------------------------------------------

static bool pa_commit_mask_all_set(const pa_commit_mask_t* commit, const pa_commit_mask_t* cm) {
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    if ((commit->mask[i] & cm->mask[i]) != cm->mask[i]) return false;
  }
  return true;
}

static bool pa_commit_mask_any_set(const pa_commit_mask_t* commit, const pa_commit_mask_t* cm) {
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    if ((commit->mask[i] & cm->mask[i]) != 0) return true;
  }
  return false;
}

static void pa_commit_mask_create_intersect(const pa_commit_mask_t* commit, const pa_commit_mask_t* cm, pa_commit_mask_t* res) {
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] = (commit->mask[i] & cm->mask[i]);
  }
}

static void pa_commit_mask_clear(pa_commit_mask_t* res, const pa_commit_mask_t* cm) {
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] &= ~(cm->mask[i]);
  }
}

static void pa_commit_mask_set(pa_commit_mask_t* res, const pa_commit_mask_t* cm) {
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] |= cm->mask[i];
  }
}

static void pa_commit_mask_create(size_t bitidx, size_t bitcount, pa_commit_mask_t* cm) {
  pa_assert_internal(bitidx < PA_COMMIT_MASK_BITS);
  pa_assert_internal((bitidx + bitcount) <= PA_COMMIT_MASK_BITS);
  if (bitcount == PA_COMMIT_MASK_BITS) {
    pa_assert_internal(bitidx==0);
    pa_commit_mask_create_full(cm);
  }
  else if (bitcount == 0) {
    pa_commit_mask_create_empty(cm);
  }
  else {
    pa_commit_mask_create_empty(cm);
    size_t i = bitidx / PA_COMMIT_MASK_FIELD_BITS;
    size_t ofs = bitidx % PA_COMMIT_MASK_FIELD_BITS;
    while (bitcount > 0) {
      pa_assert_internal(i < PA_COMMIT_MASK_FIELD_COUNT);
      size_t avail = PA_COMMIT_MASK_FIELD_BITS - ofs;
      size_t count = (bitcount > avail ? avail : bitcount);
      size_t mask = (count >= PA_COMMIT_MASK_FIELD_BITS ? ~((size_t)0) : (((size_t)1 << count) - 1) << ofs);
      cm->mask[i] = mask;
      bitcount -= count;
      ofs = 0;
      i++;
    }
  }
}

size_t _pa_commit_mask_committed_size(const pa_commit_mask_t* cm, size_t total) {
  pa_assert_internal((total%PA_COMMIT_MASK_BITS)==0);
  size_t count = 0;
  for (size_t i = 0; i < PA_COMMIT_MASK_FIELD_COUNT; i++) {
    size_t mask = cm->mask[i];
    if (~mask == 0) {
      count += PA_COMMIT_MASK_FIELD_BITS;
    }
    else {
      for (; mask != 0; mask >>= 1) {  // todo: use popcount
        if ((mask&1)!=0) count++;
      }
    }
  }
  // we use total since for huge segments each commit bit may represent a larger size
  return ((total / PA_COMMIT_MASK_BITS) * count);
}


size_t _pa_commit_mask_next_run(const pa_commit_mask_t* cm, size_t* idx) {
  size_t i = (*idx) / PA_COMMIT_MASK_FIELD_BITS;
  size_t ofs = (*idx) % PA_COMMIT_MASK_FIELD_BITS;
  size_t mask = 0;
  // find first ones
  while (i < PA_COMMIT_MASK_FIELD_COUNT) {
    mask = cm->mask[i];
    mask >>= ofs;
    if (mask != 0) {
      while ((mask&1) == 0) {
        mask >>= 1;
        ofs++;
      }
      break;
    }
    i++;
    ofs = 0;
  }
  if (i >= PA_COMMIT_MASK_FIELD_COUNT) {
    // not found
    *idx = PA_COMMIT_MASK_BITS;
    return 0;
  }
  else {
    // found, count ones
    size_t count = 0;
    *idx = (i*PA_COMMIT_MASK_FIELD_BITS) + ofs;
    do {
      pa_assert_internal(ofs < PA_COMMIT_MASK_FIELD_BITS && (mask&1) == 1);
      do {
        count++;
        mask >>= 1;
      } while ((mask&1) == 1);
      if ((((*idx + count) % PA_COMMIT_MASK_FIELD_BITS) == 0)) {
        i++;
        if (i >= PA_COMMIT_MASK_FIELD_COUNT) break;
        mask = cm->mask[i];
        ofs = 0;
      }
    } while ((mask&1) == 1);
    pa_assert_internal(count > 0);
    return count;
  }
}


/* --------------------------------------------------------------------------------
  Segment allocation
  We allocate pages inside bigger "segments" (32 MiB on 64-bit). This is to avoid
  splitting VMA's on Linux and reduce fragmentation on other OS's.
  Each thread owns its own segments.

  Currently we have:
  - small pages (64KiB)
  - medium pages (512KiB)
  - large pages (4MiB),
  - huge segments have 1 page in one segment that can be larger than `PA_SEGMENT_SIZE`.
    it is used for blocks `> PA_LARGE_OBJ_SIZE_MAX` or with alignment `> PA_BLOCK_ALIGNMENT_MAX`.

  The memory for a segment is usually committed on demand.
  (i.e. we are careful to not touch the memory until we actually allocate a block there)

  If a  thread ends, it "abandons" pages that still contain live blocks.
  Such segments are abandoned and these can be reclaimed by still running threads,
  (much like work-stealing).
-------------------------------------------------------------------------------- */


/* -----------------------------------------------------------
   Slices
----------------------------------------------------------- */


static const pa_slice_t* pa_segment_slices_end(const pa_segment_t* segment) {
  return &segment->slices[segment->slice_entries];
}

static uint8_t* pa_slice_start(const pa_slice_t* slice) {
  pa_segment_t* segment = _pa_ptr_segment(slice);
  pa_assert_internal(slice >= segment->slices && slice < pa_segment_slices_end(segment));
  return ((uint8_t*)segment + ((slice - segment->slices)*PA_SEGMENT_SLICE_SIZE));
}


/* -----------------------------------------------------------
   Bins
----------------------------------------------------------- */
// Use bit scan forward to quickly find the first zero bit if it is available

static inline size_t pa_slice_bin8(size_t slice_count) {
  if (slice_count<=1) return slice_count;
  pa_assert_internal(slice_count <= PA_SLICES_PER_SEGMENT);
  slice_count--;
  size_t s = pa_bsr(slice_count);  // slice_count > 1
  if (s <= 2) return slice_count + 1;
  size_t bin = ((s << 2) | ((slice_count >> (s - 2))&0x03)) - 4;
  return bin;
}

static inline size_t pa_slice_bin(size_t slice_count) {
  pa_assert_internal(slice_count*PA_SEGMENT_SLICE_SIZE <= PA_SEGMENT_SIZE);
  pa_assert_internal(pa_slice_bin8(PA_SLICES_PER_SEGMENT) <= PA_SEGMENT_BIN_MAX);
  size_t bin = pa_slice_bin8(slice_count);
  pa_assert_internal(bin <= PA_SEGMENT_BIN_MAX);
  return bin;
}

static inline size_t pa_slice_index(const pa_slice_t* slice) {
  pa_segment_t* segment = _pa_ptr_segment(slice);
  ptrdiff_t index = slice - segment->slices;
  pa_assert_internal(index >= 0 && index < (ptrdiff_t)segment->slice_entries);
  return index;
}


/* -----------------------------------------------------------
   Slice span queues
----------------------------------------------------------- */

static void pa_span_queue_push(pa_span_queue_t* sq, pa_slice_t* slice) {
  // todo: or push to the end?
  pa_assert_internal(slice->prev == NULL && slice->next==NULL);
  slice->prev = NULL; // paranoia
  slice->next = sq->first;
  sq->first = slice;
  if (slice->next != NULL) slice->next->prev = slice;
                     else sq->last = slice;
  slice->block_size = 0; // free
}

static pa_span_queue_t* pa_span_queue_for(size_t slice_count, pa_segments_tld_t* tld) {
  size_t bin = pa_slice_bin(slice_count);
  pa_span_queue_t* sq = &tld->spans[bin];
  pa_assert_internal(sq->slice_count >= slice_count);
  return sq;
}

static void pa_span_queue_delete(pa_span_queue_t* sq, pa_slice_t* slice) {
  pa_assert_internal(slice->block_size==0 && slice->slice_count>0 && slice->slice_offset==0);
  // should work too if the queue does not contain slice (which can happen during reclaim)
  if (slice->prev != NULL) slice->prev->next = slice->next;
  if (slice == sq->first) sq->first = slice->next;
  if (slice->next != NULL) slice->next->prev = slice->prev;
  if (slice == sq->last) sq->last = slice->prev;
  slice->prev = NULL;
  slice->next = NULL;
  slice->block_size = 1; // no more free
}


/* -----------------------------------------------------------
 Invariant checking
----------------------------------------------------------- */

static bool pa_slice_is_used(const pa_slice_t* slice) {
  return (slice->block_size > 0);
}


#if (PA_DEBUG>=3)
static bool pa_span_queue_contains(pa_span_queue_t* sq, pa_slice_t* slice) {
  for (pa_slice_t* s = sq->first; s != NULL; s = s->next) {
    if (s==slice) return true;
  }
  return false;
}

static bool pa_segment_is_valid(pa_segment_t* segment, pa_segments_tld_t* tld) {
  pa_assert_internal(segment != NULL);
  pa_assert_internal(_pa_ptr_cookie(segment) == segment->cookie);
  pa_assert_internal(segment->abandoned <= segment->used);
  pa_assert_internal(segment->thread_id == 0 || segment->thread_id == _pa_thread_id());
  pa_assert_internal(pa_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask)); // can only decommit committed blocks
  //pa_assert_internal(segment->segment_info_size % PA_SEGMENT_SLICE_SIZE == 0);
  pa_slice_t* slice = &segment->slices[0];
  const pa_slice_t* end = pa_segment_slices_end(segment);
  size_t used_count = 0;
  pa_span_queue_t* sq;
  while(slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    size_t index = pa_slice_index(slice);
    size_t maxindex = (index + slice->slice_count >= segment->slice_entries ? segment->slice_entries : index + slice->slice_count) - 1;
    if (pa_slice_is_used(slice)) { // a page in use, we need at least MAX_SLICE_OFFSET_COUNT valid back offsets
      used_count++;
      pa_assert_internal(slice->is_huge == (segment->kind == PA_SEGMENT_HUGE));
      for (size_t i = 0; i <= PA_MAX_SLICE_OFFSET_COUNT && index + i <= maxindex; i++) {
        pa_assert_internal(segment->slices[index + i].slice_offset == i*sizeof(pa_slice_t));
        pa_assert_internal(i==0 || segment->slices[index + i].slice_count == 0);
        pa_assert_internal(i==0 || segment->slices[index + i].block_size == 1);
      }
      // and the last entry as well (for coalescing)
      const pa_slice_t* last = slice + slice->slice_count - 1;
      if (last > slice && last < pa_segment_slices_end(segment)) {
        pa_assert_internal(last->slice_offset == (slice->slice_count-1)*sizeof(pa_slice_t));
        pa_assert_internal(last->slice_count == 0);
        pa_assert_internal(last->block_size == 1);
      }
    }
    else {  // free range of slices; only last slice needs a valid back offset
      pa_slice_t* last = &segment->slices[maxindex];
      if (segment->kind != PA_SEGMENT_HUGE || slice->slice_count <= (segment->slice_entries - segment->segment_info_slices)) {
        pa_assert_internal((uint8_t*)slice == (uint8_t*)last - last->slice_offset);
      }
      pa_assert_internal(slice == last || last->slice_count == 0 );
      pa_assert_internal(last->block_size == 0 || (segment->kind==PA_SEGMENT_HUGE && last->block_size==1));
      if (segment->kind != PA_SEGMENT_HUGE && segment->thread_id != 0) { // segment is not huge or abandoned
        sq = pa_span_queue_for(slice->slice_count,tld);
        pa_assert_internal(pa_span_queue_contains(sq,slice));
      }
    }
    slice = &segment->slices[maxindex+1];
  }
  pa_assert_internal(slice == end);
  pa_assert_internal(used_count == segment->used + 1);
  return true;
}
#endif

/* -----------------------------------------------------------
 Segment size calculations
----------------------------------------------------------- */

static size_t pa_segment_info_size(pa_segment_t* segment) {
  return segment->segment_info_slices * PA_SEGMENT_SLICE_SIZE;
}

static uint8_t* _pa_segment_page_start_from_slice(const pa_segment_t* segment, const pa_slice_t* slice, size_t block_size, size_t* page_size)
{
  const ptrdiff_t idx = slice - segment->slices;
  const size_t psize = (size_t)slice->slice_count * PA_SEGMENT_SLICE_SIZE;
  uint8_t* const pstart = (uint8_t*)segment + (idx*PA_SEGMENT_SLICE_SIZE);
  // make the start not OS page aligned for smaller blocks to avoid page/cache effects
  // note: the offset must always be a block_size multiple since we assume small allocations
  // are aligned (see `pa_heap_malloc_aligned`).
  size_t start_offset = 0;
  if (block_size > 0 && block_size <= PA_MAX_ALIGN_GUARANTEE) {
    // for small objects, ensure the page start is aligned with the block size (PR#66 by kickunderscore)
    const size_t adjust = block_size - ((uintptr_t)pstart % block_size);
    if (adjust < block_size && psize >= block_size + adjust) {
      start_offset += adjust;
    }
  }
  if (block_size >= PA_INTPTR_SIZE) {
    if (block_size <= 64) { start_offset += 3*block_size; }
    else if (block_size <= 512) { start_offset += block_size; }
  }
  start_offset = _pa_align_up(start_offset, PA_MAX_ALIGN_SIZE);
  pa_assert_internal(_pa_is_aligned(pstart + start_offset, PA_MAX_ALIGN_SIZE));
  pa_assert_internal(block_size == 0 || block_size > PA_MAX_ALIGN_GUARANTEE || _pa_is_aligned(pstart + start_offset,block_size));
  if (page_size != NULL) { *page_size = psize - start_offset; }
  return (pstart + start_offset);
}

// Start of the page available memory; can be used on uninitialized pages
uint8_t* _pa_segment_page_start(const pa_segment_t* segment, const pa_page_t* page, size_t* page_size)
{
  const pa_slice_t* slice = pa_page_to_slice((pa_page_t*)page);
  uint8_t* p = _pa_segment_page_start_from_slice(segment, slice, pa_page_block_size(page), page_size);
  pa_assert_internal(pa_page_block_size(page) > 0 || _pa_ptr_page(p) == page);
  pa_assert_internal(_pa_ptr_segment(p) == segment);
  return p;
}


static size_t pa_segment_calculate_slices(size_t required, size_t* info_slices) {
  size_t page_size = _pa_os_page_size();
  size_t isize     = _pa_align_up(sizeof(pa_segment_t), page_size);
  size_t guardsize = 0;

  if (PA_SECURE>0) {
    // in secure mode, we set up a protected page in between the segment info
    // and the page data (and one at the end of the segment)
    guardsize = page_size;
    if (required > 0) {
      required = _pa_align_up(required, PA_SEGMENT_SLICE_SIZE) + page_size;
    }
  }

  isize = _pa_align_up(isize + guardsize, PA_SEGMENT_SLICE_SIZE);
  if (info_slices != NULL) *info_slices = isize / PA_SEGMENT_SLICE_SIZE;
  size_t segment_size = (required==0 ? PA_SEGMENT_SIZE : _pa_align_up( required + isize + guardsize, PA_SEGMENT_SLICE_SIZE) );
  pa_assert_internal(segment_size % PA_SEGMENT_SLICE_SIZE == 0);
  return (segment_size / PA_SEGMENT_SLICE_SIZE);
}


/* ----------------------------------------------------------------------------
Segment caches
We keep a small segment cache per thread to increase local
reuse and avoid setting/clearing guard pages in secure mode.
------------------------------------------------------------------------------- */

static void pa_segments_track_size(long segment_size, pa_segments_tld_t* tld) {
  if (segment_size>=0) _pa_stat_increase(&tld->stats->segments,1);
                  else _pa_stat_decrease(&tld->stats->segments,1);
  tld->count += (segment_size >= 0 ? 1 : -1);
  if (tld->count > tld->peak_count) tld->peak_count = tld->count;
  tld->current_size += segment_size;
  if (tld->current_size > tld->peak_size) tld->peak_size = tld->current_size;
}

static void pa_segment_os_free(pa_segment_t* segment, pa_segments_tld_t* tld) {
  segment->thread_id = 0;
  _pa_segment_map_freed_at(segment);
  pa_segments_track_size(-((long)pa_segment_size(segment)),tld);
  if (segment->was_reclaimed) {
    tld->reclaim_count--;
    segment->was_reclaimed = false;
  }
  if (PA_SECURE>0) {
    // _pa_os_unprotect(segment, pa_segment_size(segment)); // ensure no more guard pages are set
    // unprotect the guard pages; we cannot just unprotect the whole segment size as part may be decommitted
    size_t os_pagesize = _pa_os_page_size();
    _pa_os_unprotect((uint8_t*)segment + pa_segment_info_size(segment) - os_pagesize, os_pagesize);
    uint8_t* end = (uint8_t*)segment + pa_segment_size(segment) - os_pagesize;
    _pa_os_unprotect(end, os_pagesize);
  }

  // purge delayed decommits now? (no, leave it to the arena)
  // pa_segment_try_purge(segment,true,tld->stats);

  const size_t size = pa_segment_size(segment);
  const size_t csize = _pa_commit_mask_committed_size(&segment->commit_mask, size);

  _pa_arena_free(segment, pa_segment_size(segment), csize, segment->memid);
}

/* -----------------------------------------------------------
   Commit/Decommit ranges
----------------------------------------------------------- */

static void pa_segment_commit_mask(pa_segment_t* segment, bool conservative, uint8_t* p, size_t size, uint8_t** start_p, size_t* full_size, pa_commit_mask_t* cm) {
  pa_assert_internal(_pa_ptr_segment(p + 1) == segment);
  pa_assert_internal(segment->kind != PA_SEGMENT_HUGE);
  pa_commit_mask_create_empty(cm);
  if (size == 0 || size > PA_SEGMENT_SIZE || segment->kind == PA_SEGMENT_HUGE) return;
  const size_t segstart = pa_segment_info_size(segment);
  const size_t segsize = pa_segment_size(segment);
  if (p >= (uint8_t*)segment + segsize) return;

  size_t pstart = (p - (uint8_t*)segment);
  pa_assert_internal(pstart + size <= segsize);

  size_t start;
  size_t end;
  if (conservative) {
    // decommit conservative
    start = _pa_align_up(pstart, PA_COMMIT_SIZE);
    end   = _pa_align_down(pstart + size, PA_COMMIT_SIZE);
    pa_assert_internal(start >= segstart);
    pa_assert_internal(end <= segsize);
  }
  else {
    // commit liberal
    start = _pa_align_down(pstart, PA_MINIMAL_COMMIT_SIZE);
    end   = _pa_align_up(pstart + size, PA_MINIMAL_COMMIT_SIZE);
  }
  if (pstart >= segstart && start < segstart) {  // note: the mask is also calculated for an initial commit of the info area
    start = segstart;
  }
  if (end > segsize) {
    end = segsize;
  }

  pa_assert_internal(start <= pstart && (pstart + size) <= end);
  pa_assert_internal(start % PA_COMMIT_SIZE==0 && end % PA_COMMIT_SIZE == 0);
  *start_p   = (uint8_t*)segment + start;
  *full_size = (end > start ? end - start : 0);
  if (*full_size == 0) return;

  size_t bitidx = start / PA_COMMIT_SIZE;
  pa_assert_internal(bitidx < PA_COMMIT_MASK_BITS);

  size_t bitcount = *full_size / PA_COMMIT_SIZE; // can be 0
  if (bitidx + bitcount > PA_COMMIT_MASK_BITS) {
    _pa_warning_message("commit mask overflow: idx=%zu count=%zu start=%zx end=%zx p=0x%p size=%zu fullsize=%zu\n", bitidx, bitcount, start, end, p, size, *full_size);
  }
  pa_assert_internal((bitidx + bitcount) <= PA_COMMIT_MASK_BITS);
  pa_commit_mask_create(bitidx, bitcount, cm);
}

// When we commit new OS memory, the OS may report it as zero-initialized (e.g. Linux).
// Optional out_committed_is_zero: set to true iff we actually committed and OS reported zero.
static bool pa_segment_commit(pa_segment_t* segment, uint8_t* p, size_t size, bool* out_committed_is_zero) {
  pa_assert_internal(pa_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));
  if (out_committed_is_zero != NULL) *out_committed_is_zero = false;

  // commit liberal
  uint8_t* start = NULL;
  size_t   full_size = 0;
  pa_commit_mask_t mask;
  pa_segment_commit_mask(segment, false /* conservative? */, p, size, &start, &full_size, &mask);
  if (pa_commit_mask_is_empty(&mask) || full_size == 0) return true;

  if (!pa_commit_mask_all_set(&segment->commit_mask, &mask)) {
    // committing
    bool is_zero = false;
    pa_commit_mask_t cmask;
    pa_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);
    _pa_stat_decrease(&_pa_stats_main.committed, _pa_commit_mask_committed_size(&cmask, PA_SEGMENT_SIZE)); // adjust for overlap
    if (!_pa_os_commit(start, full_size, &is_zero)) return false;
    pa_commit_mask_set(&segment->commit_mask, &mask);
    if (out_committed_is_zero != NULL && is_zero) *out_committed_is_zero = true;
  }

  // increase purge expiration when using part of delayed purges -- we assume more allocations are coming soon.
  if (pa_commit_mask_any_set(&segment->purge_mask, &mask)) {
    if (pa_option_is_enabled(pa_option_adaptive_purge)) {
      segment->purge_delay_ms = (segment->purge_delay_ms == 0 ? 1 : segment->purge_delay_ms * 2);
      if (segment->purge_delay_ms > 1000) segment->purge_delay_ms = 1000;
    }
    segment->purge_expire = _pa_clock_now() + (segment->purge_delay_ms > 0 ? segment->purge_delay_ms : pa_option_get(pa_option_purge_delay));
  }

  // always clear any delayed purges in our range (as they are either committed now)
  pa_commit_mask_clear(&segment->purge_mask, &mask);
  return true;
}

static bool pa_segment_ensure_committed(pa_segment_t* segment, uint8_t* p, size_t size, bool* out_committed_is_zero) {
  pa_assert_internal(pa_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));
  // note: assumes commit_mask is always full for huge segments as otherwise the commit mask bits can overflow
  if (pa_commit_mask_is_full(&segment->commit_mask) && pa_commit_mask_is_empty(&segment->purge_mask)) return true; // fully committed
  pa_assert_internal(segment->kind != PA_SEGMENT_HUGE);
  return pa_segment_commit(segment, p, size, out_committed_is_zero);
}

static bool pa_segment_purge(pa_segment_t* segment, uint8_t* p, size_t size) {
  pa_assert_internal(pa_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));
  if (!segment->allow_purge) return true;

  // purge conservative
  uint8_t* start = NULL;
  size_t   full_size = 0;
  pa_commit_mask_t mask;
  pa_segment_commit_mask(segment, true /* conservative? */, p, size, &start, &full_size, &mask);
  if (pa_commit_mask_is_empty(&mask) || full_size==0) return true;

  if (pa_commit_mask_any_set(&segment->commit_mask, &mask)) {
    // purging
    pa_assert_internal((void*)start != (void*)segment);
    pa_assert_internal(segment->allow_decommit);
    const bool decommitted = _pa_os_purge(start, full_size);  // reset or decommit
    if (decommitted) {
      pa_commit_mask_t cmask;
      pa_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);
      _pa_stat_increase(&_pa_stats_main.committed, full_size - _pa_commit_mask_committed_size(&cmask, PA_SEGMENT_SIZE)); // adjust for double counting
      pa_commit_mask_clear(&segment->commit_mask, &mask);
    }
  }

  // always clear any scheduled purges in our range
  pa_commit_mask_clear(&segment->purge_mask, &mask);
  return true;
}

static void pa_segment_schedule_purge(pa_segment_t* segment, uint8_t* p, size_t size) {
  if (!segment->allow_purge) return;

  if (pa_option_get(pa_option_purge_delay) == 0) {
    pa_segment_purge(segment, p, size);
  }
  else {
    // register for future purge in the purge mask
    uint8_t* start = NULL;
    size_t   full_size = 0;
    pa_commit_mask_t mask;
    pa_segment_commit_mask(segment, true /*conservative*/, p, size, &start, &full_size, &mask);
    if (pa_commit_mask_is_empty(&mask) || full_size==0) return;

    // update delayed commit
    pa_assert_internal(segment->purge_expire > 0 || pa_commit_mask_is_empty(&segment->purge_mask));
    pa_commit_mask_t cmask;
    pa_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);  // only purge what is committed; span_free may try to decommit more
    pa_commit_mask_set(&segment->purge_mask, &cmask);
    pa_msecs_t now = _pa_clock_now();
    if (segment->purge_expire == 0) {
      // no previous purgess, initialize now
      segment->purge_expire = now + (segment->purge_delay_ms > 0 ? segment->purge_delay_ms : pa_option_get(pa_option_purge_delay));
    }
    else if (segment->purge_expire <= now) {
      // previous purge mask already expired
      if (segment->purge_expire + pa_option_get(pa_option_purge_extend_delay) <= now) {
        pa_segment_try_purge(segment, true);
      }
      else {
        segment->purge_expire = now + pa_option_get(pa_option_purge_extend_delay); // (pa_option_get(pa_option_purge_delay) / 8); // wait a tiny bit longer in case there is a series of free's
      }
    }
    else {
      // previous purge mask is not yet expired, increase the expiration by a bit.
      segment->purge_expire += pa_option_get(pa_option_purge_extend_delay);
    }
  }
}

static void pa_segment_try_purge(pa_segment_t* segment, bool force) {
  if (!segment->allow_purge || segment->purge_expire == 0 || pa_commit_mask_is_empty(&segment->purge_mask)) return;
  pa_msecs_t now = _pa_clock_now();
  if (!force && now < segment->purge_expire) return;

  pa_commit_mask_t mask = segment->purge_mask;
  segment->purge_expire = 0;
  pa_commit_mask_create_empty(&segment->purge_mask);

  size_t idx;
  size_t count;
  pa_commit_mask_foreach(&mask, idx, count) {
    // if found, decommit that sequence
    if (count > 0) {
      uint8_t* p = (uint8_t*)segment + (idx*PA_COMMIT_SIZE);
      size_t size = count * PA_COMMIT_SIZE;
      pa_segment_purge(segment, p, size);
    }
  }
  pa_commit_mask_foreach_end()
  pa_assert_internal(pa_commit_mask_is_empty(&segment->purge_mask));

  if (pa_option_is_enabled(pa_option_adaptive_purge) && !force) {
    long delay = (long)pa_option_get(pa_option_purge_delay);
    segment->purge_delay_ms -= (segment->purge_delay_ms - delay) / 4;
    if (segment->purge_delay_ms < (pa_msecs_t)delay) segment->purge_delay_ms = (pa_msecs_t)delay;
  }
}

// called from `pa_heap_collect_ex`
// this can be called per-page so it is important that try_purge has fast exit path
void _pa_segment_collect(pa_segment_t* segment, bool force) {
  pa_segment_try_purge(segment, force);
}

/* -----------------------------------------------------------
   Span free
----------------------------------------------------------- */

static bool pa_segment_is_abandoned(pa_segment_t* segment) {
  return (pa_atomic_load_relaxed(&segment->thread_id) == 0);
}

// note: can be called on abandoned segments
static void pa_segment_span_free(pa_segment_t* segment, size_t slice_index, size_t slice_count, bool allow_purge, pa_segments_tld_t* tld) {
  pa_assert_internal(slice_index < segment->slice_entries);
  pa_span_queue_t* sq = (segment->kind == PA_SEGMENT_HUGE || pa_segment_is_abandoned(segment)
                          ? NULL : pa_span_queue_for(slice_count,tld));
  if (slice_count==0) slice_count = 1;
  pa_assert_internal(slice_index + slice_count - 1 < segment->slice_entries);

  // set first and last slice (the intermediates can be undetermined)
  pa_slice_t* slice = &segment->slices[slice_index];
  slice->slice_count = (uint32_t)slice_count;
  pa_assert_internal(slice->slice_count == slice_count); // no overflow?
  slice->slice_offset = 0;
  if (slice_count > 1) {
    pa_slice_t* last = slice + slice_count - 1;
    pa_slice_t* end  = (pa_slice_t*)pa_segment_slices_end(segment);
    if (last > end) { last = end; }
    last->slice_count = 0;
    last->slice_offset = (uint32_t)(sizeof(pa_page_t)*(slice_count - 1));
    last->block_size = 0;
  }

  // perhaps decommit
  if (allow_purge) {
    pa_segment_schedule_purge(segment, pa_slice_start(slice), slice_count * PA_SEGMENT_SLICE_SIZE);
  }

  // and push it on the free page queue (if it was not a huge page)
  if (sq != NULL) pa_span_queue_push( sq, slice );
             else slice->block_size = 0; // mark huge page as free anyways
}

/*
// called from reclaim to add existing free spans
static void pa_segment_span_add_free(pa_slice_t* slice, pa_segments_tld_t* tld) {
  pa_segment_t* segment = _pa_ptr_segment(slice);
  pa_assert_internal(slice->xblock_size==0 && slice->slice_count>0 && slice->slice_offset==0);
  size_t slice_index = pa_slice_index(slice);
  pa_segment_span_free(segment,slice_index,slice->slice_count,tld);
}
*/

static void pa_segment_span_remove_from_queue(pa_slice_t* slice, pa_segments_tld_t* tld) {
  pa_assert_internal(slice->slice_count > 0 && slice->slice_offset==0 && slice->block_size==0);
  pa_assert_internal(_pa_ptr_segment(slice)->kind != PA_SEGMENT_HUGE);
  pa_span_queue_t* sq = pa_span_queue_for(slice->slice_count, tld);
  pa_span_queue_delete(sq, slice);
}

// note: can be called on abandoned segments
static pa_slice_t* pa_segment_span_free_coalesce(pa_slice_t* slice, pa_segments_tld_t* tld) {
  pa_assert_internal(slice != NULL && slice->slice_count > 0 && slice->slice_offset == 0);
  pa_segment_t* const segment = _pa_ptr_segment(slice);

  // for huge pages, just mark as free but don't add to the queues
  if (segment->kind == PA_SEGMENT_HUGE) {
    // issue #691: segment->used can be 0 if the huge page block was freed while abandoned (reclaim will get here in that case)
    pa_assert_internal((segment->used==0 && slice->block_size==0) || segment->used == 1);  // decreased right after this call in `pa_segment_page_clear`
    slice->block_size = 0;  // mark as free anyways
    // we should mark the last slice `xblock_size=0` now to maintain invariants but we skip it to
    // avoid a possible cache miss (and the segment is about to be freed)
    return slice;
  }

  // otherwise coalesce the span and add to the free span queues
  const bool is_abandoned = (segment->thread_id == 0); // pa_segment_is_abandoned(segment);
  size_t slice_count = slice->slice_count;
  pa_slice_t* next = slice + slice->slice_count;
  pa_assert_internal(next <= pa_segment_slices_end(segment));
  if (next < pa_segment_slices_end(segment) && next->block_size==0) {
    // free next block -- remove it from free and merge
    pa_assert_internal(next->slice_count > 0 && next->slice_offset==0);
    slice_count += next->slice_count; // extend
    if (!is_abandoned) { pa_segment_span_remove_from_queue(next, tld); }
  }
  if (slice > segment->slices) {
    pa_slice_t* prev = pa_slice_first(slice - 1);
    pa_assert_internal(prev >= segment->slices);
    if (prev->block_size==0) {
      // free previous slice -- remove it from free and merge
      pa_assert_internal(prev->slice_count > 0 && prev->slice_offset==0);
      slice_count += prev->slice_count;
      slice->slice_count = 0;
      slice->slice_offset = (uint32_t)((uint8_t*)slice - (uint8_t*)prev); // set the slice offset for `segment_force_abandon` (in case the previous free block is very large).
      if (!is_abandoned) { pa_segment_span_remove_from_queue(prev, tld); }
      slice = prev;
    }
  }

  // and add the new free page
  pa_segment_span_free(segment, pa_slice_index(slice), slice_count, true, tld);
  return slice;
}



/* -----------------------------------------------------------
   Page allocation
----------------------------------------------------------- */

// Note: may still return NULL if committing the memory failed
static pa_page_t* pa_segment_span_allocate(pa_segment_t* segment, size_t slice_index, size_t slice_count) {
  pa_assert_internal(slice_index < segment->slice_entries);
  pa_slice_t* const slice = &segment->slices[slice_index];
  pa_assert_internal(slice->block_size==0 || slice->block_size==1);

  // commit before changing the slice data; learn if newly committed memory is zero (calloc can skip memset)
  bool committed_is_zero = false;
  if (!pa_segment_ensure_committed(segment, _pa_segment_page_start_from_slice(segment, slice, 0, NULL), slice_count * PA_SEGMENT_SLICE_SIZE, &committed_is_zero)) {
    return NULL;  // commit failed!
  }

  // convert the slices to a page
  slice->slice_offset = 0;
  slice->slice_count = (uint32_t)slice_count;
  pa_assert_internal(slice->slice_count == slice_count);
  const size_t bsize = slice_count * PA_SEGMENT_SLICE_SIZE;
  slice->block_size = bsize;
  pa_page_t*  page = pa_slice_to_page(slice);
  pa_assert_internal(pa_page_block_size(page) == bsize);

  // set slice back pointers for the first PA_MAX_SLICE_OFFSET_COUNT entries
  size_t extra = slice_count-1;
  if (extra > PA_MAX_SLICE_OFFSET_COUNT) extra = PA_MAX_SLICE_OFFSET_COUNT;
  if (slice_index + extra >= segment->slice_entries) extra = segment->slice_entries - slice_index - 1;  // huge objects may have more slices than avaiable entries in the segment->slices

  pa_slice_t* slice_next = slice + 1;
  for (size_t i = 1; i <= extra; i++, slice_next++) {
    slice_next->slice_offset = (uint32_t)(sizeof(pa_slice_t)*i);
    slice_next->slice_count = 0;
    slice_next->block_size = 1;
  }

  // and also for the last one (if not set already) (the last one is needed for coalescing and for large alignments)
  // note: the cast is needed for ubsan since the index can be larger than PA_SLICES_PER_SEGMENT for huge allocations (see #543)
  pa_slice_t* last = slice + slice_count - 1;
  pa_slice_t* end = (pa_slice_t*)pa_segment_slices_end(segment);
  if (last > end) last = end;
  if (last > slice) {
    last->slice_offset = (uint32_t)(sizeof(pa_slice_t) * (last - slice));
    last->slice_count = 0;
    last->block_size = 1;
  }

  // and initialize the page (freshly committed OS memory is zero; propagate so calloc can skip memset)
  page->is_committed = true;
  page->is_zero_init = segment->free_is_zero || committed_is_zero;
  page->is_huge = (segment->kind == PA_SEGMENT_HUGE);
  segment->used++;
  return page;
}

static void pa_segment_slice_split(pa_segment_t* segment, pa_slice_t* slice, size_t slice_count, pa_segments_tld_t* tld) {
  pa_assert_internal(_pa_ptr_segment(slice) == segment);
  pa_assert_internal(slice->slice_count >= slice_count);
  pa_assert_internal(slice->block_size > 0); // no more in free queue
  if (slice->slice_count <= slice_count) return;
  pa_assert_internal(segment->kind != PA_SEGMENT_HUGE);
  size_t next_index = pa_slice_index(slice) + slice_count;
  size_t next_count = slice->slice_count - slice_count;
  pa_segment_span_free(segment, next_index, next_count, false /* don't purge left-over part */, tld);
  slice->slice_count = (uint32_t)slice_count;
}

// Try to extend a large page in place by taking the next contiguous free span.
// Returns true if extended; page->slice_count and page->block_size are updated.
// Caller must zero the new tail if required (e.g. for realloc zero).
bool _pa_segment_try_extend_span(pa_segment_t* segment, pa_page_t* page, size_t need_slices, pa_segments_tld_t* tld) {
  pa_assert_internal(segment != NULL && page != NULL && need_slices > 0);
  pa_assert_internal(segment->kind != PA_SEGMENT_HUGE);
  pa_slice_t* const slice = pa_page_to_slice(page);
  pa_assert_internal(_pa_ptr_segment(slice) == segment);
  const size_t slice_index = pa_slice_index(slice);
  const size_t current_count = page->slice_count;
  pa_assert_internal(current_count > 0);

  if (slice_index + current_count + need_slices > segment->slice_entries)
    return false;

  pa_slice_t* const next_slice = slice + current_count;
  if (next_slice->block_size != 0)
    return false;
  if (next_slice->slice_count == 0 || next_slice->slice_count < need_slices)
    return false;

  pa_segment_span_remove_from_queue(next_slice, tld);

  const size_t take_count = need_slices;
  const size_t tail_count = next_slice->slice_count - take_count;
  if (tail_count > 0)
    pa_segment_span_free(segment, slice_index + current_count + take_count, tail_count, false, tld);

  uint8_t* const new_start = (uint8_t*)segment + (slice_index + current_count) * PA_SEGMENT_SLICE_SIZE;
  if (!pa_segment_ensure_committed(segment, new_start, take_count * PA_SEGMENT_SLICE_SIZE, NULL))
    return false;

  for (size_t i = 0; i < take_count; i++) {
    pa_slice_t* s = slice + current_count + i;
    s->slice_offset = (uint32_t)((current_count + i) * sizeof(pa_slice_t));
    s->slice_count = 0;
    s->block_size = 1;
  }
  pa_slice_t* const new_last = slice + current_count + take_count - 1;
  pa_slice_t* const end = (pa_slice_t*)pa_segment_slices_end(segment);
  if (new_last <= end)
    new_last->slice_offset = (uint32_t)((current_count + take_count - 1) * sizeof(pa_slice_t));

  page->slice_count = (uint32_t)(current_count + take_count);
  page->block_size = (current_count + take_count) * PA_SEGMENT_SLICE_SIZE;
  return true;
}

static pa_page_t* pa_segments_page_find_and_allocate(size_t slice_count, pa_arena_id_t req_arena_id, pa_segments_tld_t* tld) {
  pa_assert_internal(slice_count*PA_SEGMENT_SLICE_SIZE <= PA_LARGE_OBJ_SIZE_MAX);
  // search from best fit up
  pa_span_queue_t* sq = pa_span_queue_for(slice_count, tld);
  if (slice_count == 0) slice_count = 1;
  while (sq <= &tld->spans[PA_SEGMENT_BIN_MAX]) {
    for (pa_slice_t* slice = sq->first; slice != NULL; slice = slice->next) {
      if (slice->slice_count >= slice_count) {
        // found one
        pa_segment_t* segment = _pa_ptr_segment(slice);
        if (_pa_arena_memid_is_suitable(segment->memid, req_arena_id)) {
          // found a suitable page span
          pa_span_queue_delete(sq, slice);

          if (slice->slice_count > slice_count) {
            pa_segment_slice_split(segment, slice, slice_count, tld);
          }
          pa_assert_internal(slice != NULL && slice->slice_count == slice_count && slice->block_size > 0);
          pa_page_t* page = pa_segment_span_allocate(segment, pa_slice_index(slice), slice->slice_count);
          if (page == NULL) {
            // commit failed; return NULL but first restore the slice
            pa_segment_span_free_coalesce(slice, tld);
            return NULL;
          }
          return page;
        }
      }
    }
    sq++;
  }
  // could not find a page..
  return NULL;
}


/* -----------------------------------------------------------
   Segment allocation
----------------------------------------------------------- */

static pa_segment_t* pa_segment_os_alloc( size_t required, size_t page_alignment, bool eager_delayed, pa_arena_id_t req_arena_id,
                                          size_t* psegment_slices, size_t* pinfo_slices,
                                          bool commit, pa_segments_tld_t* tld)

{
  pa_memid_t memid;
  bool   allow_large = (!eager_delayed && (PA_SECURE == 0)); // only allow large OS pages once we are no longer lazy
  size_t align_offset = 0;
  size_t alignment = PA_SEGMENT_ALIGN;

  if (page_alignment > 0) {
    // pa_assert_internal(huge_page != NULL);
    pa_assert_internal(page_alignment >= PA_SEGMENT_ALIGN);
    alignment = page_alignment;
    const size_t info_size = (*pinfo_slices) * PA_SEGMENT_SLICE_SIZE;
    align_offset = _pa_align_up( info_size, PA_SEGMENT_ALIGN );
    const size_t extra = align_offset - info_size;
    // recalculate due to potential guard pages
    *psegment_slices = pa_segment_calculate_slices(required + extra, pinfo_slices);
    pa_assert_internal(*psegment_slices > 0 && *psegment_slices <= UINT32_MAX);
  }

  const size_t segment_size = (*psegment_slices) * PA_SEGMENT_SLICE_SIZE;
  pa_segment_t* segment = (pa_segment_t*)_pa_arena_alloc_aligned(segment_size, alignment, align_offset, commit, allow_large, req_arena_id, &memid);
  if (segment == NULL) {
    return NULL;  // failed to allocate
  }

  // ensure metadata part of the segment is committed
  pa_commit_mask_t commit_mask;
  if (memid.initially_committed) {
    pa_commit_mask_create_full(&commit_mask);
  }
  else {
    // at least commit the info slices
    const size_t commit_needed = _pa_divide_up((*pinfo_slices)*PA_SEGMENT_SLICE_SIZE, PA_COMMIT_SIZE);
    pa_assert_internal(commit_needed>0);
    pa_commit_mask_create(0, commit_needed, &commit_mask);
    pa_assert_internal(commit_needed*PA_COMMIT_SIZE >= (*pinfo_slices)*PA_SEGMENT_SLICE_SIZE);
    if (!_pa_os_commit(segment, commit_needed*PA_COMMIT_SIZE, NULL)) {
      _pa_arena_free(segment,segment_size,0,memid);
      return NULL;
    }
  }
  pa_assert_internal(segment != NULL && (uintptr_t)segment % PA_SEGMENT_SIZE == 0);

  segment->memid = memid;
  segment->allow_decommit = !memid.is_pinned;
  segment->allow_purge = segment->allow_decommit && (pa_option_get(pa_option_purge_delay) >= 0);
  segment->segment_size = segment_size;
  segment->subproc = tld->subproc;
  segment->commit_mask = commit_mask;
  segment->purge_expire = 0;
  segment->free_is_zero = memid.initially_zero;
  pa_commit_mask_create_empty(&segment->purge_mask);

  pa_segments_track_size((long)(segment_size), tld);
  _pa_segment_map_allocated_at(segment);
  return segment;
}


// Allocate a segment from the OS aligned to `PA_SEGMENT_SIZE` .
static pa_segment_t* pa_segment_alloc(size_t required, size_t page_alignment, pa_arena_id_t req_arena_id, pa_segments_tld_t* tld, pa_page_t** huge_page)
{
  pa_assert_internal((required==0 && huge_page==NULL) || (required>0 && huge_page != NULL));

  // calculate needed sizes first
  size_t info_slices;
  size_t segment_slices = pa_segment_calculate_slices(required, &info_slices);
  pa_assert_internal(segment_slices > 0 && segment_slices <= UINT32_MAX);

  // Commit eagerly only if not the first N lazy segments (to reduce impact of many threads that allocate just a little)
  const bool eager_delay = (// !_pa_os_has_overcommit() &&             // never delay on overcommit systems
                            _pa_current_thread_count() > 1 &&       // do not delay for the first N threads
                            tld->peak_count < (size_t)pa_option_get(pa_option_eager_commit_delay));
  const bool eager = !eager_delay && pa_option_is_enabled(pa_option_eager_commit);
  bool commit = eager || (required > 0);

  // Allocate the segment from the OS
  pa_segment_t* segment = pa_segment_os_alloc(required, page_alignment, eager_delay, req_arena_id,
                                              &segment_slices, &info_slices, commit, tld);
  if (segment == NULL) return NULL;

  // zero the segment info? -- not always needed as it may be zero initialized from the OS
  if (!segment->memid.initially_zero) {
    ptrdiff_t ofs    = offsetof(pa_segment_t, next);
    size_t    prefix = offsetof(pa_segment_t, slices) - ofs;
    size_t    zsize  = prefix + (sizeof(pa_slice_t) * (segment_slices + 1)); // one more
    _pa_memzero((uint8_t*)segment + ofs, zsize);
  }

  // initialize the rest of the segment info
  const size_t slice_entries = (segment_slices > PA_SLICES_PER_SEGMENT ? PA_SLICES_PER_SEGMENT : segment_slices);
  segment->segment_slices = segment_slices;
  segment->segment_info_slices = info_slices;
  segment->thread_id = _pa_thread_id();
  segment->node_id   = (uint16_t)(unsigned)_pa_os_numa_node();  /* NUMA-aware page grouping */
  segment->cookie = _pa_ptr_cookie(segment);
  segment->slice_entries = slice_entries;
  segment->kind = (required == 0 ? PA_SEGMENT_NORMAL : PA_SEGMENT_HUGE);
  segment->purge_delay_ms = pa_option_get(pa_option_purge_delay);

  // _pa_memzero(segment->slices, sizeof(pa_slice_t)*(info_slices+1));
  _pa_stat_increase(&tld->stats->page_committed, pa_segment_info_size(segment));

  // set up guard pages
  size_t guard_slices = 0;
  if (PA_SECURE>0) {
    // in secure mode, we set up a protected page in between the segment info
    // and the page data, and at the end of the segment.
    size_t os_pagesize = _pa_os_page_size();
    _pa_os_protect((uint8_t*)segment + pa_segment_info_size(segment) - os_pagesize, os_pagesize);
    uint8_t* end = (uint8_t*)segment + pa_segment_size(segment) - os_pagesize;
    pa_segment_ensure_committed(segment, end, os_pagesize, NULL);
    _pa_os_protect(end, os_pagesize);
    if (slice_entries == segment_slices) segment->slice_entries--; // don't use the last slice :-(
    guard_slices = 1;
  }

  // reserve first slices for segment info
  pa_page_t* page0 = pa_segment_span_allocate(segment, 0, info_slices);
  pa_assert_internal(page0!=NULL); if (page0==NULL) return NULL; // cannot fail as we always commit in advance
  pa_assert_internal(segment->used == 1);
  segment->used = 0; // don't count our internal slices towards usage

  // initialize initial free pages
  if (segment->kind == PA_SEGMENT_NORMAL) { // not a huge page
    pa_assert_internal(huge_page==NULL);
    pa_segment_span_free(segment, info_slices, segment->slice_entries - info_slices, false /* don't purge */, tld);
  }
  else {
    pa_assert_internal(huge_page!=NULL);
    pa_assert_internal(pa_commit_mask_is_empty(&segment->purge_mask));
    pa_assert_internal(pa_commit_mask_is_full(&segment->commit_mask));
    *huge_page = pa_segment_span_allocate(segment, info_slices, segment_slices - info_slices - guard_slices);
    pa_assert_internal(*huge_page != NULL); // cannot fail as we commit in advance
  }

  pa_assert_expensive(pa_segment_is_valid(segment,tld));
  return segment;
}


static void pa_segment_free(pa_segment_t* segment, bool force, pa_segments_tld_t* tld) {
  PA_UNUSED(force);
  pa_assert_internal(segment != NULL);
  pa_assert_internal(segment->next == NULL);
  pa_assert_internal(segment->used == 0);

  // in `pa_segment_force_abandon` we set this to true to ensure the segment's memory stays valid
  if (segment->dont_free) return;

  // Remove the free pages
  pa_slice_t* slice = &segment->slices[0];
  const pa_slice_t* end = pa_segment_slices_end(segment);
  #if PA_DEBUG>1
  size_t page_count = 0;
  #endif
  while (slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    pa_assert_internal(pa_slice_index(slice)==0 || slice->block_size == 0); // no more used pages ..
    if (slice->block_size == 0 && segment->kind != PA_SEGMENT_HUGE) {
      pa_segment_span_remove_from_queue(slice, tld);
    }
    #if PA_DEBUG>1
    page_count++;
    #endif
    slice = slice + slice->slice_count;
  }
  pa_assert_internal(page_count == 2); // first page is allocated by the segment itself

  // stats
  // _pa_stat_decrease(&tld->stats->page_committed, pa_segment_info_size(segment));

  // return it to the OS
  pa_segment_os_free(segment, tld);
}


/* -----------------------------------------------------------
   Page Free
----------------------------------------------------------- */

static void pa_segment_abandon(pa_segment_t* segment, pa_segments_tld_t* tld);

// note: can be called on abandoned pages
static pa_slice_t* pa_segment_page_clear(pa_page_t* page, pa_segments_tld_t* tld) {
  pa_assert_internal(page->block_size > 0);
  pa_assert_internal(pa_page_all_free(page));
  pa_segment_t* segment = _pa_ptr_segment(page);
  pa_assert_internal(segment->used > 0);

  size_t inuse = page->capacity * pa_page_block_size(page);
  _pa_stat_decrease(&tld->stats->page_committed, inuse);
  _pa_stat_decrease(&tld->stats->pages, 1);
  _pa_stat_decrease(&tld->stats->page_bins[_pa_page_stats_bin(page)], 1);
  
  // reset the page memory to reduce memory pressure?
  if (segment->allow_decommit && pa_option_is_enabled(pa_option_deprecated_page_reset)) {
    size_t psize;
    uint8_t* start = _pa_segment_page_start(segment, page, &psize);
    _pa_os_reset(start, psize);
  }

  // zero the page data, but not the segment fields and heap tag
  page->is_zero_init = false;
  uint8_t heap_tag = page->heap_tag;
  ptrdiff_t ofs = offsetof(pa_page_t, capacity);
  _pa_memzero((uint8_t*)page + ofs, sizeof(*page) - ofs);
  page->block_size = 1;
  page->heap_tag = heap_tag;

  // and free it
  pa_slice_t* slice = pa_segment_span_free_coalesce(pa_page_to_slice(page), tld);
  segment->used--;
  segment->free_is_zero = false;

  // cannot assert segment valid as it is called during reclaim
  // pa_assert_expensive(pa_segment_is_valid(segment, tld));
  return slice;
}

void _pa_segment_page_free(pa_page_t* page, bool force, pa_segments_tld_t* tld)
{
  pa_assert(page != NULL);
  pa_segment_t* segment = _pa_page_segment(page);
  pa_assert_expensive(pa_segment_is_valid(segment,tld));

  // mark it as free now
  pa_segment_page_clear(page, tld);
  pa_assert_expensive(pa_segment_is_valid(segment, tld));

  if (segment->used == 0) {
    // no more used pages; remove from the free list and free the segment
    pa_segment_free(segment, force, tld);
  }
  else if (segment->used == segment->abandoned) {
    // only abandoned pages; remove from free list and abandon
    pa_segment_abandon(segment,tld);
  }
  else {
    // perform delayed purges
    pa_segment_try_purge(segment, false /* force? */);
  }
}


/* -----------------------------------------------------------
Abandonment

When threads terminate, they can leave segments with
live blocks (reachable through other threads). Such segments
are "abandoned" and will be reclaimed by other threads to
reuse their pages and/or free them eventually. The
`thread_id` of such segments is 0.

When a block is freed in an abandoned segment, the segment
is reclaimed into that thread.

Moreover, if threads are looking for a fresh segment, they
will first consider abandoned segments -- these can be found
by scanning the arena memory
(segments outside arena memoryare only reclaimed by a free).
----------------------------------------------------------- */

/* -----------------------------------------------------------
   Abandon segment/page
----------------------------------------------------------- */

static void pa_segment_abandon(pa_segment_t* segment, pa_segments_tld_t* tld) {
  pa_assert_internal(segment->used == segment->abandoned);
  pa_assert_internal(segment->used > 0);
  pa_assert_internal(segment->abandoned_visits == 0);
  pa_assert_expensive(pa_segment_is_valid(segment,tld));

  // remove the free pages from the free page queues
  pa_slice_t* slice = &segment->slices[0];
  const pa_slice_t* end = pa_segment_slices_end(segment);
  while (slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    if (slice->block_size == 0) { // a free page
      pa_segment_span_remove_from_queue(slice,tld);
      slice->block_size = 0; // but keep it free
    }
    slice = slice + slice->slice_count;
  }

  // perform delayed decommits (forcing is much slower on mstress)
  // Only abandoned segments in arena memory can be reclaimed without a free
  // so if a segment is not from an arena we force purge here to be conservative.
  const bool force_purge = (segment->memid.memkind != PA_MEM_ARENA) || pa_option_is_enabled(pa_option_abandoned_page_purge);
  pa_segment_try_purge(segment, force_purge);

  // all pages in the segment are abandoned; add it to the abandoned list
  _pa_stat_increase(&tld->stats->segments_abandoned, 1);
  pa_segments_track_size(-((long)pa_segment_size(segment)), tld);
  segment->thread_id = 0;
  segment->abandoned_visits = 1;   // from 0 to 1 to signify it is abandoned
  if (segment->was_reclaimed) {
    tld->reclaim_count--;
    segment->was_reclaimed = false;
  }
  _pa_arena_segment_mark_abandoned(segment);
}

void _pa_segment_page_abandon(pa_page_t* page, pa_segments_tld_t* tld) {
  pa_assert(page != NULL);
  pa_assert_internal(pa_page_thread_free_flag(page)==PA_NEVER_DELAYED_FREE);
  pa_assert_internal(pa_page_heap(page) == NULL);
  pa_segment_t* segment = _pa_page_segment(page);

  pa_assert_expensive(pa_segment_is_valid(segment,tld));
  segment->abandoned++;

  _pa_stat_increase(&tld->stats->pages_abandoned, 1);
  pa_assert_internal(segment->abandoned <= segment->used);
  if (segment->used == segment->abandoned) {
    // all pages are abandoned, abandon the entire segment
    pa_segment_abandon(segment, tld);
  }
}

/* -----------------------------------------------------------
  Reclaim abandoned pages
----------------------------------------------------------- */

static pa_slice_t* pa_slices_start_iterate(pa_segment_t* segment, const pa_slice_t** end) {
  pa_slice_t* slice = &segment->slices[0];
  *end = pa_segment_slices_end(segment);
  pa_assert_internal(slice->slice_count>0 && slice->block_size>0); // segment allocated page
  slice = slice + slice->slice_count; // skip the first segment allocated page
  return slice;
}

// Possibly free pages and check if free space is available
static bool pa_segment_check_free(pa_segment_t* segment, size_t slices_needed, size_t block_size, pa_segments_tld_t* tld)
{
  pa_assert_internal(pa_segment_is_abandoned(segment));
  bool has_page = false;

  // for all slices
  const pa_slice_t* end;
  pa_slice_t* slice = pa_slices_start_iterate(segment, &end);
  while (slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    if (pa_slice_is_used(slice)) { // used page
      // ensure used count is up to date and collect potential concurrent frees
      pa_page_t* const page = pa_slice_to_page(slice);
      _pa_page_free_collect(page, false);
      if (pa_page_all_free(page)) {
        // if this page is all free now, free it without adding to any queues (yet)
        pa_assert_internal(page->next == NULL && page->prev==NULL);
        _pa_stat_decrease(&tld->stats->pages_abandoned, 1);
        segment->abandoned--;
        slice = pa_segment_page_clear(page, tld); // re-assign slice due to coalesce!
        pa_assert_internal(!pa_slice_is_used(slice));
        if (slice->slice_count >= slices_needed) {
          has_page = true;
        }
      }
      else if (pa_page_block_size(page) == block_size && pa_page_has_any_available(page)) {
        // a page has available free blocks of the right size
        has_page = true;
      }
    }
    else {
      // empty span
      if (slice->slice_count >= slices_needed) {
        has_page = true;
      }
    }
    slice = slice + slice->slice_count;
  }
  return has_page;
}

// Reclaim an abandoned segment; returns NULL if the segment was freed
// set `right_page_reclaimed` to `true` if it reclaimed a page of the right `block_size` that was not full.
static pa_segment_t* pa_segment_reclaim(pa_segment_t* segment, pa_heap_t* heap, size_t requested_block_size, bool* right_page_reclaimed, pa_segments_tld_t* tld) {
  if (right_page_reclaimed != NULL) { *right_page_reclaimed = false; }
  // can be 0 still with abandoned_next, or already a thread id for segments outside an arena that are reclaimed on a free.
  pa_assert_internal(pa_atomic_load_relaxed(&segment->thread_id) == 0 || pa_atomic_load_relaxed(&segment->thread_id) == _pa_thread_id());
  pa_assert_internal(segment->subproc == heap->tld->segments.subproc); // only reclaim within the same subprocess
  pa_atomic_store_release(&segment->thread_id, _pa_thread_id());
  segment->abandoned_visits = 0;
  segment->was_reclaimed = true;
  tld->reclaim_count++;
  pa_segments_track_size((long)pa_segment_size(segment), tld);
  pa_assert_internal(segment->next == NULL);
  _pa_stat_decrease(&tld->stats->segments_abandoned, 1);

  // for all slices
  const pa_slice_t* end;
  pa_slice_t* slice = pa_slices_start_iterate(segment, &end);
  while (slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    if (pa_slice_is_used(slice)) {
      // in use: reclaim the page in our heap
      pa_page_t* page = pa_slice_to_page(slice);
      pa_assert_internal(page->is_committed);
      pa_assert_internal(pa_page_thread_free_flag(page)==PA_NEVER_DELAYED_FREE);
      pa_assert_internal(pa_page_heap(page) == NULL);
      pa_assert_internal(page->next == NULL && page->prev==NULL);
      _pa_stat_decrease(&tld->stats->pages_abandoned, 1);
      segment->abandoned--;
      // get the target heap for this thread which has a matching heap tag (so we reclaim into a matching heap)
      pa_heap_t* target_heap = _pa_heap_by_tag(heap, page->heap_tag);  // allow custom heaps to separate objects
      if (target_heap == NULL) {
        target_heap = heap;
        _pa_error_message(EFAULT, "page with tag %u cannot be reclaimed by a heap with the same tag (using heap tag %u instead)\n", page->heap_tag, heap->tag );
      }
      // associate the heap with this page, and allow heap thread delayed free again.
      pa_page_set_heap(page, target_heap);
      _pa_page_use_delayed_free(page, PA_USE_DELAYED_FREE, true); // override never (after heap is set)
      _pa_page_free_collect(page, false); // ensure used count is up to date
      if (pa_page_all_free(page)) {
        // if everything free by now, free the page
        slice = pa_segment_page_clear(page, tld);   // set slice again due to coalesceing
      }
      else {
        // otherwise reclaim it into the heap
        _pa_page_reclaim(target_heap, page);
        if (requested_block_size == pa_page_block_size(page) && pa_page_has_any_available(page) && heap == target_heap) {
          if (right_page_reclaimed != NULL) { *right_page_reclaimed = true; }
        }
      }
    }
    else {
      // the span is free, add it to our page queues
      slice = pa_segment_span_free_coalesce(slice, tld); // set slice again due to coalesceing
    }
    pa_assert_internal(slice->slice_count>0 && slice->slice_offset==0);
    slice = slice + slice->slice_count;
  }

  pa_assert(segment->abandoned == 0);
  pa_assert_expensive(pa_segment_is_valid(segment, tld));
  if (segment->used == 0) {  // due to page_clear
    pa_assert_internal(right_page_reclaimed == NULL || !(*right_page_reclaimed));
    pa_segment_free(segment, false, tld);
    return NULL;
  }
  else {
    return segment;
  }
}


// attempt to reclaim a particular segment (called from multi threaded free `alloc.c:pa_free_block_mt`)
bool _pa_segment_attempt_reclaim(pa_heap_t* heap, pa_segment_t* segment) {
  if (pa_atomic_load_relaxed(&segment->thread_id) != 0) return false;  // it is not abandoned
  if (segment->subproc != heap->tld->segments.subproc)  return false;  // only reclaim within the same subprocess
  if (!_pa_heap_memid_is_suitable(heap,segment->memid)) return false;  // don't reclaim between exclusive and non-exclusive arena's
  const long target = _pa_option_get_fast(pa_option_target_segments_per_thread);
  if (target > 0 && (size_t)target <= heap->tld->segments.count) return false; // don't reclaim if going above the target count

  // don't reclaim more from a `free` call than half the current segments
  // this is to prevent a pure free-ing thread to start owning too many segments
  // (but not for out-of-arena segments as that is the main way to be reclaimed for those)
  if (segment->memid.memkind == PA_MEM_ARENA && heap->tld->segments.reclaim_count * 2 > heap->tld->segments.count) {
    return false;
  }
  if (_pa_arena_segment_clear_abandoned(segment)) {  // atomically unabandon
    pa_segment_t* res = pa_segment_reclaim(segment, heap, 0, NULL, &heap->tld->segments);
    pa_assert_internal(res == segment);
    return (res != NULL);
  }
  return false;
}

void _pa_abandoned_reclaim_all(pa_heap_t* heap, pa_segments_tld_t* tld) {
  pa_segment_t* segment;
  pa_arena_field_cursor_t current;
  _pa_arena_field_cursor_init(heap, tld->subproc, true /* visit all, blocking */, &current);
  while ((segment = _pa_arena_segment_clear_abandoned_next(&current)) != NULL) {
    pa_segment_reclaim(segment, heap, 0, NULL, tld);
  }
  _pa_arena_field_cursor_done(&current);
}


static bool segment_count_is_within_target(pa_segments_tld_t* tld, size_t* ptarget) {
  const size_t target = (size_t)pa_option_get_clamp(pa_option_target_segments_per_thread, 0, 1024);
  if (ptarget != NULL) { *ptarget = target; }
  return (target == 0 || tld->count < target);
}

static long pa_segment_get_reclaim_tries(pa_segments_tld_t* tld) {
  // limit the tries to 10% (default) of the abandoned segments with at least 8 and at most 1024 tries.
  const size_t perc = (size_t)pa_option_get_clamp(pa_option_max_segment_reclaim, 0, 100);
  if (perc <= 0) return 0;
  const size_t total_count = pa_atomic_load_relaxed(&tld->subproc->abandoned_count);
  if (total_count == 0) return 0;
  const size_t relative_count = (total_count > 10000 ? (total_count / 100) * perc : (total_count * perc) / 100); // avoid overflow
  long max_tries = (long)(relative_count <= 1 ? 1 : (relative_count > 1024 ? 1024 : relative_count));
  if (max_tries < 8 && total_count > 8) { max_tries = 8;  }
  return max_tries;
}

static pa_segment_t* pa_segment_try_reclaim(pa_heap_t* heap, size_t needed_slices, size_t block_size, bool* reclaimed, pa_segments_tld_t* tld)
{
  *reclaimed = false;
  long max_tries = pa_segment_get_reclaim_tries(tld);
  if (max_tries <= 0) return NULL;

  pa_segment_t* result = NULL;
  pa_segment_t* segment = NULL;
  pa_arena_field_cursor_t current;
  _pa_arena_field_cursor_init(heap, tld->subproc, false /* non-blocking */, &current);
  while (segment_count_is_within_target(tld,NULL) && (max_tries-- > 0) && ((segment = _pa_arena_segment_clear_abandoned_next(&current)) != NULL))
  {
    pa_assert(segment->subproc == heap->tld->segments.subproc); // cursor only visits segments in our sub-process
    segment->abandoned_visits++;
    // todo: should we respect numa affinity for abandoned reclaim? perhaps only for the first visit?
    // todo: an arena exclusive heap will potentially visit many abandoned unsuitable segments and use many tries
    // Perhaps we can skip non-suitable ones in a better way?
    bool is_suitable = _pa_heap_memid_is_suitable(heap, segment->memid);
    bool has_page = pa_segment_check_free(segment,needed_slices,block_size,tld); // try to free up pages (due to concurrent frees)
    if (segment->used == 0) {
      // free the segment (by forced reclaim) to make it available to other threads.
      // note1: we prefer to free a segment as that might lead to reclaiming another
      // segment that is still partially used.
      // note2: we could in principle optimize this by skipping reclaim and directly
      // freeing but that would violate some invariants temporarily)
      pa_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else if (has_page && is_suitable) {
      // found a large enough free span, or a page of the right block_size with free space
      // we return the result of reclaim (which is usually `segment`) as it might free
      // the segment due to concurrent frees (in which case `NULL` is returned).
      result = pa_segment_reclaim(segment, heap, block_size, reclaimed, tld);
      break;
    }
    else if (segment->abandoned_visits > 3 && is_suitable) {
      // always reclaim on 3rd visit to limit the abandoned segment count.
      pa_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else {
      // otherwise, push on the visited list so it gets not looked at too quickly again
      max_tries++; // don't count this as a try since it was not suitable
      pa_segment_try_purge(segment, false /* true force? */); // force purge if needed as we may not visit soon again
      _pa_arena_segment_mark_abandoned(segment);
    }
  }
  _pa_arena_field_cursor_done(&current);
  return result;
}

// collect abandoned segments
void _pa_abandoned_collect(pa_heap_t* heap, bool force, pa_segments_tld_t* tld)
{
  pa_segment_t* segment;
  pa_arena_field_cursor_t current; _pa_arena_field_cursor_init(heap, tld->subproc, force /* blocking? */, &current);
  long max_tries = (force ? (long)pa_atomic_load_relaxed(&tld->subproc->abandoned_count) : 1024);  // limit latency
  while ((max_tries-- > 0) && ((segment = _pa_arena_segment_clear_abandoned_next(&current)) != NULL)) {
    pa_segment_check_free(segment,0,0,tld); // try to free up pages (due to concurrent frees)
    if (segment->used == 0) {
      // free the segment (by forced reclaim) to make it available to other threads.
      // note: we could in principle optimize this by skipping reclaim and directly
      // freeing but that would violate some invariants temporarily)
      pa_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else {
      // otherwise, purge if needed and push on the visited list
      // note: forced purge can be expensive if many threads are destroyed/created as in mstress.
      pa_segment_try_purge(segment, force);
      _pa_arena_segment_mark_abandoned(segment);
    }
  }
  _pa_arena_field_cursor_done(&current);
}

/* -----------------------------------------------------------
   Force abandon a segment that is in use by our thread
----------------------------------------------------------- */

// force abandon a segment
static void pa_segment_force_abandon(pa_segment_t* segment, pa_segments_tld_t* tld)
{
  pa_assert_internal(!pa_segment_is_abandoned(segment));
  pa_assert_internal(!segment->dont_free);

  // ensure the segment does not get free'd underneath us (so we can check if a page has been freed in `pa_page_force_abandon`)
  segment->dont_free = true;

  // for all slices
  const pa_slice_t* end;
  pa_slice_t* slice = pa_slices_start_iterate(segment, &end);
  while (slice < end) {
    pa_assert_internal(slice->slice_count > 0);
    pa_assert_internal(slice->slice_offset == 0);
    if (pa_slice_is_used(slice)) {
      // ensure used count is up to date and collect potential concurrent frees
      pa_page_t* const page = pa_slice_to_page(slice);
      _pa_page_free_collect(page, false);
      {
        // abandon the page if it is still in-use (this will free it if possible as well)
        pa_assert_internal(segment->used > 0);
        if (segment->used == segment->abandoned+1) {
          // the last page.. abandon and return as the segment will be abandoned after this
          // and we should no longer access it.
          segment->dont_free = false;
          _pa_page_force_abandon(page);
          return;
        }
        else {
          // abandon and continue
          _pa_page_force_abandon(page);
          // it might be freed, reset the slice (note: relies on coalesce setting the slice_offset)
          slice = pa_slice_first(slice);
        }
      }
    }
    slice = slice + slice->slice_count;
  }
  segment->dont_free = false;
  pa_assert(segment->used == segment->abandoned);
  pa_assert(segment->used == 0);
  if (segment->used == 0) {  // paranoia
    // all free now
    pa_segment_free(segment, false, tld);
  }
  else {
    // perform delayed purges
    pa_segment_try_purge(segment, false /* force? */);
  }
}


// try abandon segments.
// this should be called from `reclaim_or_alloc` so we know all segments are (about) fully in use.
static void pa_segments_try_abandon_to_target(pa_heap_t* heap, size_t target, pa_segments_tld_t* tld) {
  if (target <= 1) return;
  const size_t min_target = (target > 4 ? (target*3)/4 : target);  // 75%
  // todo: we should maintain a list of segments per thread; for now, only consider segments from the heap full pages
  for (int i = 0; i < 64 && tld->count >= min_target; i++) {
    pa_page_t* page = heap->pages[PA_BIN_FULL].first;
    while (page != NULL && pa_page_block_size(page) > PA_LARGE_OBJ_SIZE_MAX) {
      page = page->next;
    }
    if (page==NULL) {
      break;
    }
    pa_segment_t* segment = _pa_page_segment(page);
    pa_segment_force_abandon(segment, tld);
    pa_assert_internal(page != heap->pages[PA_BIN_FULL].first); // as it is just abandoned
  }
}

// try abandon segments.
// this should be called from `reclaim_or_alloc` so we know all segments are (about) fully in use.
static void pa_segments_try_abandon(pa_heap_t* heap, pa_segments_tld_t* tld) {
  // we call this when we are about to add a fresh segment so we should be under our target segment count.
  size_t target = 0;
  if (segment_count_is_within_target(tld, &target)) return;
  pa_segments_try_abandon_to_target(heap, target, tld);
}

void pa_collect_reduce(size_t target_size) pa_attr_noexcept {
  pa_collect(true);
  pa_heap_t* heap = pa_heap_get_default();
  pa_segments_tld_t* tld = &heap->tld->segments;
  size_t target = target_size / PA_SEGMENT_SIZE;
  if (target == 0) {
    target = (size_t)pa_option_get_clamp(pa_option_target_segments_per_thread, 1, 1024);
  }
  pa_segments_try_abandon_to_target(heap, target, tld);
}

/* -----------------------------------------------------------
   Reclaim or allocate
----------------------------------------------------------- */

static pa_segment_t* pa_segment_reclaim_or_alloc(pa_heap_t* heap, size_t needed_slices, size_t block_size, pa_segments_tld_t* tld)
{
  pa_assert_internal(block_size <= PA_LARGE_OBJ_SIZE_MAX);

  // try to abandon some segments to increase reuse between threads
  pa_segments_try_abandon(heap,tld);

  // 1. try to reclaim an abandoned segment
  bool reclaimed;
  pa_segment_t* segment = pa_segment_try_reclaim(heap, needed_slices, block_size, &reclaimed, tld);
  if (reclaimed) {
    // reclaimed the right page right into the heap
    pa_assert_internal(segment != NULL);
    return NULL; // pretend out-of-memory as the page will be in the page queue of the heap with available blocks
  }
  else if (segment != NULL) {
    // reclaimed a segment with a large enough empty span in it
    return segment;
  }
  // 2. otherwise allocate a fresh segment
  return pa_segment_alloc(0, 0, heap->arena_id, tld, NULL);
}


/* -----------------------------------------------------------
   Page allocation
----------------------------------------------------------- */

static pa_page_t* pa_segments_page_alloc(pa_heap_t* heap, pa_page_kind_t page_kind, size_t required, size_t block_size, pa_segments_tld_t* tld)
{
  pa_assert_internal(required <= PA_LARGE_OBJ_SIZE_MAX && page_kind <= PA_PAGE_LARGE);

  // find a free page
  size_t page_size = _pa_align_up(required, (required > PA_MEDIUM_PAGE_SIZE ? PA_MEDIUM_PAGE_SIZE : PA_SEGMENT_SLICE_SIZE));
  size_t slices_needed = page_size / PA_SEGMENT_SLICE_SIZE;
  pa_assert_internal(slices_needed * PA_SEGMENT_SLICE_SIZE == page_size);
  pa_page_t* page = pa_segments_page_find_and_allocate(slices_needed, heap->arena_id, tld); //(required <= PA_SMALL_SIZE_MAX ? 0 : slices_needed), tld);
  if (page==NULL) {
    // no free page, allocate a new segment and try again
    if (pa_segment_reclaim_or_alloc(heap, slices_needed, block_size, tld) == NULL) {
      // OOM or reclaimed a good page in the heap
      return NULL;
    }
    else {
      // otherwise try again
      return pa_segments_page_alloc(heap, page_kind, required, block_size, tld);
    }
  }
  pa_assert_internal(page != NULL && page->slice_count*PA_SEGMENT_SLICE_SIZE == page_size);
  pa_assert_internal(_pa_ptr_segment(page)->thread_id == _pa_thread_id());
  pa_segment_try_purge(_pa_ptr_segment(page), false);
  return page;
}



/* -----------------------------------------------------------
   Huge page allocation
----------------------------------------------------------- */

static pa_page_t* pa_segment_huge_page_alloc(size_t size, size_t page_alignment, pa_arena_id_t req_arena_id, pa_segments_tld_t* tld)
{
  pa_page_t* page = NULL;
  pa_segment_t* segment = pa_segment_alloc(size,page_alignment,req_arena_id,tld,&page);
  if (segment == NULL || page==NULL) return NULL;
  pa_assert_internal(segment->used==1);
  pa_assert_internal(pa_page_block_size(page) >= size);
  #if PA_HUGE_PAGE_ABANDON
  segment->thread_id = 0; // huge segments are immediately abandoned
  #endif

  // for huge pages we initialize the block_size as we may
  // overallocate to accommodate large alignments.
  size_t psize;
  uint8_t* start = _pa_segment_page_start(segment, page, &psize);
  page->block_size = psize;
  pa_assert_internal(page->is_huge);

  // decommit the part of the prefix of a page that will not be used; this can be quite large (close to PA_SEGMENT_SIZE)
  if (page_alignment > 0 && segment->allow_decommit) {
    uint8_t* aligned_p = (uint8_t*)_pa_align_up((uintptr_t)start, page_alignment);
    pa_assert_internal(_pa_is_aligned(aligned_p, page_alignment));
    pa_assert_internal(psize - (aligned_p - start) >= size);
    uint8_t* decommit_start = start + sizeof(pa_block_t);              // for the free list
    ptrdiff_t decommit_size = aligned_p - decommit_start;
    _pa_os_reset(decommit_start, decommit_size);   // note: cannot use segment_decommit on huge segments
  }

  return page;
}

#if PA_HUGE_PAGE_ABANDON
// free huge block from another thread
void _pa_segment_huge_page_free(pa_segment_t* segment, pa_page_t* page, pa_block_t* block) {
  // huge page segments are always abandoned and can be freed immediately by any thread
  pa_assert_internal(segment->kind==PA_SEGMENT_HUGE);
  pa_assert_internal(segment == _pa_page_segment(page));
  pa_assert_internal(pa_atomic_load_relaxed(&segment->thread_id)==0);

  // claim it and free
  pa_heap_t* heap = pa_heap_get_default(); // issue #221; don't use the internal get_default_heap as we need to ensure the thread is initialized.
  // paranoia: if this it the last reference, the cas should always succeed
  size_t expected_tid = 0;
  if (pa_atomic_cas_strong_acq_rel(&segment->thread_id, &expected_tid, heap->thread_id)) {
    pa_block_set_next(page, block, page->free);
    page->free = block;
    page->used--;
    page->is_zero_init = false;
    pa_assert(page->used == 0);
    pa_tld_t* tld = heap->tld;
    _pa_segment_page_free(page, true, &tld->segments);
  }
#if (PA_DEBUG!=0)
  else {
    pa_assert_internal(false);
  }
#endif
}

#else
// reset memory of a huge block from another thread
void _pa_segment_huge_page_reset(pa_segment_t* segment, pa_page_t* page, pa_block_t* block) {
  PA_UNUSED(page);
  pa_assert_internal(segment->kind == PA_SEGMENT_HUGE);
  pa_assert_internal(segment == _pa_page_segment(page));
  pa_assert_internal(page->used == 1); // this is called just before the free
  pa_assert_internal(page->free == NULL);
  if (segment->allow_decommit) {
    size_t csize = pa_usable_size(block);
    if (csize > sizeof(pa_block_t)) {
      csize = csize - sizeof(pa_block_t);
      uint8_t* p = (uint8_t*)block + sizeof(pa_block_t);
      _pa_os_reset(p, csize);  // note: cannot use segment_decommit on huge segments
    }
  }
}
#endif

/* -----------------------------------------------------------
   Page allocation and free
----------------------------------------------------------- */
pa_page_t* _pa_segment_page_alloc(pa_heap_t* heap, size_t block_size, size_t page_alignment, pa_segments_tld_t* tld) {
  pa_page_t* page;
  if pa_unlikely(page_alignment > PA_BLOCK_ALIGNMENT_MAX) {
    pa_assert_internal(_pa_is_power_of_two(page_alignment));
    pa_assert_internal(page_alignment >= PA_SEGMENT_SIZE);
    if (page_alignment < PA_SEGMENT_SIZE) { page_alignment = PA_SEGMENT_SIZE; }
    page = pa_segment_huge_page_alloc(block_size,page_alignment,heap->arena_id,tld);
  }
  else if (block_size <= PA_SMALL_OBJ_SIZE_MAX) {
    page = pa_segments_page_alloc(heap,PA_PAGE_SMALL,block_size,block_size,tld);
  }
  else if (block_size <= PA_MEDIUM_OBJ_SIZE_MAX) {
    page = pa_segments_page_alloc(heap,PA_PAGE_MEDIUM,PA_MEDIUM_PAGE_SIZE,block_size,tld);
  }
  else if (block_size <= PA_LARGE_OBJ_SIZE_MAX) {
    page = pa_segments_page_alloc(heap,PA_PAGE_LARGE,block_size,block_size,tld);
  }
  else {
    page = pa_segment_huge_page_alloc(block_size,page_alignment,heap->arena_id,tld);
  }
  pa_assert_internal(page == NULL || _pa_heap_memid_is_suitable(heap, _pa_page_segment(page)->memid));
  pa_assert_expensive(page == NULL || pa_segment_is_valid(_pa_page_segment(page),tld));
  pa_assert_internal(page == NULL || _pa_page_segment(page)->subproc == tld->subproc);
  return page;
}


/* -----------------------------------------------------------
   Visit blocks in a segment (only used for abandoned segments)
----------------------------------------------------------- */

static bool pa_segment_visit_page(pa_page_t* page, bool visit_blocks, pa_block_visit_fun* visitor, void* arg) {
  pa_heap_area_t area;
  _pa_heap_area_init(&area, page);
  if (!visitor(NULL, &area, NULL, area.block_size, arg)) return false;
  if (visit_blocks) {
    return _pa_heap_area_visit_blocks(&area, page, visitor, arg);
  }
  else {
    return true;
  }
}

bool _pa_segment_visit_blocks(pa_segment_t* segment, int heap_tag, bool visit_blocks, pa_block_visit_fun* visitor, void* arg) {
  const pa_slice_t* end;
  pa_slice_t* slice = pa_slices_start_iterate(segment, &end);
  while (slice < end) {
    if (pa_slice_is_used(slice)) {
      pa_page_t* const page = pa_slice_to_page(slice);
      if (heap_tag < 0 || (int)page->heap_tag == heap_tag) {
        if (!pa_segment_visit_page(page, visit_blocks, visitor, arg)) return false;
      }
    }
    slice = slice + slice->slice_count;
  }
  return true;
}
