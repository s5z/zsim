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

#ifndef TIMING_CACHE_H_
#define TIMING_CACHE_H_

#include "breakdown_stats.h"
#include "cache.h"

class HitEvent;
class MissStartEvent;
class MissResponseEvent;
class MissWritebackEvent;
class ReplAccessEvent;
class TimingEvent;

class TimingCache : public Cache {
    private:
        uint64_t lastAccCycle, lastFreeCycle;
        uint32_t numMSHRs, activeMisses;
        g_vector<TimingEvent*> pendingQueue;

        // Stats
        CycleBreakdownStat profOccHist;
        Counter profHitLat, profMissRespLat, profMissLat;

        uint32_t domain;

        // For zcache replacement simulation (pessimistic, assumes we walk the whole tree)
        uint32_t tagLat, ways, cands;

        PAD();
        lock_t topLock;
        PAD();

    public:
        TimingCache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs,
                uint32_t tagLat, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name);
        void initStats(AggregateStat* parentStat);

        uint64_t access(MemReq& req);

        void simulateHit(HitEvent* ev, uint64_t cycle);
        void simulateMissStart(MissStartEvent* ev, uint64_t cycle);
        void simulateMissResponse(MissResponseEvent* ev, uint64_t cycle, MissStartEvent* mse);
        void simulateMissWriteback(MissWritebackEvent* ev, uint64_t cycle, MissStartEvent* mse);
        void simulateReplAccess(ReplAccessEvent* ev, uint64_t cycle);

    private:
        uint64_t highPrioAccess(uint64_t cycle);
        uint64_t tryLowPrioAccess(uint64_t cycle);
};

#endif  // TIMING_CACHE_H_
