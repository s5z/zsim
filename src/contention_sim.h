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

#ifndef CONTENTION_SIM_H_
#define CONTENTION_SIM_H_

#include <functional>
#include <stdint.h>
#include <vector>
#include "bithacks.h"
#include "event_recorder.h"
#include "g_std/g_vector.h"
#include "galloc.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "prio_queue.h"
#include "profile_stats.h"
#include "stats.h"

//Set to 1 to produce stats of how many event crossings are generated and run. Useful for debugging, but adds overhead.
#define PROFILE_CROSSINGS 0
//#define PROFILE_CROSSINGS 1

class TimingEvent;
class DelayEvent;
class CrossingEvent;

#define PQ_BLOCKS 1024

class ContentionSim : public GlobAlloc {
    private:
        struct CompareEvents : public std::binary_function<TimingEvent*, TimingEvent*, bool> {
            bool operator()(TimingEvent* lhs, TimingEvent* rhs) const;
        };

        struct CrossingEventInfo {
            uint64_t cycle;
            CrossingEvent* ev; //only valid if the source's curCycle < cycle (otherwise this may be already executed or recycled)
        };

        CrossingEventInfo* lastCrossing; //indexed by [srcId*doms*doms + srcDom*doms + dstDom]

        struct DomainData : public GlobAlloc {
            PrioQueue<TimingEvent, PQ_BLOCKS> pq;

            PAD();

            volatile uint64_t curCycle;
            lock_t pqLock; //used on phase 1 enqueues
            //lock_t domainLock; //used by simulation thread

            uint32_t prio;
            uint64_t queuePrio;

            PAD();

            ClockStat profTime;

#if PROFILE_CROSSINGS
            VectorCounter profIncomingCrossingSims;
            VectorCounter profIncomingCrossings;
            VectorCounter profIncomingCrossingHist;
#endif
        };

        struct CompareDomains : public std::binary_function<DomainData*, DomainData*, bool> {
             bool operator()(DomainData* d1, DomainData* d2) const;
        };

        struct SimThreadData {
            lock_t wakeLock; //used to sleep/wake up simulation thread
            uint32_t firstDomain;
            uint32_t supDomain; //supreme, ie first not included

            std::vector<std::pair<uint64_t, TimingEvent*> > logVec;
        };

        //RO
        DomainData* domains;
        SimThreadData* simThreads;

        PAD();

        uint32_t numDomains;
        uint32_t numSimThreads;
        bool skipContention;

        PAD();

        //RW
        lock_t waitLock;
        volatile uint64_t limit;
        volatile uint64_t lastLimit;
        volatile bool terminate;

        volatile uint32_t threadsDone;
        volatile uint32_t threadTicket; //used only at init

        volatile bool inCSim; //true when inside contention simulation

        PAD();

        //lock_t testLock;
        lock_t postMortemLock;

    public:
        ContentionSim(uint32_t _numDomains, uint32_t _numSimThreads);

        void initStats(AggregateStat* parentStat);

        void postInit(); //must be called after the simulator is initialized

        void enqueue(TimingEvent* ev, uint64_t cycle);
        void enqueueSynced(TimingEvent* ev, uint64_t cycle);
        void enqueueCrossing(CrossingEvent* ev, uint64_t cycle, uint32_t srcId, uint32_t srcDomain, uint32_t dstDomain, EventRecorder* evRec);

        void simulatePhase(uint64_t limit);

        void finish();

        uint64_t getLastLimit() {return lastLimit;}

        uint64_t getCurCycle(uint32_t domain) {
            assert(domain < numDomains);
            uint64_t c = domains[domain].curCycle;
            assert(((int64_t)c) >= 0);
            return c;
        }

        void setPrio(uint32_t domain, uint32_t prio) {domains[domain].prio = prio;}

#if PROFILE_CROSSINGS
        void profileCrossing(uint32_t srcDomain, uint32_t dstDomain, uint32_t count) {
            domains[dstDomain].profIncomingCrossings.inc(srcDomain);
            domains[dstDomain].profIncomingCrossingSims.inc(srcDomain, count);
            domains[dstDomain].profIncomingCrossingHist.inc(MIN(count, (unsigned)32));
        }
#endif

    private:
        void simThreadLoop(uint32_t thid);
        void simulatePhaseThread(uint32_t thid);

        static void SimThreadTrampoline(void* arg);
};

#endif  // CONTENTION_SIM_H_
