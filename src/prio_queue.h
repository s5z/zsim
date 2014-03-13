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

#ifndef PRIO_QUEUE_H_
#define PRIO_QUEUE_H_

#include "g_std/g_multimap.h"

template <typename T, uint32_t B>
class PrioQueue {
    struct PQBlock {
        T* array[64];
        uint64_t occ; // bit i is 1 if array[i] is populated

        PQBlock() {
            for (uint32_t i = 0; i < 64; i++) array[i] = NULL;
            occ = 0;
        }

        inline T* dequeue(uint32_t& offset) {
            assert(occ);
            uint32_t pos = __builtin_ctzl(occ);
            T* res = array[pos];
            T* next = res->next;
            array[pos] = next;
            if (!next) occ ^= 1L << pos;
            assert(res);
            offset = pos;
            res->next = NULL;
            return res;
        }

        inline void enqueue(T* obj, uint32_t pos) {
            occ |= 1L << pos;
            assert(!obj->next);
            obj->next = array[pos];
            array[pos] = obj;
        }
    };

    PQBlock blocks[B];

    typedef g_multimap<uint64_t, T*> FEMap; //far element map
    typedef typename FEMap::iterator FEMapIterator;

    FEMap feMap;

    uint64_t curBlock;
    uint64_t elems;

    public:
        PrioQueue() {
            curBlock = 0;
            elems = 0;
        }

        void enqueue(T* obj, uint64_t cycle) {
            uint64_t absBlock = cycle/64;
            assert(absBlock >= curBlock);

            if (absBlock < curBlock + B) {
                uint32_t i = absBlock % B;
                uint32_t offset = cycle % 64;
                blocks[i].enqueue(obj, offset);
            } else {
                //info("XXX far enq() %ld", cycle);
                feMap.insert(std::pair<uint64_t, T*>(cycle, obj));
            }
            elems++;
        }

        T* dequeue(uint64_t& deqCycle) {
            assert(elems);
            while (!blocks[curBlock % B].occ) {
                curBlock++;
                if ((curBlock % (B/2)) == 0 && !feMap.empty()) {
                    uint64_t topCycle = (curBlock + B)*64;
                    //Move every element with cycle < topCycle to blocks[]
                    FEMapIterator it = feMap.begin();
                    while (it != feMap.end() && it->first < topCycle) {
                        uint64_t cycle = it->first;
                        T* obj = it->second;

                        uint64_t absBlock = cycle/64;
                        assert(absBlock >= curBlock);
                        assert(absBlock < curBlock + B);
                        uint32_t i = absBlock % B;
                        uint32_t offset = cycle % 64;
                        blocks[i].enqueue(obj, offset);
                        it++;
                    }
                    feMap.erase(feMap.begin(), it);
                }
            }

            //We're now at the first populated block
            uint32_t offset;
            T* obj = blocks[curBlock % B].dequeue(offset);
            elems--;

            deqCycle = curBlock*64 + offset;
            return obj;
        }

        inline uint64_t size() const {
            return elems;
        }

        inline uint64_t firstCycle() const {
            assert(elems);
            for (uint32_t i = 0; i < B/2; i++) {
                uint64_t occ = blocks[(curBlock + i) % B].occ;
                if (occ) {
                    uint64_t pos = __builtin_ctzl(occ);
                    return (curBlock + i)*64 + pos;
                }
            }
            for (uint32_t i = B/2; i < B; i++) { //beyond B/2 blocks, there may be a far element that comes earlier
                uint64_t occ = blocks[(curBlock + i) % B].occ;
                if (occ) {
                    uint64_t pos = __builtin_ctzl(occ);
                    uint64_t cycle = (curBlock + i)*64 + pos;
                    return feMap.empty()? cycle : MIN(cycle, feMap.begin()->first);
                }
            }

            return feMap.begin()->first;
        }
};

#endif  // PRIO_QUEUE_H_

