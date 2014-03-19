/** $lic$
 * Copyright (C) 2012-2014 by Massachusetts Institute of Technology
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

#ifndef PHASE_SLAB_ALLOC_H_
#define PHASE_SLAB_ALLOC_H_

#include <stdint.h>
#include <string>
#include "g_std/g_list.h"
#include "log.h"
#include "pad.h"

class PhaseSlabAlloc {
    private:
        struct Slab {
            Slab* next;
            uint32_t size;
            uint32_t used;

            char buf[0]; //buffer starts here

            void init(uint32_t sz) {
                size = sz;
                clear();
            }

            void clear() {
                used = 0;
                next = nullptr;
                //memset(buf, 0, size); //zeroing the slab can help chase memory corruption bugs
            }

            void* alloc(uint32_t bytes) {
#if 1 //no effort to align, but objs are a multiple of 8 bytes, so all allocs are as well
                char* ptr = buf+used;
                used += bytes;
#else //align to some block size --- performs worse in my analysis, the loss in locality does not compensate
#define ALIGN_SZ 64
                char* base = buf+used;
                char* ptr = static_cast<char*>(((uint64_t)(base+(ALIGN_SZ))) & (-ALIGN_SZ)); //aligned
                used = (ptr-buf)+bytes;
#endif
                //info("Allocation stating at %p, %d bytes", ptr, bytes);
                return (used < size)? ptr : nullptr;
            }
        };

        //SLL, intrusive, LIFO, LIFO prepend. Pass by value, it's just 2 pointers
        class SlabList {
            private:
                Slab* start;
                Slab* end;

            public:
                SlabList() : start(nullptr), end(nullptr) {}

            void push_front(Slab* s) {
                assert(s);
                assert(s->next == nullptr);
                s->next = start;
                start = s;
                if (!end) end = s;
            }

            Slab* pop_front() {
                assert(start);
                Slab* res = start;
                start = start->next;
                if (res == end) {
                    assert(start == nullptr);
                    end = nullptr;
                }
                return res;
            }

            void prepend(SlabList& lst) {
                if (lst.start == nullptr) { //lst is empty
                    assert(lst.end == nullptr);
                } else {
                    assert(lst.end);
                    assert(lst.end->next == nullptr);
                    lst.end->next = start;
                    start = lst.start;
                    if (!end) end = lst.end; //we could be empty
                }
            }

            void clear() {
                start = nullptr;
                end = nullptr;
            }

            bool empty() const {
                return !start;
            }
        };

        Slab* curSlab;
        SlabList freeList;
        SlabList curPhaseList;

        g_list<std::pair<SlabList, uint64_t> > liveList;

        uint32_t slabSize;

    public:
        PhaseSlabAlloc() {
            //slabSize = (1<<12); //4KB, too small
            slabSize = (1<<16); //64KB, seems to be sweet spot in a number of tests, though I tried 32KB-256KB and the differences are minimal in that range (2.3% weave time)
            curSlab = nullptr;
            freeList.clear();
            curPhaseList.clear();
            allocSlab();
        }

        template <typename T>
        T* alloc() {
            assert(sizeof(T) < slabSize);
            T* ptr = static_cast<T*>(curSlab->alloc(sizeof(T)));
            if (unlikely(!ptr)) {
                allocSlab();
                ptr = static_cast<T*>(curSlab->alloc(sizeof(T)));
                assert(ptr);
            }
            return ptr;
        }

        void* alloc(size_t sz) {
            assert(sz < slabSize);
            void* ptr = curSlab->alloc(sz);
            if (unlikely(!ptr)) {
                allocSlab();
                ptr = curSlab->alloc(sz);
                assert(ptr);
            }
            return ptr;
        }



        //Every event currently produced is < prodCycle, every event < usedCycle is dead (has already been simulated)
        void advance(uint64_t prodCycle, uint64_t usedCycle) {
            if (!curPhaseList.empty()) {
                liveList.push_back(std::make_pair(curPhaseList, prodCycle));
                curPhaseList.clear();
            }

            while (!liveList.empty()) {
                std::pair<SlabList, uint64_t> p = liveList.front();
                uint64_t cycle = p.second;
                if (cycle < usedCycle) {
                    freeList.prepend(p.first);
                    liveList.pop_front();
                    //info("(%ld, %ld) Recycling %ld, %ld left", prodCycle, usedCycle, cycle, liveList.size());
                } else {
                    break;
                }
            }
        }

    private:
        void allocSlab() {
            if (curSlab) curPhaseList.push_front(curSlab);

            if (!freeList.empty()) {
                curSlab = freeList.pop_front();
                assert(curSlab);
                curSlab->clear();
            } else {
                curSlab = static_cast<Slab*>(gm_malloc(sizeof(Slab) + slabSize));
                curSlab->init(slabSize); //NOTE: Slab is POD
            }
        }
};

#endif  // PHASE_SLAB_ALLOC_H_
