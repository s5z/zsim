/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SLAB_ALLOC_H_
#define SLAB_ALLOC_H_

/* Slab allocator for timing events
 *
 * Each EventRecorder includes a slab allocator, and all timing events that are
 * in access paths, as well as TimingEventBlocks, are allocated there. Slabs
 * are garbage-collected once all their events are done. To do this without space
 * overheads, slabs are carefully aligned, so that objects inside the slab can
 * derive the pointer of their slab.
 */

#include <deque>
#include <stddef.h>
#include <stdint.h>
#include "g_std/g_vector.h"
#include "log.h"
#include "mutex.h"

#define SLAB_SIZE (1<<16)  // 64KB; must be a power of two
#define SLAB_MASK (~(SLAB_SIZE - 1))

// Uncomment to immediately scrub slabs (to 0) and freed elems (to -1).
// This makes use-after-free errors obvious.
//#define DEBUG_SLAB_ALLOC

namespace slab {

class SlabAlloc;

struct Slab {  // POD type (no constructor)
    SlabAlloc* allocator;
    volatile uint32_t liveElems;
    uint32_t usedBytes;
    char buf[SLAB_SIZE - sizeof(SlabAlloc*) - sizeof(volatile uint32_t) - sizeof(uint32_t)];

    void init(SlabAlloc* _allocator) {
        allocator = _allocator;
        clear();
    }

    void clear() {
        liveElems = 0;
        usedBytes = 0;
    }

    void* alloc(uint32_t bytes) {
#if 1 //no effort to align, but objs are a multiple of 8 bytes, so all allocs are as well
        char* ptr = buf + usedBytes;
        usedBytes += bytes;
#else //align to some block size --- performs worse in my analysis, the loss in locality does not compensate
#define ALIGN_SZ 64
        char* base = buf+used;
        char* ptr = static_cast<char*>(((uint64_t)(base+(ALIGN_SZ))) & (-ALIGN_SZ)); //aligned
        used = (ptr-buf)+bytes;
#endif
        //info("Allocation starting at %p, %d bytes", ptr, bytes);
        if (usedBytes < sizeof(buf)) {
            liveElems++;  // allocation is unsynced, no need for atomic op
            return ptr;
        } else {
            return nullptr;
        }
    }

    inline void freeElem();
};

class SlabAlloc {
    private:
        Slab* curSlab;
        g_vector<Slab*> freeList;
        uint32_t liveSlabs;
        mutex freeLock;  // used because slab frees may be concurrent

    public:
        SlabAlloc() : curSlab(nullptr), liveSlabs(0) {
            allocSlab();
        }

        void* alloc(size_t sz) {
            assert(sz < SLAB_SIZE);
            void* ptr = curSlab->alloc(sz);
            if (unlikely(!ptr)) {
                allocSlab();
                ptr = curSlab->alloc(sz);
                assert(ptr);
            }
            assert((((uintptr_t)ptr) & SLAB_MASK) == (uintptr_t)curSlab)
            return ptr;
        }

        template <typename T> T* alloc() { return (T*)alloc(sizeof(T)); }

    private:
        void allocSlab() {
            scoped_mutex sm(freeLock);
            if (!freeList.empty()) {
                curSlab = freeList.back();
                freeList.pop_back();
                assert(curSlab);
            } else {
                assert(sizeof(Slab) == SLAB_SIZE);
                curSlab = gm_memalign<Slab>(sizeof(Slab));
                assert((((uintptr_t)curSlab) & SLAB_MASK) == (uintptr_t)curSlab);
                curSlab->init(this);  // NOTE: Slab is POD
            }
            liveSlabs++;
            //info("allocated slab %p, %d live, %ld in freeList", curSlab, liveSlabs, freeList.size());
        }

        void freeSlab(Slab* s) {
            scoped_mutex sm(freeLock);
            //info("freeing slab %p, %d live, %ld in freeList", s, liveSlabs, freeList.size());
            s->clear();
#ifdef DEBUG_SLAB_ALLOC
            memset(s->buf, -1, sizeof(s->buf));
#endif
            if (s != curSlab) {
                freeList.push_back(s);
                liveSlabs--;
            }
            assert(liveSlabs);  // at least curSlab
        }

        friend struct Slab;
};

inline void Slab::freeElem() {
    uint32_t prevLiveElems = __sync_fetch_and_sub(&liveElems, 1);
    assert(prevLiveElems && prevLiveElems < usedBytes /* >= 1 bytes/obj*/);
    //info("[%p] Slab::freeElem %d prevLiveElems", this, prevLiveElems);
    if (prevLiveElems == 1) {
        allocator->freeSlab(this);
    }
}

inline void freeElem(void* elem, size_t minSz) {
#ifdef DEBUG_SLAB_ALLOC
    memset(elem, 0, minSz);
#endif
    Slab* s = (Slab*)(((uintptr_t)elem) & SLAB_MASK);
    s->freeElem();
}

};  // namespace slab

#endif  // SLAB_ALLOC_H_
