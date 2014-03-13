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

#ifndef UTILITY_MONITOR_H_
#define UTILITY_MONITOR_H_

#include "galloc.h"
#include "memory_hierarchy.h"
#include "stats.h"

//Print some information regarding utility monitors and partitioning
#define UMON_INFO 0
//#define UMON_INFO 1

class HashFamily;

class UMon : public GlobAlloc {
    private:
        uint32_t umonLines;
        uint32_t samplingFactor; //Size of sampled cache (lines)/size of umon. Should be power of 2
        uint32_t buckets; //umon ways
        uint32_t sets; //umon sets. Should be power of 2.

        //Used in masks for set indices and sampling factor descisions
        uint64_t samplingFactorBits;
        uint64_t setsBits;

        uint64_t* curWayHits;
        uint64_t curMisses;

        Counter profHits;
        Counter profMisses;
        VectorCounter profWayHits;

        //Even for high associativity/number of buckets, performance of this is not important because we downsample so much (so this is a LL)
        struct Node {
            Address addr;
            struct Node* next;
        };
        Node** array;
        Node** heads;

        HashFamily* hf;

    public:
        UMon(uint32_t _bankLines, uint32_t _umonLines, uint32_t _buckets);
        void initStats(AggregateStat* parentStat);

        void access(Address lineAddr);

        uint64_t getNumAccesses() const;
        void getMisses(uint64_t* misses);
        void startNextInterval();

        uint32_t getBuckets() const { return buckets; }
};

#endif  // UTILITY_MONITOR_H_

