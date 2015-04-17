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

#include "tracing_cache.h"
#include "zsim.h"

TracingCache::TracingCache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, g_string& _tracefile, g_string& _name) :
    Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name), tracefile(_tracefile)
{
    futex_init(&traceLock);
}

void TracingCache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    Cache::setChildren(children, network);
    //We need to initialize the trace writer here because it needs the number of children
    atw = new AccessTraceWriter(tracefile, children.size());
    zinfo->traceWriters->push_back(atw); //register it so that it gets flushed when the simulation ends
}

uint64_t TracingCache::access(MemReq& req) {
    uint64_t respCycle = Cache::access(req);
    futex_lock(&traceLock);
    uint32_t lat = respCycle - req.cycle;
    AccessRecord acc = {req.lineAddr, req.cycle, lat, req.childId, req.type};
    atw->write(acc);
    futex_unlock(&traceLock);
    return respCycle;
}

