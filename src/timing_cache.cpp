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

#include "timing_cache.h"
#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

// Events
class HitEvent : public TimingEvent {
    private:
        TimingCache* cache;

    public:
        HitEvent(TimingCache* _cache,  uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache) {}

        void simulate(uint64_t startCycle) {
            cache->simulateHit(this, startCycle);
        }
};


class MissStartEvent : public TimingEvent {
    private:
        TimingCache* cache;
    public:
        uint64_t startCycle; //for profiling purposes
        MissStartEvent(TimingCache* _cache,  uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache) {}
        void simulate(uint64_t startCycle) {cache->simulateMissStart(this, startCycle);}
};

class MissResponseEvent : public TimingEvent {
    private:
        TimingCache* cache;
        MissStartEvent* mse;
    public:
        MissResponseEvent(TimingCache* _cache, MissStartEvent* _mse, int32_t domain) : TimingEvent(0, 0, domain), cache(_cache), mse(_mse) {}
        void simulate(uint64_t startCycle) {cache->simulateMissResponse(this, startCycle, mse);}
};

class MissWritebackEvent : public TimingEvent {
    private:
        TimingCache* cache;
        MissStartEvent* mse;
    public:
        MissWritebackEvent(TimingCache* _cache,  MissStartEvent* _mse, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), mse(_mse) {}
        void simulate(uint64_t startCycle) {cache->simulateMissWriteback(this, startCycle, mse);}
};

class ReplAccessEvent : public TimingEvent {
    private:
        TimingCache* cache;
    public:
        uint32_t accsLeft;
        ReplAccessEvent(TimingCache* _cache, uint32_t _accsLeft, uint32_t preDelay, uint32_t postDelay, int32_t domain) : TimingEvent(preDelay, postDelay, domain), cache(_cache), accsLeft(_accsLeft) {}
        void simulate(uint64_t startCycle) {cache->simulateReplAccess(this, startCycle);}
};

TimingCache::TimingCache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp,
        uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t _tagLat, uint32_t _ways, uint32_t _cands, uint32_t _domain, const g_string& _name)
    : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name), numMSHRs(mshrs), tagLat(_tagLat), ways(_ways), cands(_cands)
{
    lastFreeCycle = 0;
    lastAccCycle = 0;
    assert(numMSHRs > 0);
    activeMisses = 0;
    domain = _domain;
    info("%s: mshrs %d domain %d", name.c_str(), numMSHRs, domain);
}

void TimingCache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Timing cache stats");
    initCacheStats(cacheStat);

    //Stats specific to timing cache
    profOccHist.init("occHist", "Occupancy MSHR cycle histogram", numMSHRs+1);
    cacheStat->append(&profOccHist);

    profHitLat.init("latHit", "Cumulative latency accesses that hit (demand and non-demand)");
    profMissRespLat.init("latMissResp", "Cumulative latency for miss start to response");
    profMissLat.init("latMiss", "Cumulative latency for miss start to finish (free MSHR)");

    cacheStat->append(&profHitLat);
    cacheStat->append(&profMissRespLat);
    cacheStat->append(&profMissLat);

    parentStat->append(cacheStat);
}

