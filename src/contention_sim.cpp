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

#include "contention_sim.h"
#include <algorithm>
#include <queue>
#include <sstream>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include "log.h"
#include "ooo_core.h"
#include "timing_core.h"
#include "timing_event.h"
#include "zsim.h"

//Set to 1 to produce a post-mortem analysis log
#define POST_MORTEM 0
//#define POST_MORTEM 1

bool ContentionSim::CompareEvents::operator()(TimingEvent* lhs, TimingEvent* rhs) const {
    return lhs->cycle > rhs->cycle;
}

bool ContentionSim::CompareDomains::operator()(DomainData* d1, DomainData* d2) const {
    uint64_t v1 = d1->queuePrio;
    uint64_t v2 = d2->queuePrio;
    return (v1 > v2);
}


void ContentionSim::SimThreadTrampoline(void* arg) {
    ContentionSim* csim = static_cast<ContentionSim*>(arg);
    uint32_t thid = __sync_fetch_and_add(&csim->threadTicket, 1);
    csim->simThreadLoop(thid);
}

ContentionSim::ContentionSim(uint32_t _numDomains, uint32_t _numSimThreads) {
    numDomains = _numDomains;
    numSimThreads = _numSimThreads;
    threadsDone = 0;
    limit = 0;
    lastLimit = 0;
    inCSim = false;

    domains = gm_calloc<DomainData>(numDomains);
    simThreads = gm_calloc<SimThreadData>(numSimThreads);

    for (uint32_t i = 0; i < numDomains; i++) {
        new (&domains[i].pq) PrioQueue<TimingEvent, PQ_BLOCKS>();
        domains[i].curCycle = 0;
        futex_init(&domains[i].pqLock);
    }

    if ((numDomains % numSimThreads) != 0) panic("numDomains(%d) must be a multiple of numSimThreads(%d) for now", numDomains, numSimThreads);

    for (uint32_t i = 0; i < numSimThreads; i++) {
        futex_init(&simThreads[i].wakeLock);
        futex_lock(&simThreads[i].wakeLock); //starts locked, so first actual call to lock blocks
        simThreads[i].firstDomain = i*numDomains/numSimThreads;
        simThreads[i].supDomain = (i+1)*numDomains/numSimThreads;
    }

    futex_init(&waitLock);
    futex_lock(&waitLock); //wait lock must also start locked

    //futex_init(&testLock);
    futex_init(&postMortemLock);

    //Launch domain simulation threads
    threadTicket = 0;
    __sync_synchronize();
    for (uint32_t i = 0; i < numSimThreads; i++) {
        PIN_SpawnInternalThread(SimThreadTrampoline, this, 1024*1024, nullptr);
    }

    lastCrossing = gm_calloc<CrossingEventInfo>(numDomains*numDomains*MAX_THREADS); //TODO: refine... this allocs too much
}

void ContentionSim::postInit() {
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        TimingCore* tcore = dynamic_cast<TimingCore*>(zinfo->cores[i]);
        if (tcore) {
            skipContention = false;
            return;
        }
        OOOCore* ocore = dynamic_cast<OOOCore*>(zinfo->cores[i]);
        if (ocore) {
            skipContention = false;
            return;
        }
    }
    skipContention = true;
}

void ContentionSim::initStats(AggregateStat* parentStat) {
    AggregateStat* objStat = new AggregateStat(false);
    objStat->init("contention", "Contention simulation stats");
    for (uint32_t i = 0; i < numDomains; i++) {
        std::stringstream ss;
        ss << "domain-" << i;
        AggregateStat* domStat = new AggregateStat();
        domStat->init(gm_strdup(ss.str().c_str()), "Domain stats");
#if PROFILE_CROSSINGS
        new (&domains[i].profIncomingCrossings) VectorCounter();
        new (&domains[i].profIncomingCrossingSims) VectorCounter();
        new (&domains[i].profIncomingCrossingHist) VectorCounter();
        domains[i].profIncomingCrossings.init("ixe", "Incoming crossing events", numDomains);
        domains[i].profIncomingCrossingSims.init("ixs", "Incoming crossings simulated but held", numDomains);
        domains[i].profIncomingCrossingHist.init("ixh", "Incoming crossings held count histogram", 33 /*32 means >31*/);
        domStat->append(&domains[i].profIncomingCrossings);
        domStat->append(&domains[i].profIncomingCrossingSims);
        domStat->append(&domains[i].profIncomingCrossingHist);
#endif
        new (&domains[i].profTime) ClockStat();
        domains[i].profTime.init("time", "Weave simulation time");
        domStat->append(&domains[i].profTime);
        objStat->append(domStat);
    }
    parentStat->append(objStat);
}

