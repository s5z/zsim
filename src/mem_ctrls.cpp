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

//#include "timing_event.h"
//#include "event_recorder.h"
#include "mem_ctrls.h"
#include "zsim.h"

uint64_t SimpleMemory::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            *req.state = M;
            break;

        default: panic("!?");
    }

    uint64_t respCycle = req.cycle + latency;
    assert(respCycle > req.cycle);
/*
    if ((req.type == GETS || req.type == GETX) && eventRecorders[req.srcId]) {
        Address addr = req.lineAddr<<lineBits;
        MemAccReqEvent* memEv = new (eventRecorders[req.srcId]->alloc<MemAccReqEvent>()) MemAccReqEvent(nullptr, false, addr);
        TimingRecord tr = {addr, req.cycle, respCycle, req.type, memEv, memEv};
        eventRecorders[req.srcId]->pushRecord(tr);
    }
*/
    return respCycle;
}




MD1Memory::MD1Memory(uint32_t requestSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, g_string& _name)
    : zeroLoadLatency(_zeroLoadLatency), name(_name)
{
    lastPhase = 0;

    double bytesPerCycle = ((double)megabytesPerSecond)/((double)megacyclesPerSecond);
    maxRequestsPerCycle = bytesPerCycle/requestSize;
    assert(maxRequestsPerCycle > 0.0);

    zeroLoadLatency = _zeroLoadLatency;

    smoothedPhaseAccesses = 0.0;
    curPhaseAccesses = 0;
    curLatency = zeroLoadLatency;

    futex_init(&updateLock);
}

void MD1Memory::updateLatency() {
    uint32_t phaseCycles = (zinfo->numPhases - lastPhase)*(zinfo->phaseLength);
    if (phaseCycles < 10000) return; //Skip with short phases

    smoothedPhaseAccesses =  (curPhaseAccesses*0.5) + (smoothedPhaseAccesses*0.5);
    double requestsPerCycle = smoothedPhaseAccesses/((double)phaseCycles);
    double load = requestsPerCycle/maxRequestsPerCycle;

    //Clamp load
    if (load > 0.95) {
        //warn("MC: Load exceeds limit, %f, clamping, curPhaseAccesses %d, smoothed %f, phase %ld", load, curPhaseAccesses, smoothedPhaseAccesses, zinfo->numPhases);
        load = 0.95;
        profClampedLoads.inc();
    }

    double latMultiplier = 1.0 + 0.5*load/(1.0 - load); //See Pollancek-Khinchine formula
    curLatency = (uint32_t)(latMultiplier*zeroLoadLatency);

    //info("%s: Load %.2f, latency multiplier %.2f, latency %d", name.c_str(), load, latMultiplier, curLatency);
    uint32_t intLoad = (uint32_t)(load*100.0);
    profLoad.inc(intLoad);
    profUpdates.inc();

    curPhaseAccesses = 0;
    __sync_synchronize();
    lastPhase = zinfo->numPhases;
}

uint64_t MD1Memory::access(MemReq& req) {
    if (zinfo->numPhases > lastPhase) {
        futex_lock(&updateLock);
        //Recheck, someone may have updated already
        if (zinfo->numPhases > lastPhase) {
            updateLatency();
        }
        futex_unlock(&updateLock);
    }

    switch (req.type) {
        case PUTX:
            //Dirty wback
            profWrites.atomicInc();
            profTotalWrLat.atomicInc(curLatency);
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            //Note no break
        case PUTS:
            //Not a real access -- memory must treat clean wbacks as if they never happened.
            *req.state = I;
            break;
        case GETS:
            profReads.atomicInc();
            profTotalRdLat.atomicInc(curLatency);
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            profReads.atomicInc();
            profTotalRdLat.atomicInc(curLatency);
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            *req.state = M;
            break;

        default: panic("!?");
    }
    return req.cycle + ((req.type == PUTS)? 0 /*PUTS is not a real access*/ : curLatency);
}

