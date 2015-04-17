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

#ifndef PROFILE_STATS_H_
#define PROFILE_STATS_H_

/* Stats used to profile the simulator */

#include <time.h>
#include "stats.h"

//Helper function
inline uint64_t getNs() {
    struct timespec ts;
    //guaranteed synchronized across processors, averages 20ns/call on Ubuntu 12.04... Linux hrtimers have gotten really good! In comparison, rdtsc is 9ns.
    clock_gettime(CLOCK_REALTIME, &ts);
    return 1000000000L*ts.tv_sec + ts.tv_nsec;
}

/* Implements a single stopwatch-style cumulative clock. Useful to profile isolated events.
 * get() accounts for current interval if clock is running.
 */
class ClockStat : public ScalarStat {
    private:
        uint64_t startNs;
        uint64_t totalNs;

    public:
        ClockStat() : ScalarStat(), startNs(0), totalNs(0) {}

        void start() {
            assert(!startNs);
            startNs = getNs();
        }

        void end() {
            assert(startNs);
            uint64_t endNs = getNs();
            assert(endNs >= startNs)
            totalNs += (endNs - startNs);
            startNs = 0;
        }

        uint64_t get() const {
            return totalNs + (startNs? (getNs() - startNs) : 0);
        }
};

/* Implements multi-state time profiling. Always starts at state 0.
 * Using this with an enum will help retain your sanity. Does not stop,
 * so just transition to a dummy state if you want to stop profiling.
 * count() accounts for partial time in current state; count() is used
 * because we extend VectorCounter (TODO: we should have a VectorStat)
 */
class TimeBreakdownStat : public VectorCounter {
    private:
        uint32_t curState;
        uint64_t startNs;

    public:
        TimeBreakdownStat() : VectorCounter() {}

        virtual void init(const char* name, const char* desc, uint32_t size) {
            VectorCounter::init(name, desc, size);
            curState = 0;
            startNs = getNs();
        }

        //I need to define this even though it is completely unnecessary, but only if I override init. gcc bug or C++ oddity?
        virtual void init(const char* name, const char* desc, uint32_t size, const char** names) {
            VectorCounter::init(name, desc, size, names); //will call our init(name, desc, size)
        }

        void transition(uint32_t newState) {
            assert(curState < size());
            assert(newState < size());

            uint64_t curNs = getNs();
            assert(curNs >= startNs);

            inc(curState, curNs - startNs);
            //info("%d: %ld / %ld", curState, curNs - startNs, VectorCounter::count(curState));
            curState = newState;
            startNs = curNs;
        }

        inline virtual uint64_t count(uint32_t idx) const {
            uint64_t partial = VectorCounter::count(idx);
            return partial + ((idx == curState)? (getNs() - startNs) : 0);
        }
};

#endif  // PROFILE_STATS_H_
