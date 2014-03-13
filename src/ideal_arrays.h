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

#ifndef IDEAL_ARRAYS_H_
#define IDEAL_ARRAYS_H_

#include "cache_arrays.h"
#include "g_std/g_unordered_map.h"
#include "intrusive_list.h"
#include "part_repl_policies.h"
#include "repl_policies.h"

/* Fully associative cache arrays with LRU replacement (non-part; part coming up) */

//We use a combination of a hash table and an intrusive list to perform fully-associative lookups and insertions in O(1) time
//TODO: Post-deadline, make it a single array with a rank(req) interface

class IdealLRUArray : public CacheArray {
    private:
        //We need a fake replpolicy and just want the CC...
        class ProxyReplPolicy : public ReplPolicy {
            private:
                IdealLRUArray* a;
            public:
                explicit ProxyReplPolicy(IdealLRUArray* _a) : a(_a) {}
                void setCC(CC* _cc) {a->setCC(cc);}

                void update(uint32_t id, const MemReq* req) {panic("!")}
                void replaced(uint32_t id) {panic("!!");}
                template <typename C> uint32_t rank(const MemReq* req, C cands) {panic("!!!");}
                void initStats(AggregateStat* parent) {}
                DECL_RANK_BINDINGS
        };

        struct Entry : InListNode<Entry> {
            Address lineAddr;
            const uint32_t lineId;
            explicit Entry(uint32_t _lineId) : lineAddr(0), lineId(_lineId) {}
        };

        Entry* array;
        InList<Entry> lruList;
        g_unordered_map<Address, uint32_t> lineMap; //address->lineId; if too slow, try an AATree, which does not alloc dynamically

        uint32_t numLines;
        ProxyReplPolicy* rp;
        CC* cc;

    public:
        explicit IdealLRUArray(uint32_t _numLines) : numLines(_numLines), cc(NULL) {
            array = gm_calloc<Entry>(numLines);
            for (uint32_t i = 0; i < numLines; i++) {
                Entry* e = new (&array[i]) Entry(i);
                lruList.push_front(e);
            }
            rp = new ProxyReplPolicy(this);
        }

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
            g_unordered_map<Address, uint32_t>::iterator it = lineMap.find(lineAddr);
            if (it == lineMap.end()) return -1;

            uint32_t lineId = it->second;
            if (updateReplacement) {
                lruList.remove(&array[lineId]);
                lruList.push_front(&array[lineId]);
            }
            return lineId;
        }

        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
            Entry* e = lruList.back();
            *wbLineAddr = e->lineAddr;
            return e->lineId;
        }

        void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) {
            Entry* e = &array[lineId];

            //Update addr mapping for lineId
            lineMap.erase(e->lineAddr);
            assert(lineMap.find(lineAddr) == lineMap.end());
            e->lineAddr = lineAddr;
            lineMap[lineAddr] = lineId;

            //Update repl
            lruList.remove(e);
            lruList.push_front(e);
        }

        ReplPolicy* getRP() const {return rp;}
        void setCC(CC* _cc) {cc = _cc;}
};

//Goes with IdealLRUPartArray
class IdealLRUPartReplPolicy : public PartReplPolicy {
    protected:
        struct Entry : InListNode<Entry> {
            const uint32_t lineId;
            uint32_t p;
            bool used; //careful, true except when just evicted, even if invalid
            Entry(uint32_t _id, uint32_t _p) : lineId(_id), p(_p), used(true) {}
        };

        struct IdPartInfo : public PartInfo {
            InList<Entry> lruList;
        };

        Entry* array;
        IdPartInfo* partInfo;
        uint32_t partitions;
        uint32_t numLines;
        uint32_t numBuckets;

    public:
        IdealLRUPartReplPolicy(PartitionMonitor* _monitor, PartMapper* _mapper, uint32_t _numLines, uint32_t _numBuckets) : PartReplPolicy(_monitor, _mapper), numLines(_numLines), numBuckets(_numBuckets) {
            partitions = mapper->getNumPartitions();
            partInfo = gm_calloc<IdPartInfo>(partitions);

            for (uint32_t p = 0; p <= partitions; p++) {
                new (&partInfo[p]) IdPartInfo();
                partInfo[p].targetSize = numLines/partitions;
                partInfo[p].size = 0;
            }

            array = gm_calloc<Entry>(numLines);
            for (uint32_t i = 0; i < numLines; i++) {
                Entry* e = new (&array[i]) Entry(i, 0);
                partInfo[0].lruList.push_front(e);
                partInfo[0].size++;
            }
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* rpStat = new AggregateStat();
            rpStat->init("part", "IdealLRUPart replacement policy stats");
            ProxyStat* pStat;
            for (uint32_t p = 0; p < partitions; p++) {
                std::stringstream pss;
                pss << "part-" << p;
                AggregateStat* partStat = new AggregateStat();
                partStat->init(gm_strdup(pss.str().c_str()), "Partition stats");
                pStat = new ProxyStat(); pStat->init("sz", "Actual size", &partInfo[p].size); partStat->append(pStat);
                pStat = new ProxyStat(); pStat->init("tgtSz", "Target size", &partInfo[p].targetSize); partStat->append(pStat);
                partInfo[p].profHits.init("hits", "Hits"); partStat->append(&partInfo[p].profHits);
                partInfo[p].profMisses.init("misses", "Misses"); partStat->append(&partInfo[p].profMisses);
                partInfo[p].profSelfEvictions.init("selfEvs", "Evictions caused by us"); partStat->append(&partInfo[p].profSelfEvictions);
                partInfo[p].profExtEvictions.init("extEvs", "Evictions caused by others (in transients)"); partStat->append(&partInfo[p].profExtEvictions);
                rpStat->append(partStat);
            }
            parentStat->append(rpStat);
        }