// TODO(dsm): This is copied verbatim from Cache. We should split Cache into different methods, then call those.
uint64_t TimingCache::access(MemReq& req) {
    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "TimingCache is not connected to TimingCore");
    uint32_t initialRecords = evRec->numRecords();

    bool hasWritebackRecord = false;
    TimingRecord writebackRecord;
    bool hasAccessRecord = false;
    TimingRecord accessRecord;
    uint64_t evDoneCycle = 0;
    
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (lineId == -1 /*&& cc->shouldAllocate(req)*/) {
            assert(cc->shouldAllocate(req)); //dsm: for now, we don't deal with non-inclusion in TimingCache

            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            evDoneCycle = cc->processEviction(req, wbLineAddr, lineId, respCycle); //if needed, send invalidates/downgrades to lower level, and wb to upper level

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.

            if (evRec->numRecords() > initialRecords) {
                assert_msg(evRec->numRecords() == initialRecords + 1, "evRec records on eviction %ld", evRec->numRecords());
                writebackRecord = evRec->getRecord(initialRecords);
                hasWritebackRecord = true;
                evRec->popRecord();
            }
        }

        uint64_t getDoneCycle = respCycle;
        respCycle = cc->processAccess(req, lineId, respCycle, &getDoneCycle);

        if (evRec->numRecords() > initialRecords) {
            assert_msg(evRec->numRecords() == initialRecords + 1, "evRec records %ld", evRec->numRecords());
            accessRecord = evRec->getRecord(initialRecords);
            hasAccessRecord = true;
            evRec->popRecord();
        }

        // At this point we have all the info we need to hammer out the timing record
        TimingRecord tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr}; //note the end event is the response, not the wback

        if (getDoneCycle - req.cycle == accLat) {
            // Hit
            assert(!hasWritebackRecord);
            assert(!hasAccessRecord);
            uint64_t hitLat = respCycle - req.cycle; // accLat + invLat
            HitEvent* ev = new (evRec) HitEvent(this, hitLat, domain);
            ev->setMinStartCycle(req.cycle);
            tr.startEvent = tr.endEvent = ev;
        } else {
            assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

            // Miss events:
            // MissStart (does high-prio lookup) -> getEvent || evictionEvent || replEvent (if needed) -> MissWriteback

            MissStartEvent* mse = new (evRec) MissStartEvent(this, accLat, domain);
            MissResponseEvent* mre = new (evRec) MissResponseEvent(this, mse, domain);
            MissWritebackEvent* mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);

            mse->setMinStartCycle(req.cycle);
            mre->setMinStartCycle(getDoneCycle);
            mwe->setMinStartCycle(MAX(evDoneCycle, getDoneCycle));

            // Tie two events to an optional timing record
            // TODO: Promote to evRec if this is more generally useful
            auto connect = [evRec](const TimingRecord* r, TimingEvent* startEv, TimingEvent* endEv, uint64_t startCycle, uint64_t endCycle) {
                assert_msg(startCycle <= endCycle, "start > end? %ld %ld", startCycle, endCycle);
                if (r) {
                    assert_msg(startCycle <= r->reqCycle, "%ld / %ld", startCycle, r->reqCycle);
                    assert_msg(r->respCycle <= endCycle, "%ld %ld %ld %ld", startCycle, r->reqCycle, r->respCycle, endCycle);
                    uint64_t upLat = r->reqCycle - startCycle;
                    uint64_t downLat = endCycle - r->respCycle;

                    if (upLat) {
                        DelayEvent* dUp = new (evRec) DelayEvent(upLat);
                        dUp->setMinStartCycle(startCycle);
                        startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
                    } else {
                        startEv->addChild(r->startEvent, evRec);
                    }

                    if (downLat) {
                        DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                        dDown->setMinStartCycle(r->respCycle);
                        r->endEvent->addChild(dDown, evRec)->addChild(endEv, evRec);
                    } else {
                        r->endEvent->addChild(endEv, evRec);
                    }
                } else {
                    if (startCycle == endCycle) {
                        startEv->addChild(endEv, evRec);
                    } else {
                        DelayEvent* dEv = new (evRec) DelayEvent(endCycle - startCycle);
                        dEv->setMinStartCycle(startCycle);
                        startEv->addChild(dEv, evRec)->addChild(endEv, evRec);
                    }
                }
            };

            // Get path
            connect(hasAccessRecord? &accessRecord : nullptr, mse, mre, req.cycle + accLat, getDoneCycle);
            mre->addChild(mwe, evRec);

            // Eviction path
            if (evDoneCycle) {
                connect(hasWritebackRecord? &writebackRecord : nullptr, mse, mwe, req.cycle + accLat, evDoneCycle);
            }

            // Replacement path
            if (evDoneCycle && cands > ways) {
                uint32_t replLookups = (cands + (ways-1))/ways - 1; // e.g., with 4 ways, 5-8 -> 1, 9-12 -> 2, etc.
                assert(replLookups);

                uint32_t fringeAccs = ways - 1;
                uint32_t accsSoFar = 0;

                TimingEvent* p = mse;

                // Candidate lookup events
                while (accsSoFar < replLookups) {
                    uint32_t preDelay = accsSoFar? 0 : tagLat;
                    uint32_t postDelay = tagLat - MIN(tagLat - 1, fringeAccs);
                    uint32_t accs = MIN(fringeAccs, replLookups - accsSoFar);
                    //info("ReplAccessEvent rl %d fa %d preD %d postD %d accs %d", replLookups, fringeAccs, preDelay, postDelay, accs);
                    ReplAccessEvent* raEv = new (evRec) ReplAccessEvent(this, accs, preDelay, postDelay, domain);
                    raEv->setMinStartCycle(req.cycle /*lax...*/);
                    accsSoFar += accs;
                    p->addChild(raEv, evRec);
                    p = raEv;
                    fringeAccs *= ways - 1;
                }

                // Swap events -- typically, one read and one write work for 1-2 swaps. Exact number depends on layout.
                ReplAccessEvent* rdEv = new (evRec) ReplAccessEvent(this, 1, tagLat, tagLat, domain);
                rdEv->setMinStartCycle(req.cycle /*lax...*/);
                ReplAccessEvent* wrEv = new (evRec) ReplAccessEvent(this, 1, 0, 0, domain);
                wrEv->setMinStartCycle(req.cycle /*lax...*/);

                p->addChild(rdEv, evRec)->addChild(wrEv, evRec)->addChild(mwe, evRec);
            }


            tr.startEvent = mse;
            tr.endEvent = mre; // note the end event is the response, not the wback
        }
        evRec->pushRecord(tr);
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}


