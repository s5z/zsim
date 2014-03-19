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

#ifndef PART_REPL_POLICIES_H_
#define PART_REPL_POLICIES_H_

#include <sstream>
#include <stdint.h>
#include "event_queue.h"
#include "mtrand.h"
#include "partition_mapper.h"
#include "partitioner.h"
#include "repl_policies.h"

struct PartInfo {
    uint64_t size; //in lines
    uint64_t targetSize; //in lines

    Counter profHits;
    Counter profMisses;
    Counter profSelfEvictions; // from our same partition
    Counter profExtEvictions; // from other partitions (if too large, we're probably doing something wrong, e.g., too small an adjustment period)
};

class PartReplPolicy : public virtual ReplPolicy {
    protected:
        PartitionMonitor* monitor;
        PartMapper* mapper;

    public:
        PartReplPolicy(PartitionMonitor* _monitor, PartMapper* _mapper) : monitor(_monitor), mapper(_mapper) {}
        ~PartReplPolicy() { delete monitor; }

        virtual void setPartitionSizes(const uint32_t* sizes) = 0;

        PartitionMonitor* getMonitor() { return monitor; }
        const PartitionMonitor* getMonitor() const { return monitor; }
};

class WayPartReplPolicy : public PartReplPolicy, public LegacyReplPolicy {
    private:
        PartInfo* partInfo;
        uint32_t partitions;

        uint32_t totalSize;
        uint32_t waySize;
        uint32_t ways;

        struct WayPartInfo {
            Address addr; //FIXME: This is redundant due to the replacement policy interface
            uint64_t ts; //timestamp, >0 if in the cache, == 0 if line is empty
            uint32_t p;
        };

        WayPartInfo* array;

        uint32_t* wayPartIndex; //stores partition of each way

        bool testMode;

        PAD();

        //Replacement process state (RW)
        int32_t bestId;
        uint32_t candIdx;
        uint32_t incomingLinePart; //to what partition does the incoming line belong?
        Address incomingLineAddr;

        //Globally incremented, but bears little significance per se
        uint64_t timestamp;

    public:
        WayPartReplPolicy(PartitionMonitor* _monitor, PartMapper* _mapper, uint64_t _lines, uint32_t _ways, bool _testMode)
                : PartReplPolicy(_monitor, _mapper), totalSize(_lines), ways(_ways), testMode(_testMode)
        {
            partitions = mapper->getNumPartitions();
            waySize = totalSize/ways;
            assert(waySize*ways == totalSize); //no partial ways...

            partInfo = gm_calloc<PartInfo>(partitions);
            for (uint32_t i = 0; i < partitions; i++) {
                partInfo[i].targetSize = 0;

                //Need placement new, these object have vptr
                new (&partInfo[i].profHits) Counter;
                new (&partInfo[i].profMisses) Counter;
                new (&partInfo[i].profSelfEvictions) Counter;
                new (&partInfo[i].profExtEvictions) Counter;
            }

            array = gm_calloc<WayPartInfo>(totalSize); //all have ts, p == 0...
            partInfo[0].size = totalSize; // so partition 0 has all the lines

            wayPartIndex = gm_calloc<uint32_t>(ways);
            for (uint32_t w = 0; w < ways; w++) {
                //Do initial way assignment, partitioner has no profiling info yet
                uint32_t p = w*partitions/ways; // in [0, ..., partitions-1]
                wayPartIndex[w] = p;
                partInfo[p].targetSize += waySize;
            }

            candIdx = 0;
            bestId = -1;
            timestamp = 1;
        }

        void initStats(AggregateStat* parentStat) {
            //AggregateStat* partsStat = new AggregateStat(true /*this is a regular aggregate, ONLY PARTITION STATS GO IN HERE*/);
            AggregateStat* partsStat = new AggregateStat(false); //don't make it a regular aggregate... it gets compacted in periodic stats and becomes useless!
            partsStat->init("part", "Partition stats");
            for (uint32_t p = 0; p < partitions; p++) {
                std::stringstream pss;
                pss << "part-" << p;
                AggregateStat* partStat = new AggregateStat();
                partStat->init(gm_strdup(pss.str().c_str()), "Partition stats");
                ProxyStat* pStat;
                pStat = new ProxyStat(); pStat->init("sz", "Actual size", &partInfo[p].size); partStat->append(pStat);
                pStat = new ProxyStat(); pStat->init("tgtSz", "Target size", &partInfo[p].targetSize); partStat->append(pStat);
                partInfo[p].profHits.init("hits", "Hits"); partStat->append(&partInfo[p].profHits);
                partInfo[p].profMisses.init("misses", "Misses"); partStat->append(&partInfo[p].profMisses);
                partInfo[p].profSelfEvictions.init("selfEvs", "Evictions caused by us"); partStat->append(&partInfo[p].profSelfEvictions);
                partInfo[p].profExtEvictions.init("extEvs", "Evictions caused by others"); partStat->append(&partInfo[p].profExtEvictions);

                partsStat->append(partStat);
            }
            parentStat->append(partsStat);
        }

