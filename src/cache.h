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

#ifndef CACHE_H_
#define CACHE_H_

#include "cache_arrays.h"
#include "coherence_ctrls.h"
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "repl_policies.h"
#include "stats.h"

class Network;

/* General coherent modular cache. The replacement policy and cache array are
 * pretty much mix and match. The coherence controller interfaces are general
 * too, but to avoid virtual function call overheads we work with MESI
 * controllers, since for now we only have MESI controllers
 */
class Cache : public BaseCache {
    protected:
        CC* cc;
        CacheArray* array;
        ReplPolicy* rp;

        uint32_t numLines;

        //Latencies
        uint32_t accLat; //latency of a normal access (could split in get/put, probably not needed)
        uint32_t invLat; //latency of an invalidation

        g_string name;

    public:
        Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name);

        const char* getName();
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);
        void initStats(AggregateStat* parentStat);

        virtual uint64_t access(MemReq& req);

        //NOTE: reqWriteback is pulled up to true, but not pulled down to false.
        virtual uint64_t invalidate(Address lineAddr, InvType type, bool* reqWriteback, uint64_t reqCycle, uint32_t srcId);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

#endif  // CACHE_H_