void ContentionSim::simulatePhase(uint64_t limit) {
    if (skipContention) return; //fastpath when there are no cores to simulate

    this->limit = limit;
    assert(limit >= lastLimit);

    //info("simulatePhase limit %ld", limit);
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        TimingCore* tcore = dynamic_cast<TimingCore*>(zinfo->cores[i]);
        if (tcore) tcore->cSimStart();
        OOOCore* ocore = dynamic_cast<OOOCore*>(zinfo->cores[i]);
        if (ocore) ocore->cSimStart();
    }

    inCSim = true;
    __sync_synchronize();

    //Wake up sim threads
    for (uint32_t i = 0; i < numSimThreads; i++) {
        futex_unlock(&simThreads[i].wakeLock);
    }

    //Sleep until phase is simulated
    futex_lock_nospin(&waitLock);

    inCSim = false;
    __sync_synchronize();

    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        TimingCore* tcore = dynamic_cast<TimingCore*>(zinfo->cores[i]);
        if (tcore) tcore->cSimEnd();
        OOOCore* ocore = dynamic_cast<OOOCore*>(zinfo->cores[i]);
        if (ocore) ocore->cSimEnd();
    }

    lastLimit = limit;
    __sync_synchronize();
}

void ContentionSim::enqueue(TimingEvent* ev, uint64_t cycle) {
    assert(inCSim);
    assert(ev);
    assert_msg(cycle >= lastLimit, "Enqueued event before last limit! cycle %ld min %ld", cycle, lastLimit);
    //Hacky, but helpful to chase events scheduled too far ahead due to bugs (e.g., cycle -1). We should probably formalize this a bit more
    assert_msg(cycle < lastLimit+10*zinfo->phaseLength+1000000, "Queued event too far into the future, cycle %ld lastLimit %ld", cycle, lastLimit);

    assert_msg(cycle >= domains[ev->domain].curCycle, "Queued event goes back in time, cycle %ld curCycle %ld", cycle, domains[ev->domain].curCycle);
    ev->privCycle = cycle;
    assert(ev->numParents == 0);
    assert(ev->domain != -1);
    assert(ev->domain < (int32_t)numDomains);

    domains[ev->domain].pq.enqueue(ev, cycle);
}

void ContentionSim::enqueueSynced(TimingEvent* ev, uint64_t cycle) {
    assert(!inCSim);
    assert(ev && ev->domain != -1);
    assert(ev->domain < (int32_t)numDomains);
    uint32_t domain = ev->domain;

    futex_lock(&domains[domain].pqLock);

    assert_msg(cycle >= lastLimit, "Enqueued (synced) event before last limit! cycle %ld min %ld", cycle, lastLimit);
    //Hacky, but helpful to chase events scheduled too far ahead due to bugs (e.g., cycle -1). We should probably formalize this a bit more
    assert_msg(cycle < lastLimit+10*zinfo->phaseLength+10000, "Queued  (synced) event too far into the future, cycle %ld lastLimit %ld", cycle, lastLimit);
    ev->privCycle = cycle;
    assert(ev->numParents == 0);
    domains[ev->domain].pq.enqueue(ev, cycle);

    futex_unlock(&domains[domain].pqLock);
}