        void update(uint32_t id, const MemReq* req) {
            WayPartInfo* e = &array[id];
            if (e->ts > 0) { //this is a hit update
                partInfo[e->p].profHits.inc();
            } else { //post-miss update, old line has been removed, this is empty
                uint32_t oldPart = e->p;
                uint32_t newPart = incomingLinePart;
                if (oldPart != newPart) {
                    partInfo[oldPart].size--;
                    partInfo[oldPart].profExtEvictions.inc();
                    partInfo[newPart].size++;
                } else {
                    partInfo[oldPart].profSelfEvictions.inc();
                }
                partInfo[newPart].profMisses.inc();
                e->p = newPart;
            }
            e->ts = timestamp++;

            //Update partitioner...
            monitor->access(e->p, e->addr);
        }

        void startReplacement(const MemReq* req) {
            assert(candIdx == 0);
            assert(bestId == -1);
            incomingLinePart = mapper->getPartition(*req);
            incomingLineAddr = req->lineAddr;
        }

        void recordCandidate(uint32_t id) {
            assert(candIdx < ways);
            WayPartInfo* c = &array[id]; //candidate info
            WayPartInfo* best = (bestId >= 0)? &array[bestId] : nullptr;
            uint32_t way = candIdx++;
            //In test mode, this works as LRU
            if (testMode || wayPartIndex[way] == incomingLinePart) { //this is a way we can fill
                if (best == nullptr) {
                    bestId = id;
                } else {
                    //NOTE: This is actually not feasible without tagging. But what IS feasible is to stop updating the LRU position on new fills. We could kill this, and profile the differences.
                    if ( testMode || (c->p == incomingLinePart && best->p == incomingLinePart) ) {
                        if (c->ts < best->ts) bestId = id;
                    } else if (c->p == incomingLinePart && best->p != incomingLinePart) {
                        //c wins
                    } else if (c->p != incomingLinePart && best->p == incomingLinePart) {
                        //c loses
                        bestId = id;
                    } else { //none in our partition, this should be transient but at least enforce LRU
                        if (c->ts < best->ts) bestId = id;
                    }
                }
            }
        }

        uint32_t getBestCandidate() {
            assert(bestId >= 0);
            return bestId;
        }

        void replaced(uint32_t id) {
            candIdx = 0;
            bestId = -1;
            array[id].ts = 0;
            array[id].addr = incomingLineAddr;
            //info("0x%lx", incomingLineAddr);
        }

    private:
        void setPartitionSizes(const uint32_t* waysPart) {
            uint32_t curWay = 0;
            for (uint32_t p = 0; p < partitions; p++) {
                partInfo[p].targetSize = totalSize*waysPart[p]/ways;
#if UMON_INFO
                info("part %d assigned %d ways", p, waysPart[p]);
#endif
                for (uint32_t i = 0; i < waysPart[p]; i++) wayPartIndex[curWay++] = p;
            }
#if UMON_INFO
            for (uint32_t w = 0; w < ways; w++) info("wayPartIndex[%d] = %d", w, wayPartIndex[w]);
#endif
            assert(curWay == ways);
        }
};

#define VANTAGE_8BIT_BTS 1 //1 for 8-bit coarse-grain timestamps, 0 for 64-bit coarse-grain (no wrap-arounds)

/* Vantage replacement policy. Please refer to our ISCA 2011 paper for implementation details.
 */
class VantageReplPolicy : public PartReplPolicy, public LegacyReplPolicy {
    private:
        /* NOTE: This implementation uses 64-bit coarse-grain TSs for simplicity. You have a choice of constraining
         * these to work 8-bit timestamps by setting VANTAGE_8BIT_BTS to 1. Note that this code still has remnants of
         * the 64-bit global fine-grain timestamps used to simulate perfect LRU. They are not using for anything but profiling.
         */
        uint32_t partitions;
        uint32_t totalSize;
        uint32_t assoc;

