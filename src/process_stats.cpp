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

#include "process_stats.h"
#include "process_tree.h"
#include "scheduler.h"
#include "zsim.h"

ProcessStats::ProcessStats(AggregateStat* parentStat) {
    uint32_t maxProcs = zinfo->lineSize;
    processCycles.resize(maxProcs, 0);
    processInstrs.resize(maxProcs, 0);
    lastCoreCycles.resize(zinfo->numCores, 0);
    lastCoreInstrs.resize(zinfo->numCores, 0);
    lastUpdatePhase = 0;

    auto procCyclesLambda = [this](uint32_t p) { return getProcessCycles(p); };
    auto procCyclesStat = makeLambdaVectorStat(procCyclesLambda, maxProcs);
    procCyclesStat->init("procCycles", "Per-process unhalted core cycles");

    auto procInstrsLambda = [this](uint32_t p) { return getProcessInstrs(p); };
    auto procInstrsStat = makeLambdaVectorStat(procInstrsLambda, maxProcs);
    procInstrsStat->init("procInstrs", "Per-process instructions");

    parentStat->append(procCyclesStat);
    parentStat->append(procInstrsStat);
}

uint64_t ProcessStats::getProcessCycles(uint32_t p) {
    if (unlikely(lastUpdatePhase != zinfo->numPhases)) update();
    assert(p < processCycles.size());
    return processCycles[p];
}

uint64_t ProcessStats::getProcessInstrs(uint32_t p) {
    if (unlikely(lastUpdatePhase != zinfo->numPhases)) update();
    assert(p < processInstrs.size());
    return processInstrs[p];
}

void ProcessStats::notifyDeschedule(uint32_t cid, uint32_t outgoingPid) {
    assert(cid < lastCoreCycles.size());
    assert(outgoingPid < processCycles.size());
    updateCore(cid, outgoingPid);
}

/* Private */

void ProcessStats::updateCore(uint32_t cid, uint32_t p) {
    p = zinfo->procArray[p]->getGroupIdx();

    uint64_t cCycles = zinfo->cores[cid]->getCycles();
    uint64_t cInstrs = zinfo->cores[cid]->getInstrs();

    assert(cCycles >= lastCoreCycles[cid] && cInstrs >= lastCoreInstrs[cid]);
    processCycles[p]  += cCycles - lastCoreCycles[cid];
    processInstrs[p]  += cInstrs - lastCoreInstrs[cid];

    lastCoreCycles[cid] = cCycles;
    lastCoreInstrs[cid] = cInstrs;
}

void ProcessStats::update() {
    assert(lastUpdatePhase < zinfo->numPhases);
    for (uint32_t cid = 0; cid < lastCoreCycles.size(); cid++) {
        uint32_t p = zinfo->sched->getScheduledPid(cid);
        if (p == (uint32_t)-1) continue;
        assert(p < processCycles.size());
        updateCore(cid, p);
    }
    lastUpdatePhase = zinfo->numPhases;
}