void ContentionSim::enqueueCrossing(CrossingEvent* ev, uint64_t cycle, uint32_t srcId, uint32_t srcDomain, uint32_t dstDomain, EventRecorder* evRec) {
    CrossingStack& cs = evRec->getCrossingStack();
    bool isFirst = cs.empty();
    bool isResp = false;
    CrossingEvent* req = nullptr;
    if (!isFirst) {
        CrossingEvent* b = cs.back();
        if (b->srcDomain == (uint32_t)ev->domain && (uint32_t)b->domain == ev->srcDomain) {
            //info("XXX response identified %d->%d", ev->srcDomain, ev->domain);
            isResp = true;
            req = b;
        }
    }

    if (!isResp) cs.push_back(ev);
    else cs.pop_back();

    if (isResp) {
        req->parentEv->addChild(ev, evRec);
    } else {
        CrossingEventInfo* last = &lastCrossing[(srcId*numDomains + srcDomain)*numDomains + dstDomain];
        uint64_t srcDomCycle = domains[srcDomain].curCycle;
        if (last->cycle > srcDomCycle && last->cycle <= cycle) { //NOTE: With the OOO model, last->cycle > cycle is now possible, since requests are issued in instruction order -> ooo
            //Chain to previous req
            assert_msg(last->cycle <= cycle, "last->cycle (%ld) > cycle (%ld)", last->cycle, cycle);
            last->ev->addChild(ev, evRec);
        } else {
            //We can't queue --- queue directly (synced, we're in phase 1)
            assert(cycle >= srcDomCycle);
            //info("Queuing xing %ld %ld (lst eve too old at cycle %ld)", cycle, srcDomCycle, last->cycle);
            enqueueSynced(ev, cycle);
        }
        //Store this one as the last req
        last->cycle = cycle;
        last->ev = ev;
    }
}

void ContentionSim::simThreadLoop(uint32_t thid) {
    info("Started contention simulation thread %d", thid);
#if 0
    //Pin
    uint32_t nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    //CPU_SET(domain % nprocs, cpuset); //basically no difference
    //CPU_SET(0, cpuset); CPU_SET(1, cpuset); CPU_SET(2, cpuset); CPU_SET(3, cpuset); //don't use hyperthreads; confuses the scheduler to no end, does worse
    CPU_SET(thid % nprocs, &cpuset); CPU_SET((thid % nprocs) + (nprocs/2), &cpuset); //pin to a single core but can use both its hyperthreads, works best (~20% gain)
    //TODO: Must make this optional, or multiple simulations/machine will work horribly
    int r = sched_setaffinity(0 /*calling thread, equiv to syscall(SYS_gettid)*/, sizeof(cpuset), &cpuset);
    assert_msg(r == 0, "sched_setaffinity failed (%d)", r);
#endif
    while (true) {
        futex_lock_nospin(&simThreads[thid].wakeLock);

        if (terminate) {
            break;
        }

        //info("%d --- phase start", domain);
        simulatePhaseThread(thid);
        //info("%d --- phase end", domain);

        uint32_t val = __sync_add_and_fetch(&threadsDone, 1);
        if (val == numSimThreads) {
            threadsDone = 0;
            futex_unlock(&waitLock); //unblock caller
        }
    }
    info("Finished contention simulation thread %d", thid);
}

