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

#ifndef __TRACE_DRIVER_H__
#define __TRACE_DRIVER_H__

#include <unordered_map>
#include <vector>
#include "access_tracing.h"
#include "g_std/g_string.h"
#include "stats.h"

/* Basic class for trace-driven simulation. Shares the cache interface (invalidate), but it is not a cache in any sense --- it just reads in a single trace and replays it */

class TraceDriverProxyCache;

class TraceDriver {
    private:
        struct ChildInfo {
            std::unordered_map<Address, MESIState> cStore; //holds current sets of lines for each child. Needs to support an arbitrary set, hence the hash table
            int64_t skew;
            uint64_t lastReqCycle;
            //Counter bypassedGETS;
            //Counter bypassedGETX;
            Counter profLat;
            Counter profSelfInv; //invalidations in response to our own access
            Counter profCrossInv; //invalidations in response to another access
            Counter profInvx;
        };

        ChildInfo* children;
        lock_t lock; //NOTE: not needed for now
        AccessTraceReader tr;
        uint32_t numChildren;
        bool useSkews; //If false, replays the trace using its request cycles. If true, it skews the simulated child. Can only be true with a single child.
        bool playPuts; //If true, issues PUTS/PUTX requests as they appear in the trace. If false, it just issues the GETS/X requests, leaving it up to the parent to decide when to evict something (NOTE: if the parent is running OPT, it knows better!)
        bool playAllGets; //If true, if we have a get to an address that we already have, issue a put immediately before.
        MemObject* parent;

        AccessTraceWriter* atw;

        //Last access, childId == -1 if invalid, acts as 1-elem buffer
        AccessRecord lastAcc;

    public:
        TraceDriver(std::string filename, std::string retracefile, std::vector<TraceDriverProxyCache*>& proxies, bool _useSkews, bool _playPuts, bool _playAllGets);
        void initStats(AggregateStat* parentStat);
        void setParent(MemObject* _parent);

        uint64_t invalidate(uint32_t childId, Address lineAddr, InvType type, bool* reqWriteback, uint64_t reqCycle, uint32_t srcId);

        //Returns false if done, true otherwise
        bool executePhase();

    private:
        inline void executeAccess(AccessRecord acc);
};


class TraceDriverProxyCache : public BaseCache {
    private:
        TraceDriver* drv;
        uint32_t id;
        g_string name;
        MemObject* parent;
    public:
        TraceDriverProxyCache(g_string& _name) : drv(NULL), id(-1), name(_name) {}
        const char* getName() {return name.c_str();}

        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {id = _childId; assert(parents.size() == 1); parent = parents[0];}; //FIXME: Support multi-banked caches...
        void setChildren(const g_vector<BaseCache*>& children, Network* network) {panic("Should not be called, this must be terminal");};
        
        MemObject* getParent() const {return parent;}
        void setDriver(TraceDriver* driver) {drv = driver;}

        uint64_t access(MemReq& req) {panic("Should never be called");}
        uint64_t invalidate(Address lineAddr, InvType type, bool* reqWriteback, uint64_t reqCycle, uint32_t srcId) {
            return drv->invalidate(id, lineAddr, type, reqWriteback, reqCycle, srcId);
        }
};

#endif /*__TRACE_DRIVER_H__*/
