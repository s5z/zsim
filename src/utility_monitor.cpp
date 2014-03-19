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

#include "utility_monitor.h"
#include "hash.h"

#define DEBUG_UMON 0
//#define DEBUG_UMON 1

UMon::UMon(uint32_t _bankLines, uint32_t _umonLines, uint32_t _buckets) {
    umonLines = _umonLines;
    buckets = _buckets;
    samplingFactor = _bankLines/umonLines;
    sets = umonLines/buckets;

    heads = gm_calloc<Node*>(sets);
    array = gm_calloc<Node*>(sets);
    for (uint32_t i = 0; i < sets; i++) {
        array[i] = gm_calloc<Node>(buckets);
        heads[i] = &array[i][0];
        for (uint32_t j = 0; j < buckets-1; j++) {
            array[i][j].next = &array[i][j+1];
        }
    }

    curWayHits = gm_calloc<uint64_t>(buckets);
    curMisses = 0;

    hf = new H3HashFamily(2, 32, 0xF000BAAD);

    samplingFactorBits = 0;
    uint32_t tmp = samplingFactor;
    while (tmp >>= 1) samplingFactorBits++;

    setsBits = 0;
    tmp = sets;
    while (tmp >>= 1) setsBits++;
}

void UMon::initStats(AggregateStat* parentStat) {
    profWayHits.init("hits", "Sampled hits per bucket", buckets); parentStat->append(&profWayHits);
    profMisses.init("misses", "Sampled misses"); parentStat->append(&profMisses);
}


void UMon::access(Address lineAddr) {
    //1. Hash to decide if it should go in the cache
    uint64_t sampleMask = ~(((uint64_t)-1LL) << samplingFactorBits);
    uint64_t sampleSel = (hf->hash(0, lineAddr)) & sampleMask;

    //info("0x%lx 0x%lx", sampleMask, sampleSel);

    if (sampleSel != 0) {
        return;
    }

    //2. Insert; hit or miss?
    uint64_t setMask = ~(((uint64_t)-1LL) << setsBits);
    uint64_t set = (hf->hash(1, lineAddr)) & setMask;

    // Check hit
    Node* prev = nullptr;
    Node* cur = heads[set];
    bool hit = false;
    for (uint32_t b = 0; b < buckets; b++) {
        if (cur->addr == lineAddr) { //Hit at position b, profile
            //profHits.inc();
            //profWayHits.inc(b);
            curWayHits[b]++;
            hit = true;
            break;
        } else if (b < buckets-1) {
            prev = cur;
            cur = cur->next;
        }
    }

    //Profile miss, kick cur out, put lineAddr in
    if (!hit) {
        curMisses++;
        //profMisses.inc();
        assert(cur->next == nullptr);
        cur->addr = lineAddr;
    }

    //Move cur to MRU (happens regardless of whether this is a hit or a miss)
    if (prev) {
        prev->next = cur->next;
        cur->next = heads[set];
        heads[set] = cur;
    }
}

uint64_t UMon::getNumAccesses() const {
    uint64_t total = curMisses;
    for (uint32_t i = 0; i < buckets; i++) {
        total += curWayHits[buckets - i - 1];
    }
    return total;
}

void UMon::getMisses(uint64_t* misses) {
    uint64_t total = curMisses;
    for (uint32_t i = 0; i < buckets; i++) {
        misses[buckets - i] = total;
        total += curWayHits[buckets - i - 1];
    }
    misses[0] = total;
#if DEBUG_UMON
    info("UMON miss utility curve:");
    for (uint32_t i = 0; i <= buckets; i++) info(" misses[%d] = %ld", i, misses[i]);
#endif
}


void UMon::startNextInterval() {
curMisses = 0;
                for (uint32_t b = 0; b < buckets; b++) {
                    curWayHits[b] = 0;
                }
}

