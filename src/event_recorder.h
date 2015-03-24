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

#ifndef EVENT_RECORDER_H_
#define EVENT_RECORDER_H_

#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "slab_alloc.h"

class TimingEvent;

// Encodes an event that the core should capture for the contention simulation
struct TimingRecord {
    Address addr;
    uint64_t reqCycle;
    uint64_t respCycle;
    AccessType type;
    TimingEvent* startEvent;
    TimingEvent* endEvent;

    bool isValid() const { return startEvent; }
    void clear() { startEvent = nullptr; }
};

//class CoreRecorder;
class CrossingEvent;
typedef g_vector<CrossingEvent*> CrossingStack;

class EventRecorder : public GlobAlloc {
    private:
        slab::SlabAlloc slabAlloc;
        TimingRecord tr;
        CrossingStack crossingStack;
        uint32_t srcId;

        volatile uint64_t lastGapCycles;
        PAD();
        volatile uint64_t lastStartSlack;
        PAD();

    public:
        EventRecorder() {
            tr.clear();
        }

        //Alloc interface

        template <typename T>
        T* alloc() {
            return slabAlloc.alloc<T>();
        }

        void* alloc(size_t sz) {
            return slabAlloc.alloc(sz);
        }

        void advance(uint64_t prodCycle, uint64_t usedCycle) {
            slabAlloc.advance(prodCycle, usedCycle);
        }

        //Event recording interface

        void pushRecord(const TimingRecord& rec) {
            assert(!tr.isValid());
            tr = rec;
            assert(tr.isValid());
        }

        // Inline to avoid extra copy
        inline TimingRecord popRecord() __attribute__((always_inline)) {
            TimingRecord rec = tr;
            tr.clear();
            return rec;
        }

        inline size_t hasRecord() const {
            return tr.isValid();
        }

        //Called by crossing events
        inline uint64_t getSlack(uint64_t origStartCycle) const {
            return origStartCycle + lastStartSlack;
        }

        inline uint64_t getGapCycles() const {
            return lastGapCycles;
        }

        //Called by the core's recorder
        //infrequently
        void setGapCycles(uint64_t gapCycles) {
            lastGapCycles = gapCycles;
        }

        //frequently
        inline void setStartSlack(uint64_t startSlack) {
            //Avoid a write, it can cost a bunch of coherence misses
            if (lastStartSlack != startSlack) lastStartSlack = startSlack;
        }

        uint32_t getSourceId() const {return srcId;}
        void setSourceId(uint32_t i) {srcId = i;}

        inline CrossingStack& getCrossingStack() {
            return crossingStack;
        }
};

#endif  // EVENT_RECORDER_H_