        struct VantagePartInfo : public PartInfo {
            uint64_t curBts; //per-partition coarse-grain timestamp (CurrentTS in paper)
            uint32_t curBtsHits; //hits on current timestamp (AccessCounter in paper)

            uint64_t setpointBts; // setpoint coarse-grain timestamp (SetpointTS in paper)
            uint64_t setpointAdjs; // setpoint adjustments so far, just for profiling purposes

            uint32_t curIntervalIns; // insertions in current interval. Not currently used.
            uint32_t curIntervalDems; // CandsDemoted in paper
            uint32_t curIntervalCands; // CandsSeen in paper

            uint64_t extendedSize;

            uint64_t longTermTargetSize; //in lines

            Counter profDemotions;
            Counter profEvictions;
            Counter profSizeCycles;
            Counter profExtendedSizeCycles;
        };

        VantagePartInfo* partInfo;

        struct LineInfo {
            Address addr; //FIXME: This is redundant due to the replacement policy interface
            uint64_t ts; //timestamp, >0 if in the cache, == 0 if line is empty (little significance otherwise)
            uint64_t bts; //coarse-grain per-partition timestamp
            uint32_t p; //partition ID
            uint32_t op; //original partition id: same as partition id when in partition, but does not change when moved to FFA (unmanaged region)
        };

        LineInfo* array;

        Counter profPromotions;
        Counter profUpdateCycles;

        //Repl process stuff
        uint32_t* candList;
        uint32_t candIdx;
        Address incomingLineAddr;

        //Globally incremented, but bears little significance per se
        uint64_t timestamp;

        double partPortion; //how much of the cache do we devote to the partition's target sizes?
        double partSlack; //how much the aperture curve reacts to "cushion" the load. partSlack+targetSize sets aperture to 1.0
        double maxAperture; //Maximum aperture allowed in each partition, must be < 1.0
        uint32_t partGranularity; //number of partitions that UMON/LookaheadPartitioner expects

        uint64_t lastUpdateCycle; //for cumulative size counter updates; could be made event-driven

        MTRand rng;
        bool smoothTransients; //if set, keeps all growing partitions at targetSz = actualSz + 1 until they reach their actual target; takes space away slowly from the shrinking partitions instead of aggressively demoting them to the unmanaged region, which turns the whole thing into a shared cache if transients are frequent

