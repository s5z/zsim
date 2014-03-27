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

#ifndef PROC_STATS_H_
#define PROC_STATS_H_

#include "galloc.h"
#include "stats.h"

class ProcStats : public GlobAlloc {
    private:

        class ProcessCounter;
        class ProcessVectorCounter;

        uint64_t lastUpdatePhase;

        AggregateStat* coreStats;  // each member must be a regular aggregate with numCores elems
        AggregateStat* procStats;  // stats produced

        uint64_t* buf;
        uint64_t* lastBuf;
        uint64_t bufSize;

    public:
        explicit ProcStats(AggregateStat* parentStat, AggregateStat* _coreStats); //includes initStats, called post-system init

        // Must be called by scheduler when descheduling; core must be quiesced
        void notifyDeschedule();

    private:
        Stat* replStat(Stat* s, const char* name = NULL, const char* desc = NULL);

        void update();  // transparent
};

#endif  // PROCESS_STATS_H_
