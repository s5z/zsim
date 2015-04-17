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

#ifndef WEAVE_MD1_MEM_H_
#define WEAVE_MD1_MEM_H_

#include "mem_ctrls.h"
#include "timing_event.h"
#include "zsim.h"

/* Implements a weave-phase memory controller based on the MD1 controller, returning the same
 * latencies MD1 would return in the weave phase, but
 */

//Weave-phase event
class WeaveMemAccEvent : public TimingEvent {
    private:
        uint32_t lat;

    public:
        WeaveMemAccEvent(uint32_t _lat, int32_t domain, uint32_t preDelay, uint32_t postDelay) :  TimingEvent(preDelay, postDelay, domain), lat(_lat) {}

        void simulate(uint64_t startCycle) {
            done(startCycle + lat);
        }
};

// Actual controller
class WeaveMD1Memory : public MD1Memory {
    private:
        const uint32_t zeroLoadLatency;
        const uint32_t boundLatency;
        const uint32_t domain;
        uint32_t preDelay, postDelay;

    public:
        WeaveMD1Memory(uint32_t lineSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, uint32_t _boundLatency, uint32_t _domain, g_string& _name) :
            MD1Memory(lineSize, megacyclesPerSecond, megabytesPerSecond, _zeroLoadLatency, _name), zeroLoadLatency(_zeroLoadLatency), boundLatency(_boundLatency), domain(_domain)
        {
            preDelay = zeroLoadLatency/2;
            postDelay = zeroLoadLatency - preDelay;
        }

        uint64_t access(MemReq& req) {
            uint64_t realRespCycle = MD1Memory::access(req);
            uint32_t realLatency = realRespCycle - req.cycle;

            uint64_t respCycle = req.cycle + ((req.type == PUTS)? 0 : boundLatency);
            assert(realRespCycle >= respCycle);
            assert(req.type == PUTS || realLatency >= zeroLoadLatency);

            if ((req.type != PUTS) && zinfo->eventRecorders[req.srcId]) {
                WeaveMemAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) WeaveMemAccEvent(realLatency-zeroLoadLatency, domain, preDelay, postDelay);
                memEv->setMinStartCycle(req.cycle);
                TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
                zinfo->eventRecorders[req.srcId]->pushRecord(tr);
            }

            // info("Access to %lx at %ld, %d lat, returning %d", req.lineAddr, req.cycle, realLatency, zeroLoadLatency);
            return respCycle;
        }
};

// OK, even simpler...
class WeaveSimpleMemory : public SimpleMemory {
    private:
        uint32_t zeroLoadLatency;
        uint32_t domain;
        uint32_t preDelay, postDelay;

    public:
        WeaveSimpleMemory(uint32_t _latency, uint32_t _zeroLoadLatency, uint32_t _domain, g_string& _name) :
            SimpleMemory(_latency, _name), zeroLoadLatency(_zeroLoadLatency), domain(_domain)
        {
            assert(_latency >= _zeroLoadLatency);
            preDelay = zeroLoadLatency/2;
            postDelay = zeroLoadLatency - preDelay;
        }

        uint64_t access(MemReq& req) {
            uint64_t realRespCycle = SimpleMemory::access(req);
            uint32_t realLatency = realRespCycle - req.cycle;

            uint64_t respCycle = req.cycle + ((req.type == PUTS)? 0 : zeroLoadLatency);
            assert(realRespCycle >= respCycle);
            assert(req.type == PUTS || realLatency >= zeroLoadLatency);

            if ((req.type != PUTS) && zinfo->eventRecorders[req.srcId]) {
                WeaveMemAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) WeaveMemAccEvent(realLatency-zeroLoadLatency, domain, preDelay, postDelay);
                memEv->setMinStartCycle(req.cycle);
                TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
                zinfo->eventRecorders[req.srcId]->pushRecord(tr);
            }

            // info("Access to %lx at %ld, %d lat, returning %d", req.lineAddr, req.cycle, realLatency, zeroLoadLatency);
            return respCycle;
        }
};

#endif  // WEAVE_MD1_MEM_H_