    public:
        VantageReplPolicy(PartitionMonitor* _monitor, PartMapper* _mapper, uint64_t _lines,  uint32_t _assoc, uint32_t partPortionPct,
                          uint32_t partSlackPct, uint32_t maxAperturePct, uint32_t _partGranularity, bool _smoothTransients)
                : PartReplPolicy(_monitor, _mapper), totalSize(_lines), assoc(_assoc), rng(0xABCDE563F), smoothTransients(_smoothTransients)
        {
            partitions = mapper->getNumPartitions();

            assert(partPortionPct <= 100);
            assert(partSlackPct <= 100);
            assert(maxAperturePct <= 100);

            partPortion = ((double)partPortionPct)/100.0;
            partSlack = ((double)partSlackPct)/100.0;
            maxAperture = ((double)maxAperturePct)/100.0;
            partGranularity = _partGranularity;  // NOTE: partitioning at too fine granularity (+1K buckets) overwhelms the lookahead partitioner

            uint32_t targetManagedSize = (uint32_t)(((double)totalSize)*partPortion);

            partInfo = gm_calloc<VantagePartInfo>(partitions+1);  // last one is unmanaged region

            for (uint32_t i = 0; i <= partitions; i++) {
                partInfo[i].targetSize = targetManagedSize/partitions;
                partInfo[i].longTermTargetSize = partInfo[i].targetSize;
                partInfo[i].extendedSize = 0;

                //Need placement new, these objects have vptr
                new (&partInfo[i].profHits) Counter;
                new (&partInfo[i].profMisses) Counter;
                new (&partInfo[i].profSelfEvictions) Counter;
                new (&partInfo[i].profExtEvictions) Counter;
                new (&partInfo[i].profDemotions) Counter;
                new (&partInfo[i].profEvictions) Counter;
                new (&partInfo[i].profSizeCycles) Counter;
                new (&partInfo[i].profExtendedSizeCycles) Counter;
            }

            //unmanaged region should not use these
            partInfo[partitions].targetSize = 0;
            partInfo[partitions].longTermTargetSize = 0;

            array = gm_calloc<LineInfo>(totalSize);

            //Initially, assign all the lines to the unmanaged region
            partInfo[partitions].size = totalSize;
            partInfo[partitions].extendedSize = totalSize;
            for (uint32_t i = 0; i < totalSize; i++) {
                array[i].p = partitions;
                array[i].op = partitions;
            }

            candList = gm_calloc<uint32_t>(assoc);
            candIdx = 0;
            timestamp = 1;

            lastUpdateCycle = 0;

            info("Vantage RP: %d partitions, managed portion %f Amax %f slack %f", partitions, partPortion, maxAperture, partSlack);
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* rpStat = new AggregateStat();
            rpStat->init("part", "Vantage replacement policy stats");
            ProxyStat* pStat;
            profPromotions.init("ffaProms", "Promotions from unmanaged region"); rpStat->append(&profPromotions);
            profUpdateCycles.init("updCycles", "Cycles of updates experienced on size-cycle counters"); rpStat->append(&profUpdateCycles);
            for (uint32_t p = 0; p <= partitions; p++) {
                std::stringstream pss;
                pss << "part-" << p;
                AggregateStat* partStat = new AggregateStat();
                partStat->init(gm_strdup(pss.str().c_str()), "Partition stats");

                pStat = new ProxyStat(); pStat->init("sz", "Actual size", &partInfo[p].size); partStat->append(pStat);
                pStat = new ProxyStat(); pStat->init("xSz", "Extended actual size, including lines currently demoted to FFA", &partInfo[p].extendedSize); partStat->append(pStat);
                //NOTE: To avoid breaking scripts, I've changed tgtSz to track longTermTargetSize
                //FIXME: Code and stats should be named similarly
                pStat = new ProxyStat(); pStat->init("tgtSz", "Target size", &partInfo[p].longTermTargetSize); partStat->append(pStat);
                pStat = new ProxyStat(); pStat->init("stTgtSz", "Short-term target size (used with smoothedTransients)", &partInfo[p].targetSize); partStat->append(pStat);
                partInfo[p].profHits.init("hits", "Hits"); partStat->append(&partInfo[p].profHits);
                partInfo[p].profMisses.init("misses", "Misses"); partStat->append(&partInfo[p].profMisses);
                //Vantage does not do evictions directly, these do not make sense and are not used
                //partInfo[p].profSelfEvictions.init("selfEvs", "Evictions caused by us"); partStat->append(&partInfo[p].profSelfEvictions);
                //partInfo[p].profExtEvictions.init("extEvs", "Evictions caused by others"); partStat->append(&partInfo[p].profExtEvictions);
                partInfo[p].profDemotions.init("dems", "Demotions"); partStat->append(&partInfo[p].profDemotions);
                partInfo[p].profEvictions.init("evs", "Evictions"); partStat->append(&partInfo[p].profEvictions);
                partInfo[p].profSizeCycles.init("szCycles", "Cumulative per-cycle sum of sz"); partStat->append(&partInfo[p].profSizeCycles);
                partInfo[p].profExtendedSizeCycles.init("xSzCycles", "Cumulative per-cycle sum of xSz"); partStat->append(&partInfo[p].profExtendedSizeCycles);

                rpStat->append(partStat);
            }
            parentStat->append(rpStat);
        }

        void update(uint32_t id, const MemReq* req) {
            if (unlikely(zinfo->globPhaseCycles > lastUpdateCycle)) {
                //Update size-cycle counter stats
                uint64_t diff = zinfo->globPhaseCycles - lastUpdateCycle;
                for (uint32_t p = 0; p <= partitions; p++) {
                    partInfo[p].profSizeCycles.inc(diff*partInfo[p].size);
                    partInfo[p].profExtendedSizeCycles.inc(diff*partInfo[p].extendedSize);
                }
                profUpdateCycles.inc(diff);
                lastUpdateCycle = zinfo->globPhaseCycles;
            }

            LineInfo* e = &array[id];
            if (e->ts > 0) {
                if (e->p == partitions) { //this is an unmanaged region promotion
                    e->p = mapper->getPartition(*req);
                    profPromotions.inc();
                    partInfo[e->p].curIntervalIns++;
                    partInfo[e->p].size++;
                    partInfo[partitions].size--;
                }
                e->ts = timestamp++;
                partInfo[e->p].profHits.inc();
            } else { //post-miss update, old one has been removed, this is empty
                e->ts = timestamp++;
                partInfo[e->p].size--;
                partInfo[e->p].profEvictions.inc();
                partInfo[e->op].extendedSize--;
                e->p = mapper->getPartition(*req);
                e->op = e->p;
                partInfo[e->p].curIntervalIns++;
                partInfo[e->p].size++;
                partInfo[e->op].extendedSize++;
                partInfo[e->p].profMisses.inc();

                if (partInfo[e->p].targetSize < partInfo[e->p].longTermTargetSize) {
                    assert(smoothTransients);
                    partInfo[e->p].targetSize++;
                    takeOneLine();
                }
            }

            //Profile the access
            monitor->access(e->p, e->addr);

            //Adjust coarse-grain timestamp
            e->bts = partInfo[e->p].curBts;
            if (++partInfo[e->p].curBtsHits >= (uint32_t) partInfo[e->p].size/16) {
                partInfo[e->p].curBts++;
                partInfo[e->p].setpointBts++;
                partInfo[e->p].curBtsHits = 0;
            }
        }

