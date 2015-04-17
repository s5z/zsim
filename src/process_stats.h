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

#ifndef PROCESS_STATS_H_
#define PROCESS_STATS_H_

#include "galloc.h"
#include "stats.h"

/* Maintains, queries, and transparently updates per-process instruction and cycle counts.
 * You'd think it'd make sense to include this in ProcTreeNode, but those are dynamic, and
 * stats must be static (and zeros compress great)
 */
class ProcessStats : public GlobAlloc {
    private:
        g_vector<uint64_t> processCycles, processInstrs;
        g_vector<uint64_t> lastCoreCycles, lastCoreInstrs;
        uint64_t lastUpdatePhase;

    public:
        explicit ProcessStats(AggregateStat* parentStat); //includes initStats, called post-system init

        // May trigger a global update, should call ONLY when quiesced
        uint64_t getProcessCycles(uint32_t p);
        uint64_t getProcessInstrs(uint32_t p);

        // Must be called by scheduler when descheduling; core must be quiesced
        void notifyDeschedule(uint32_t cid, uint32_t outgoingPid);

    private:
        void updateCore(uint32_t cid, uint32_t p);
        void update(); //transparent
};

#endif  // PROCESS_STATS_H_
