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

#include <sstream>
#include "trace_driver.h"
#include "zsim.h"

TraceDriver::TraceDriver(std::string filename, std::string retraceFilename, std::vector<TraceDriverProxyCache*>& proxies, bool _useSkews, bool _playPuts, bool _playAllGets)
    : tr(filename), numChildren(proxies.size()), useSkews(_useSkews), playPuts(_playPuts), playAllGets(_playAllGets)
{
    assert(numChildren > 0);
    assert(!useSkews || numChildren == 1);
    if (tr.getNumChildren() != numChildren) panic("Number of proxy caches (%d) does not match with streams in the trace file (%d)", numChildren, tr.getNumChildren());
    children = new ChildInfo[numChildren];
    futex_init(&lock);
    lastAcc.childId = -1;
    parent = proxies[0]->getParent();
    for (uint32_t i = 0; i < numChildren; i++) proxies[i]->setDriver(this);

    if (retraceFilename != "") { //we're doing retracing with the new skews
        g_string fname(retraceFilename.c_str());
        atw = new AccessTraceWriter(fname, numChildren);
        zinfo->traceWriters->push_back(atw);
    } else {
        atw = nullptr;
    }
}

void TraceDriver::initStats(AggregateStat* parentStat) {
    AggregateStat* drvStat = new AggregateStat(false); //don't make it a regular aggregate... it gets compacted in periodic stats and becomes useless!
    drvStat->init("driver", "Trace driver stats");
    for (uint32_t c = 0; c < numChildren; c++) {
        std::stringstream pss;
        pss << "child-" << c;
        AggregateStat* cStat = new AggregateStat();
        cStat->init(gm_strdup(pss.str().c_str()), "Child stats");
        ProxyStat* cycleStat = new ProxyStat();
        cycleStat->init("cycles", "Cycles", &children[c].lastReqCycle);  cStat->append(cycleStat);
        children[c].profLat.init("latGET", "GET request latency"); cStat->append(&children[c].profLat);
        ProxyStat* skewStat = new ProxyStat();
        skewStat->init("skew", "Latency skew", (uint64_t*)&children[c].skew);  cStat->append(skewStat);

        children[c].profSelfInv.init("selfINV", "Self-invalidations"); cStat->append(&children[c].profSelfInv);
        children[c].profCrossInv.init("crossINV", "Cross-invalidations"); cStat->append(&children[c].profCrossInv);
        children[c].profInvx.init("INVX", "Downgrades"); cStat->append(&children[c].profInvx);
        drvStat->append(cStat);
    }
    parentStat->append(drvStat);
}

void TraceDriver::setParent(MemObject* _parent) {
    parent = _parent;
}

uint64_t TraceDriver::invalidate(uint32_t childId, Address lineAddr, InvType type, bool* reqWriteback, uint64_t reqCycle, uint32_t srcId) {
    assert(childId < numChildren);
    std::unordered_map<Address, MESIState>& cStore = children[childId].cStore;
    std::unordered_map<Address, MESIState>::iterator it = cStore.find(lineAddr);
    assert((it != cStore.end()));
    *reqWriteback = (it->second == M);
    if (type == INVX) {
        it->second = S;
        children[childId].profInvx.inc();
    } else {
        cStore.erase(it);
        if (srcId == childId) {
            children[childId].profSelfInv.inc();
        } else {
            children[childId].profCrossInv.inc();
        }
    }
    return 0;
}

//Returns false if done, true otherwise
bool TraceDriver::executePhase() {
    uint64_t limit = zinfo->globPhaseCycles + zinfo->phaseLength;

    //Load valid access
    AccessRecord acc;
    if (lastAcc.childId == (uint32_t)-1) {
        if (tr.empty()) return false;
        acc = tr.read();
        if (useSkews) acc.reqCycle += children[acc.childId].skew;
    } else {
        acc = lastAcc;
        lastAcc.childId = (uint32_t)-1;
    }

    //Run until we reach the cycle limit or run out of phases
    while (acc.reqCycle < limit) {
        executeAccess(acc);
        if (tr.empty()) return false;
        acc = tr.read();
        if (useSkews) acc.reqCycle += children[acc.childId].skew;
    }

    lastAcc = acc; //save this access for the next phase
    return true;
}

void TraceDriver::executeAccess(AccessRecord acc) {
    assert(acc.childId < numChildren);
    std::unordered_map<Address, MESIState>& cStore = children[acc.childId].cStore;

    int64_t lat = 0;
    switch (acc.type) {
        case PUTS:
        case PUTX:
            {
                if (!playPuts) return;
                std::unordered_map<Address, MESIState>::iterator it = cStore.find(acc.lineAddr);
                if (it == cStore.end()) return; //we don't currently have this line, skip
                MemReq req = {acc.lineAddr, acc.type, acc.childId, &it->second, acc.reqCycle, nullptr, it->second, acc.childId};
                lat = parent->access(req) - acc.reqCycle; //note that PUT latency does not affect driver latency
                assert(it->second == I);
                cStore.erase(it);
            }
            break;
        case GETS:
        case GETX:
            {
                std::unordered_map<Address, MESIState>::iterator it = cStore.find(acc.lineAddr);
                MESIState state = I;
                if (it != cStore.end()) {
                    if (!((it->second == S) && (acc.type == GETX))) { //we have the line, and it's not an upgrade miss, we can't replay this access directly
                        if (playAllGets) { //issue a PUT
                            MemReq req = {acc.lineAddr, (it->second == M)? PUTX : PUTS, acc.childId, &it->second, acc.reqCycle, nullptr, it->second, acc.childId};
                            parent->access(req);
                            assert(it->second == I);
                        } else {
                            return; //skip
                        }
                    } else {
                        state = it->second;
                    }
                }
                MemReq req = {acc.lineAddr, acc.type, acc.childId, &state, acc.reqCycle, nullptr, state, acc.childId};
                uint64_t respCycle = parent->access(req);
                lat = respCycle - acc.reqCycle;
                children[acc.childId].profLat.inc(lat);
                children[acc.childId].skew += ((int64_t)lat - acc.latency);
                assert(state != I);
                cStore[acc.lineAddr] = state;
            }
            break;
        default:
            panic("Unknown access type %d, trace is probably corrupted", acc.type);
    }

    children[acc.childId].lastReqCycle = acc.reqCycle;
    if (atw) {
        AccessRecord wAcc = acc;
        // We always want the outout trace to be skewed regardless... otherwise it does not make sense to produce an output trace
        if (!useSkews) wAcc.reqCycle += children[acc.childId].skew;
        wAcc.latency = lat;
        atw->write(wAcc);
    }
}