        void startReplacement(const MemReq* req) {
            incomingLineAddr = req->lineAddr;
        }

        void recordCandidate(uint32_t id) {
            assert(candIdx < assoc);
            candList[candIdx++] = id;
        }

        uint32_t getBestCandidate() {
            assert(candIdx > 0);
            assert(candIdx <= assoc);

            //Demote all lines below their setpoints
            for (uint32_t i = 0; i < candIdx; i++) {
                LineInfo* e = &array[candList[i]];
                if (e->ts == 0) continue; //empty, bypass

                uint32_t p = e->p;
                if (p == partitions) continue; //bypass unmanaged region entries

                uint32_t size = partInfo[p].size;

                if (size <= partInfo[p].targetSize) continue; //bypass partitions below target

#if VANTAGE_8BIT_BTS
                //Must do mod 256 arithmetic. This will do generally worse because of wrap-arounds, but wrapping around is pretty rare
                //TODO: Doing things this way, we can profile the difference between this and using larger coarse-grain timestamps
                if (((partInfo[p].curBts - e->bts) % 256) /*8-bit distance to current TS*/ >= ((partInfo[p].curBts - partInfo[p].setpointBts) % 256)) {
#else
                if (e->bts <= partInfo[p].setpointBts) {
#endif
                    // Demote!
                    // Out of p
                    partInfo[p].profDemotions.inc();
                    partInfo[p].size--;

                    // Into unmanaged
                    e->p = partitions;
                    partInfo[partitions].size++;

                    partInfo[p].curIntervalDems++;

                    //Note extended size and op not affected
                }

                partInfo[p].curIntervalCands++;

                // See if we need interval change
                if (/*partInfo[p].curIntervalDems >= 16 || partInfo[p].curIntervalIns >= 16 ||*/ partInfo[p].curIntervalCands >= 256) {
                    double maxSz = partInfo[p].targetSize*(1.0 + partSlack);
                    double curSz = partInfo[p].size;
                    double aperture = 0.0;

                    // Feedback-based aperture control
                    // TODO: Copy over the demotion thresholds lookup table code from the ISCA paper code, or quantize this.
                    // This is doing finer-grain demotions, but requires a bit more math.
                    if (curSz >= maxSz) {
                        aperture = maxAperture;
                    } else {
                        double slope = (maxAperture)/(maxSz - partInfo[p].targetSize);
                        assert(slope > 0.0);
                        aperture = slope*(curSz - partInfo[p].targetSize);
                    }

                    if (aperture > 0.0) {
/*
                        info ("part %d setpoint adjust, curSz %f tgtSz %ld maxSz %f aperture %f curBts %ld setpointBts %ld interval cands %d ins %d dems %d cpt %f",
                            p, curSz, partInfo[p].targetSize, maxSz, aperture, partInfo[p].curBts, partInfo[p].setpointBts, partInfo[p].curIntervalCands,\
                            partInfo[p].curIntervalIns, partInfo[p].curIntervalDems, partInfo[p].curIntervalCands*aperture);
*/

                        int32_t shrink = partInfo[p].curIntervalDems;
                        if (shrink < aperture*partInfo[p].curIntervalCands) {
                            //info ("increasing setpoint");
                            if (partInfo[p].setpointBts < partInfo[p].curBts) partInfo[p].setpointBts++;
                        } else if (shrink > aperture*partInfo[p].curIntervalCands) {
                            //info ("decreasing setpoint");
#if VANTAGE_8BIT_BTS
                            //Never get the setpoint to go 256 positions behind the current timestamp
                            if ((partInfo[p].curBts - partInfo[p].setpointBts) < 255) partInfo[p].setpointBts--;
#else
                            if (partInfo[p].setpointBts > 0) partInfo[p].setpointBts--;
#endif
                        } else {
                            //info ("keeping setpoint");
                        }
                    }

                    //info("part %d post setpointBts %ld", p, partInfo[p].setpointBts);

                    partInfo[p].curIntervalCands = 0;
                    partInfo[p].curIntervalIns = 0;
                    partInfo[p].curIntervalDems = 0;
                    partInfo[p].setpointAdjs++;
                }
            } //for

            //Get best candidate for eviction
            int32_t bestId = candList[0];

            for (uint32_t i = 0; i < candIdx; i++) { //note we include 0; 0 compares with itself, see shortcut to understand why
                uint32_t id = candList[i];
                LineInfo* e = &array[id];
                LineInfo* best = &array[bestId];

                if (e->ts == 0) {
                    //shortcut for empty positions
                    bestId = id;
                    break;
                }

                uint32_t p = e->p;

                if (p == partitions && best->p != partitions) { //prioritize umgd
                    bestId = id;
                } else if (p == partitions && best->p == partitions) {
                    if (e->ts < best->ts) bestId = id;
                } else if (p != partitions && best->p == partitions) {
                    //best wins, prioritize unmanaged
                } else {
                    assert(p != partitions && best->p != partitions);
                    //Just do LRU; with correctly-sized partitions, this is VERY rare
                    //NOTE: If we were to study really small unmanaged regions, we can always get fancier and prioritize by aperture, bts, etc.
                    if (e->ts < best->ts) bestId = id;
                }
            }
            assert(bestId >= 0 && (uint32_t)bestId < totalSize);
            return bestId;
        }

        void replaced(uint32_t id) {
            candIdx = 0; //reset

            LineInfo* e = &array[id];
            e->ts = 0;
            e->bts = 0;
            e->addr = incomingLineAddr;
        }

    private:
        void setPartitionSizes(const uint32_t* sizes) {
            uint32_t s[partitions];
            uint32_t usedSize = 0;
            uint32_t linesToTakeAway = 0;
            for (uint32_t p = 0; p < partitions; p++) {
                s[p] = totalSize*sizes[p]/partGranularity;
#if UMON_INFO
                info("part %d, %ld -> %d lines (now it's %ld lines) [cur %ld/%ld set %ld/%ld setAdjs %ld]", p, partInfo[p].targetSize, s[p],
                        partInfo[p].size, partInfo[p].curBts, partInfo[p].curBts % 256, partInfo[p].setpointBts, partInfo[p].setpointBts % 256, partInfo[p].setpointAdjs);
#endif
                if (smoothTransients) {
                    partInfo[p].longTermTargetSize = s[p];
                    if (s[p] > partInfo[p].targetSize) { //growing
                        uint32_t newTarget = MAX(partInfo[p].targetSize, MIN(partInfo[p].longTermTargetSize, partInfo[p].size+1)); //always in [target,longTermTarget]
                        linesToTakeAway += newTarget - partInfo[p].targetSize;
                        partInfo[p].targetSize = newTarget;
                    }
                } else {
                    partInfo[p].targetSize = s[p];
                    partInfo[p].longTermTargetSize = s[p];
                }
                usedSize += s[p];
            }

            while (linesToTakeAway--) takeOneLine();
#if UMON_INFO
            info("%d lines assigned, %d unmanaged", usedSize, totalSize - usedSize);
#endif
        }

        void takeOneLine() {
            assert(smoothTransients);
            uint32_t linesLeft = 0;
            //NOTE: This is a fairly inefficient implementation, but we can do it cheaply in hardware
            //Take away proportionally to difference between actual and long-term target
            for (uint32_t p = 0; p < partitions; p++) {
                int32_t left = partInfo[p].targetSize - partInfo[p].longTermTargetSize;
                linesLeft += MAX(left, 0);
            }
            assert(linesLeft > 0);
            uint32_t l = rng.randInt(linesLeft-1); //[0, linesLeft-1]
            uint32_t curLines = 0;
            for (uint32_t p = 0; p < partitions; p++) {
                int32_t left = partInfo[p].targetSize - partInfo[p].longTermTargetSize;
                curLines += MAX(left, 0);
                if (left > 0 && l < curLines) {
                    partInfo[p].targetSize--;
                    return;
                }
            }
            panic("Could not find any partition to take away space from???");
        }
};

#endif  // PART_REPL_POLICIES_H_