        void setPartitionSizes(const uint32_t* sizes) {
            for (uint32_t p = 0; p < partitions; p++) {
                partInfo[p].targetSize = (sizes[p]*numLines)/numBuckets;
            }
        }

        void update(uint32_t id, const MemReq* req) {
            Entry* e = &array[id];
            if (e->used) {
                partInfo[e->p].profHits.inc();
                partInfo[e->p].lruList.remove(e);
                partInfo[e->p].lruList.push_front(e);
            } else {
                uint32_t oldPart = e->p;
                uint32_t newPart = mapper->getPartition(*req);
                if (oldPart != newPart) {
                    partInfo[oldPart].size--;
                    partInfo[oldPart].profExtEvictions.inc();
                    partInfo[newPart].size++;
                } else {
                    partInfo[oldPart].profSelfEvictions.inc();
                }
                partInfo[newPart].profMisses.inc();
                e->p = newPart;
                partInfo[oldPart].lruList.remove(e);
                partInfo[newPart].lruList.push_front(e);
                e->used = true;
            }

            //Update partitioner
            monitor->access(e->p, req->lineAddr);
        }

        void replaced(uint32_t id) {
            array[id].used = false;
        }

        uint32_t rank(const MemReq* req) {
            //Choose part to evict from as a part with highest *proportional* diff between tgt and actual sizes (minimize/smooth transients); if all parts are within limits, evict from own
            uint32_t victimPart = mapper->getPartition(*req);
            double maxPartDiff = 0.0;
            if (partInfo[victimPart].size == 0) maxPartDiff = -2.0; //force a > 0-size partition
            for (uint32_t p = 0; p < partitions; p++) {
                double diff = ((int32_t)partInfo[p].size - (int32_t)partInfo[p].targetSize)/((double)(partInfo[p].targetSize + 1));
                //info("YYY %d %f", p, diff);
                if (diff > maxPartDiff && partInfo[p].size > 0) {
                    maxPartDiff = diff;
                    victimPart = p;
                }
            }
            //assert(maxPartDiff >= -1e-8, "Evicting from non-full line! diff=%f victimPart %d (sz %d tgt %d) origPart %d", ); //someone must be over...
            if (maxPartDiff < -1e-8) {
                warn("Evicting from non-full part! diff=%f victimPart %d (sz %ld tgt %ld) origPart %d",
                    maxPartDiff, victimPart, partInfo[victimPart].size, partInfo[victimPart].targetSize, mapper->getPartition(*req));
            }

            //info("rp: %d / %d %d / %d %d", victimPart, partInfo[0].size, partInfo[0].targetSize, partInfo[1].size, partInfo[1].targetSize);
            assert(partInfo[victimPart].size > 0);
            assert(partInfo[victimPart].size == partInfo[victimPart].lruList.size());
            return partInfo[victimPart].lruList.back()->lineId;
        }

        template <typename C> uint32_t rank(const MemReq* req, C cands) {panic("!!");}
        DECL_RANK_BINDINGS;
};

class IdealLRUPartArray : public CacheArray {
    private:
        g_unordered_map<Address, uint32_t> lineMap; //address->lineId; if too slow, try an AATree, which does not alloc dynamically
        Address* lineAddrs; //lineId -> address, for replacements
        IdealLRUPartReplPolicy* rp;
        uint32_t numLines;

    public:
        IdealLRUPartArray(uint32_t _numLines, IdealLRUPartReplPolicy* _rp) : rp(_rp), numLines(_numLines) {
            lineAddrs = gm_calloc<Address>(numLines);
        }

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
            g_unordered_map<Address, uint32_t>::iterator it = lineMap.find(lineAddr);
            if (it == lineMap.end()) return -1;

            uint32_t lineId = it->second;
            if (updateReplacement) {
                rp->update(lineId, req);
            }
            return lineId;
        }

        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
            uint32_t lineId = rp->rank(req);
            *wbLineAddr = lineAddrs[lineId];
            return lineId;
        }

        void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) {
            //Update addr mapping for lineId
            lineMap.erase(lineAddrs[lineId]);
            assert(lineMap.find(lineAddr) == lineMap.end());
            lineAddrs[lineId] = lineAddr;
            lineMap[lineAddr] = lineId;

            //Update repl
            rp->replaced(lineId);
            rp->update(lineId, req);
        }
};

#endif  // IDEAL_ARRAYS_H_