uint64_t TimingCache::highPrioAccess(uint64_t cycle) {
    assert(cycle >= lastFreeCycle);
    uint64_t lookupCycle = MAX(cycle, lastAccCycle+1);
    if (lastAccCycle < cycle-1) lastFreeCycle = cycle-1; //record last free run
    lastAccCycle = lookupCycle;
    return lookupCycle;
}

/* The simple things you see here are complicated,
 * I look pretty young but I'm just back-dated...
 * 
 * To make this efficient, we do not want to keep priority queues. Instead, a
 * low-priority access is granted if there was a free slot on the *previous*
 * cycle. This means that low-prio accesses should be post-dated by 1 cycle.
 * This is fine to do, since these accesses are writebacks and non critical
 * path accesses. Essentially, we're modeling that we know those accesses one
 * cycle in advance.
 */
uint64_t TimingCache::tryLowPrioAccess(uint64_t cycle) {
    if (lastAccCycle < cycle-1 || lastFreeCycle == cycle-1) {
        lastFreeCycle = 0;
        lastAccCycle = MAX(cycle-1, lastAccCycle);
        return cycle;
    } else {
        return 0;
    }
}

void TimingCache::simulateHit(HitEvent* ev, uint64_t cycle) {
    if (activeMisses < numMSHRs) {
        uint64_t lookupCycle = highPrioAccess(cycle);
        profHitLat.inc(lookupCycle-cycle);
        ev->done(lookupCycle);  // postDelay includes accLat + invalLat
    } else {
        // queue
        ev->hold();
        pendingQueue.push_back(ev);
    }
}

void TimingCache::simulateMissStart(MissStartEvent* ev, uint64_t cycle) {
    if (activeMisses < numMSHRs) {
        activeMisses++;
        profOccHist.transition(activeMisses, cycle);

        ev->startCycle = cycle;
        uint64_t lookupCycle = highPrioAccess(cycle);
        ev->done(lookupCycle);
    } else {
        //info("Miss, all MSHRs used, queuing");
        ev->hold();
        pendingQueue.push_back(ev);
    }
}

void TimingCache::simulateMissResponse(MissResponseEvent* ev, uint64_t cycle, MissStartEvent* mse) {
    profMissRespLat.inc(cycle - mse->startCycle);
    ev->done(cycle);
}

void TimingCache::simulateMissWriteback(MissWritebackEvent* ev, uint64_t cycle, MissStartEvent* mse) {
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) { //success, release MSHR
        assert(activeMisses);
        profMissLat.inc(cycle - mse->startCycle);
        activeMisses--;
        profOccHist.transition(activeMisses, lookupCycle);
        if (!pendingQueue.empty()) {
            //info("XXX %ld elems in pending queue", pendingQueue.size());
            for (TimingEvent* qev : pendingQueue) {
                qev->requeue(cycle+1);
            }
            pendingQueue.clear();
        }
        ev->done(cycle);
    } else {
        ev->requeue(cycle+1);
    }
}

void TimingCache::simulateReplAccess(ReplAccessEvent* ev, uint64_t cycle) {
    assert(ev->accsLeft);
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) {
        ev->accsLeft--;
        if (!ev->accsLeft) {
            ev->done(cycle);
        } else {
            ev->requeue(cycle+1);
        }
    } else {
        ev->requeue(cycle+1);
    }
}

