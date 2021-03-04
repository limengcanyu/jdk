/*
 * Copyright (c) 2017, 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1FullCollector.hpp"
#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/g1FullGCCompactTask.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/ticks.hpp"

class G1ResetPinnedClosure : public HeapRegionClosure {
  G1CMBitMap* _bitmap;

public:
  G1ResetPinnedClosure(G1CMBitMap* bitmap) : _bitmap(bitmap) { }

  bool do_heap_region(HeapRegion* r) {
    if (!r->is_pinned()) {
      return false;
    }
    assert(!r->is_starts_humongous() || _bitmap->is_marked((oop)r->bottom()),
           "must be, otherwise reclaimed earlier");
    r->reset_pinned_after_full_gc();
    return false;
  }
};

size_t G1FullGCCompactTask::G1CompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());
  if (destination == NULL) {
    // Object not moving
    return size;
  }

  // copy object and reinit its mark
  HeapWord* obj_addr = cast_from_oop<HeapWord*>(obj);
  assert(obj_addr != destination, "everything in this pass should be moving");
  Copy::aligned_conjoint_words(obj_addr, destination, size);
  oop(destination)->init_mark();
  assert(oop(destination)->klass() != NULL, "should have a class");

  return size;
}

void G1FullGCCompactTask::compact_region(HeapRegion* hr) {
  assert(!hr->is_pinned(), "Should be no pinned region in compaction queue");
  assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");
  G1CompactRegionClosure compact(collector()->mark_bitmap());
  hr->apply_to_marked_objects(collector()->mark_bitmap(), &compact);
  // Clear the liveness information for this region if necessary i.e. if we actually look at it
  // for bitmap verification. Otherwise it is sufficient that we move the TAMS to bottom().
  if (G1VerifyBitmaps) {
    collector()->mark_bitmap()->clear_region(hr);
  }
  hr->reset_compacted_after_full_gc();
}

void G1FullGCCompactTask::process_skipping_compaction_region(HeapRegion* hr) {
  collector()->mark_bitmap()->clear_region(hr);
  hr->reset_no_compaction_region_during_compaction();
}

void G1FullGCCompactTask::work(uint worker_id) {
  Ticks start = Ticks::now();
  GrowableArray<HeapRegion*>* compaction_queue = collector()->compaction_point(worker_id)->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }

  if (MarkSweepDeadRatio > 0) {
    GrowableArray<HeapRegion*>* skipping_compaction_queue = collector()->skipping_compaction_set(worker_id);
    for (GrowableArrayIterator<HeapRegion*> it = skipping_compaction_queue->begin();
         it != skipping_compaction_queue->end();
         ++it) {
      process_skipping_compaction_region(*it);
    }
  }

  G1ResetPinnedClosure hc(collector()->mark_bitmap());
  G1CollectedHeap::heap()->heap_region_par_iterate_from_worker_offset(&hc, &_claimer, worker_id);
  log_task("Compaction task", worker_id, start);
}

void G1FullGCCompactTask::serial_compaction() {
  GCTraceTime(Debug, gc, phases) tm("Phase 4: Serial Compaction", collector()->scope()->timer());
  GrowableArray<HeapRegion*>* compaction_queue = collector()->serial_compaction_point()->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }
}
