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

#ifndef BREAKDOWN_STATS_H_
#define BREAKDOWN_STATS_H_

#include "bithacks.h"
#include "stats.h"
#include "zsim.h"

/* Implements per-cycle breakdowns. Always starts at state 0.
 * count() accounts for cycles in current state; count() is used
 * because we extend VectorCounter (TODO: Move to VectorStat).
 */
class CycleBreakdownStat : public VectorCounter {
    private:
        uint32_t curState;
        uint64_t lastCycle;

    public:
        CycleBreakdownStat() : VectorCounter() {}

        virtual void init(const char* name, const char* desc, uint32_t size) {
            VectorCounter::init(name, desc, size);
            curState = 0;
            lastCycle = 0;
        }

        // I need to define this even though it is completely unnecessary, but only if I override init. gcc bug or C++ oddity?
        virtual void init(const char* name, const char* desc, uint32_t size, const char** names) {
            VectorCounter::init(name, desc, size, names); // will call our init(name, desc, size)
        }

        void transition(uint32_t newState, uint64_t cycle) {
            assert(curState < size());
            assert(newState < size());
            assert(lastCycle <= cycle);
            inc(curState, cycle - lastCycle);
            curState = newState;
            lastCycle = cycle;
        }

        // Accounts for time in current state, even if the last transition happened long ago
        inline virtual uint64_t count(uint32_t idx) const {
            uint64_t partial = VectorCounter::count(idx);
            uint64_t curCycle = MAX(lastCycle, zinfo->globPhaseCycles);
            return partial + ((idx == curState)? (curCycle - lastCycle) : 0);
        }
};

#endif  // BREAKDOWN_STATS_H_
