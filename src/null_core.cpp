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

#include "null_core.h"
#include "zsim.h"

NullCore::NullCore(g_string& _name) : Core(_name), instrs(0), curCycle(0), phaseEndCycle(0) {}

void NullCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    ProxyStat* cyclesStat = new ProxyStat();
    cyclesStat->init("cycles", "Simulated cycles", &instrs); //simulated instrs == simulated cycles; curCycle can be skewed forward
    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    coreStat->append(cyclesStat);
    coreStat->append(instrsStat);
    parentStat->append(coreStat);
}

uint64_t NullCore::getPhaseCycles() const {
    return curCycle - zinfo->globPhaseCycles;
}

void NullCore::bbl(BblInfo* bblInfo) {
    instrs += bblInfo->instrs;
    curCycle += bblInfo->instrs;
}

void NullCore::contextSwitch(int32_t gid) {}

void NullCore::join() {
    curCycle = MAX(curCycle, zinfo->globPhaseCycles);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
}

//Static class functions: Function pointers and trampolines

InstrFuncPtrs NullCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};
}

void NullCore::LoadFunc(THREADID tid, ADDRINT addr) {}
void NullCore::StoreFunc(THREADID tid, ADDRINT addr) {}
void NullCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {}
void NullCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {}

void NullCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    NullCore* core = static_cast<NullCore*>(cores[tid]);
    core->bbl(bblInfo);

    while (unlikely(core->curCycle > core->phaseEndCycle)) {
        assert(core->phaseEndCycle == zinfo->globPhaseCycles + zinfo->phaseLength);
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        //NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        //the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. If newCid == cid,
        //we're not at risk of racing, even if we were switched out and then switched in.
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
    }
}

