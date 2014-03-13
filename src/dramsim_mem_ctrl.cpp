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

#include "dramsim_mem_ctrl.h"
#include <map>
#include <string>
#include "event_recorder.h"
#include "tick_event.h"
#include "timing_event.h"
#include "zsim.h"

#ifdef _WITH_DRAMSIM_ //was compiled with dramsim
#include "DRAMSim.h"

using namespace DRAMSim; // NOLINT(build/namespaces)

class DRAMSimAccEvent : public TimingEvent {
    private:
        DRAMSimMemory* dram;
        bool write;
        Address addr;

    public:
        uint64_t sCycle;

        DRAMSimAccEvent(DRAMSimMemory* _dram, bool _write, Address _addr, int32_t domain) :  TimingEvent(0, 0, domain), dram(_dram), write(_write), addr(_addr) {}

        bool isWrite() const {
            return write;
        }

        Address getAddr() const {
            return addr;
        }

        void simulate(uint64_t startCycle) {
            sCycle = startCycle;
            dram->enqueue(this, startCycle);
        }
};


DRAMSimMemory::DRAMSimMemory(string& dramTechIni, string& dramSystemIni, string& outputDir, string& traceName,
        uint32_t capacityMB, uint64_t cpuFreqHz, uint32_t _minLatency, uint32_t _domain, const g_string& _name)
{
    curCycle = 0;
    minLatency = _minLatency;
    // NOTE: this will alloc DRAM on the heap and not the glob_heap, make sure only one process ever handles this
    dramCore = getMemorySystemInstance(dramTechIni, dramSystemIni, outputDir, traceName, capacityMB);
    dramCore->setCPUClockSpeed(cpuFreqHz);

    TransactionCompleteCB *read_cb = new Callback<DRAMSimMemory, void, unsigned, uint64_t, uint64_t>(this, &DRAMSimMemory::DRAM_read_return_cb);
    TransactionCompleteCB *write_cb = new Callback<DRAMSimMemory, void, unsigned, uint64_t, uint64_t>(this, &DRAMSimMemory::DRAM_write_return_cb);
    dramCore->RegisterCallbacks(read_cb, write_cb, NULL);

    domain = _domain;
    TickEvent<DRAMSimMemory>* tickEv = new TickEvent<DRAMSimMemory>(this, domain);
    tickEv->queue(0);  // start the sim at time 0

    name = _name;
}

void DRAMSimMemory::initStats(AggregateStat* parentStat) {
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Memory controller stats");
    profReads.init("rd", "Read requests"); memStats->append(&profReads);
    profWrites.init("wr", "Write requests"); memStats->append(&profWrites);
    profTotalRdLat.init("rdlat", "Total latency experienced by read requests"); memStats->append(&profTotalRdLat);
    profTotalWrLat.init("wrlat", "Total latency experienced by write requests"); memStats->append(&profTotalWrLat);
    parentStat->append(memStats);
}

uint64_t DRAMSimMemory::access(MemReq& req) {
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

    uint64_t respCycle = req.cycle + minLatency;
    assert(respCycle > req.cycle);

    if ((req.type != PUTS /*discard clean writebacks*/) && zinfo->eventRecorders[req.srcId]) {
        Address addr = req.lineAddr << lineBits;
        bool isWrite = (req.type == PUTX);
        DRAMSimAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) DRAMSimAccEvent(this, isWrite, addr, domain);
        memEv->setMinStartCycle(req.cycle);
        TimingRecord tr = {addr, req.cycle, respCycle, req.type, memEv, memEv};
        zinfo->eventRecorders[req.srcId]->pushRecord(tr);
    }

    return respCycle;
}

uint32_t DRAMSimMemory::tick(uint64_t cycle) {
    dramCore->update();
    curCycle++;
    return 1;
}

void DRAMSimMemory::enqueue(DRAMSimAccEvent* ev, uint64_t cycle) {
    //info("[%s] %s access to %lx added at %ld, %ld inflight reqs", getName(), ev->isWrite()? "Write" : "Read", ev->getAddr(), cycle, inflightRequests.size());
    dramCore->addTransaction(ev->isWrite(), ev->getAddr());
    inflightRequests.insert(std::pair<Address, DRAMSimAccEvent*>(ev->getAddr(), ev));
    ev->hold();
}

void DRAMSimMemory::DRAM_read_return_cb(uint32_t id, uint64_t addr, uint64_t memCycle) {
    std::multimap<uint64_t, DRAMSimAccEvent*>::iterator it = inflightRequests.find(addr);
    assert(it != inflightRequests.end());
    DRAMSimAccEvent* ev = it->second;

    uint32_t lat = curCycle+1 - ev->sCycle;
    if (ev->isWrite()) {
        profWrites.inc();
        profTotalWrLat.inc(lat);
    } else {
        profReads.inc();
        profTotalRdLat.inc(lat);
    }

    ev->release();
    ev->done(curCycle+1);
    inflightRequests.erase(it);
    //info("[%s] %s access to %lx DONE at %ld (%ld cycles), %ld inflight reqs", getName(), it->second->isWrite()? "Write" : "Read", it->second->getAddr(), curCycle, curCycle-it->second->sCycle, inflightRequests.size());
}

void DRAMSimMemory::DRAM_write_return_cb(uint32_t id, uint64_t addr, uint64_t memCycle) {
    //Same as read for now
    DRAM_read_return_cb(id, addr, memCycle);
}

#else //no dramsim, have the class fail when constructed

using std::string;

DRAMSimMemory::DRAMSimMemory(string& dramTechIni, string& dramSystemIni, string& outputDir, string& traceName,
        uint32_t capacityMB, uint64_t cpuFreqHz, uint32_t _minLatency, uint32_t _domain, const g_string& _name)
{
    panic("Cannot use DRAMSimMemory, zsim was not compiled with DRAMSim");
}

uint64_t DRAMSimMemory::access(MemReq& req) { panic("???"); return 0; }
uint32_t DRAMSimMemory::tick(uint64_t cycle) { panic("???"); return 0; }
void DRAMSimMemory::enqueue(DRAMSimAccEvent* ev, uint64_t cycle) { panic("???"); }
void DRAMSimMemory::DRAM_read_return_cb(uint32_t id, uint64_t addr, uint64_t memCycle) { panic("???"); }
void DRAMSimMemory::DRAM_write_return_cb(uint32_t id, uint64_t addr, uint64_t memCycle) { panic("???"); }

#endif