void ContentionSim::simulatePhaseThread(uint32_t thid) {
    uint32_t thDomains = simThreads[thid].supDomain - simThreads[thid].firstDomain;
    uint32_t numFinished = 0;

    if (thDomains == 1) {
        DomainData& domain = domains[simThreads[thid].firstDomain];
        domain.profTime.start();
        PrioQueue<TimingEvent, PQ_BLOCKS>& pq = domain.pq;
        while (pq.size() && pq.firstCycle() < limit) {
            uint64_t domCycle = domain.curCycle;
            uint64_t cycle;
            TimingEvent* te = pq.dequeue(cycle);
            assert(cycle >= domCycle);
            if (cycle != domCycle) {
                domCycle = cycle;
                domain.curCycle = cycle;
            }
            te->run(cycle);
            uint64_t newCycle = pq.size()? pq.firstCycle() : limit;
            assert(newCycle >= domCycle);
            if (newCycle != domCycle) domain.curCycle = newCycle;
#if POST_MORTEM
            simThreads[thid].logVec.push_back(std::make_pair(cycle, te));
#endif
        }
        domain.curCycle = limit;
        domain.profTime.end();

#if POST_MORTEM
        //Post-mortem
        if (limit % 10000000 == 0)  {
            futex_lock(&postMortemLock); //serialize output
            uint32_t uniqueEvs = 0;
            std::unordered_map<TimingEvent*, std::string> evsSeen;
            for (std::pair<uint64_t, TimingEvent*> p : simThreads[thid].logVec) {
                uint64_t cycle = p.first;
                TimingEvent* te = p.second;
                std::string desc = evsSeen[te];
                if (desc == "") { //non-existnt
                    std::stringstream ss;
                    ss << uniqueEvs << " " << typeid(*te).name();
                    CrossingEvent* ce = dynamic_cast<CrossingEvent*>(te);
                    if (ce) {
                        ss << " slack " << (ce->preSlack + ce->postSlack) << " osc " << ce->origStartCycle << " cnt " << ce->simCount;
                    }

                    evsSeen[te] = ss.str();
                    uniqueEvs++;
                    desc = ss.str();
                }
                info("[%d] %ld %s", thid, cycle, desc.c_str());
            }
            futex_unlock(&postMortemLock);
        }
        simThreads[thid].logVec.clear();
#endif

    } else {
        //info("XXX %d / %d %d %d", thid, thDomains, simThreads[thid].supDomain, simThreads[thid].firstDomain);

        std::priority_queue<DomainData*, std::vector<DomainData*>, CompareDomains> domPq;
        for (uint32_t i = simThreads[thid].firstDomain; i < simThreads[thid].supDomain; i++) {
            domPq.push(&domains[i]);
        }

        std::vector<DomainData*> sq1;
        std::vector<DomainData*> sq2;

        std::vector<DomainData*>& stalledQueue = sq1;
        std::vector<DomainData*>& nextStalledQueue = sq2;

        while (numFinished < thDomains) {
            while (domPq.size()) {
                DomainData* domain = domPq.top();
                domPq.pop();
                PrioQueue<TimingEvent, PQ_BLOCKS>& pq = domain->pq;
                if (!pq.size() || pq.firstCycle() > limit) {
                    numFinished++;
                    domain->curCycle = limit;
                } else {
                    //info("YYY %d %ld %ld %d", numFinished, domPq.size(), domain->curCycle, domain->prio);
                    uint64_t cycle;
                    TimingEvent* te = pq.dequeue(cycle);
                    //uint64_t nextCycle = pq.size()? pq.firstCycle() : cycle;
                    if (cycle != domain->curCycle) domain->curCycle = cycle;
                    te->run(cycle);
                    domain->curCycle = pq.size()? pq.firstCycle() : limit;
                    domain->queuePrio = domain->curCycle;
                    if (domain->prio == 0) domPq.push(domain);
                    else stalledQueue.push_back(domain);
                }
            }

            while (stalledQueue.size()) {
                DomainData* domain = stalledQueue.back();
                stalledQueue.pop_back();
                PrioQueue<TimingEvent, PQ_BLOCKS>& pq = domain->pq;
                if (!pq.size() || pq.firstCycle() > limit) {
                    numFinished++;
                    domain->curCycle = limit;
                } else {
                    //info("SSS %d %ld %ld", numFinished, stalledQueue.size(), domain->curCycle);
                    uint64_t cycle;
                    TimingEvent* te = pq.dequeue(cycle);
                    if (cycle != domain->curCycle) domain->curCycle = cycle;
                    te->state = EV_RUNNING;
                    te->simulate(cycle);
                    domain->curCycle = pq.size()? pq.firstCycle() : limit;
                    domain->queuePrio = domain->curCycle;
                    if (domain->prio == 0) domPq.push(domain);
                    else nextStalledQueue.push_back(domain);
                }
                if (domPq.size()) break;
            }
            if (!stalledQueue.size()) std::swap(stalledQueue, nextStalledQueue);
        }
    }

    //info("Phase done");
    __sync_synchronize();
}

void ContentionSim::finish() {
    assert(!terminate);
    terminate = true;
    __sync_synchronize();
}

