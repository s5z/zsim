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

#include "timing_event.h"
#include <sstream>
#include <typeinfo>
#include "contention_sim.h"
#include "zsim.h"

/* TimingEvent */

void TimingEvent::parentDone(uint64_t startCycle) {
    cycle = MAX(cycle, startCycle);
    assert(numParents);
    numParents--;
    if (!numParents) {
        assert(state == EV_NONE);
        state = EV_QUEUED;
        zinfo->contentionSim->enqueue(this, cycle+preDelay);
    }
}

void TimingEvent::queue(uint64_t nextCycle) {
    assert(state == EV_NONE && numParents == 0);
    state = EV_QUEUED;
    zinfo->contentionSim->enqueueSynced(this, nextCycle);
}

void TimingEvent::requeue(uint64_t nextCycle) {
    assert(numParents == 0);
    assert(state == EV_RUNNING || state == EV_HELD);
    state = EV_QUEUED;
    zinfo->contentionSim->enqueue(this, nextCycle);
}

void TimingEvent::produceCrossings(EventRecorder* evRec) {
    assert(domain != -1);
    //assert(dynamic_cast<CrossingEvent*>(this) == nullptr); //careful, expensive...
    auto pcLambda = [this, evRec](TimingEvent** childPtr) {
        TimingEvent* c = *childPtr;
        if (c->domain != domain) *childPtr = handleCrossing(c, evRec, true);
        c->produceCrossings(evRec);
    };
    visitChildren< decltype(pcLambda) > (pcLambda);
}

TimingEvent* TimingEvent::handleCrossing(TimingEvent* childEv, EventRecorder* evRec, bool unlinkChild) {
    if (unlinkChild) {
        assert_msg(childEv->numParents, "child has %d parents, nonzero expected", childEv->numParents);
        childEv->numParents--;
    }
    assert_msg(minStartCycle != ((uint64_t)-1L), "Crossing domain (%d -> %d), but parent's minStartCycle is not set (my class: %s)",
            domain, childEv->domain, typeid(*this).name()); //we can only handle a crossing if this has been set
    CrossingEvent* xe = new (evRec) CrossingEvent(this, childEv, minStartCycle+postDelay, evRec);
    return xe->getSrcDomainEvent();
}

void TimingEvent::checkDomain(TimingEvent* ch) {
    //dynamic_cast takes a while, so let's just punt on this now that it's correct
    //assert(domain == ch->domain || dynamic_cast<CrossingEvent*>(ch));
}


/* CrossingEvent */

CrossingEvent::CrossingEvent(TimingEvent* parent, TimingEvent* child, uint64_t _minStartCycle, EventRecorder* _evRec)
    : TimingEvent(0, 0, child->domain), cpe(this, parent->domain)
{
    assert(parent->domain != child->domain);
    parentEv = parent;
    evRec = _evRec;
    srcDomain = parent->domain;
    assert(srcDomain >= 0);
    simCount = 0;
    called = false;
    addChild(child, evRec);
    doneCycle = 0;

    //Delay stealing
    preSlack = parent->postDelay;
    postSlack = child->preDelay;

    //assert(preSlack > 0);
    if (preSlack == 0) {
        //warn("%ld: No preSlack", _minStartCycle);
        preSlack = 1;
        _minStartCycle++;
    }

    minStartCycle = _minStartCycle;
    origStartCycle = minStartCycle - evRec->getGapCycles();
    //queue(MAX(zinfo->contentionSim->getLastLimit(), minStartCycle)); //this initial queue always works --- 0 parents
    //childCrossing = nullptr;
    zinfo->contentionSim->enqueueCrossing(this, MAX(zinfo->contentionSim->getLastLimit(), minStartCycle), evRec->getSourceId(), srcDomain, child->domain, evRec);
}

void CrossingEvent::markSrcEventDone(uint64_t cycle) {
    assert(!called);
    //Sanity check
    srcDomainCycleAtDone = zinfo->contentionSim->getCurCycle(srcDomain);
    assert(cycle >= srcDomainCycleAtDone);
    //NOTE: No fencing needed; TSO ensures writes to doneCycle and callled happen in order.
    doneCycle = cycle;
    called = true;
    //Also, no fencing needed after.
}

void CrossingEvent::parentDone(uint64_t startCycle) {
    //We don't pad chained crossings with delays; just make sure we don't enqueue ourselves before minStartCycle
    uint64_t cycle = MAX(startCycle, minStartCycle);
    if (called) {
        if (doneCycle < cycle) {
            //warn("Crossing enqueued too late, doneCycle %ld startCycle %ld minStartCycle %ld cycle %ld", doneCycle, startCycle, minStartCycle, cycle);
            doneCycle = cycle;
        }
        //assert_msg(doneCycle >= cycle, "Crossing enqueued too late, doneCycle %ld startCycle %ld minStartCycle %ld cycle %ld", doneCycle, startCycle, minStartCycle, cycle);
    }
    TimingEvent::parentDone(cycle);
}

void CrossingEvent::simulate(uint64_t simCycle) {
    if (!called) {
        uint64_t curSrcCycle = zinfo->contentionSim->getCurCycle(srcDomain) + preSlack + postSlack;
        //uint64_t coreRelCycle = 0; //evRec->getSlack(origStartCycle) + postSlack; //note we do not add preDelay, because minStartCycle already has it
        uint64_t coreRelCycle = evRec->getSlack(origStartCycle) + postSlack; //note we do not add preDelay, because minStartCycle already has it
        uint64_t nextCycle = MAX(coreRelCycle, MAX(curSrcCycle, simCycle));

        __sync_synchronize(); //not needed --- these are all volatile, and by TSO, if we see a cycle > doneCycle, by force we must see doneCycle set
        if (!called) { //have to check again, AFTER reading the cycles! Otherwise, we have a race
            zinfo->contentionSim->setPrio(domain, (nextCycle == simCycle)? 1 : 2);

#if PROFILE_CROSSINGS
            simCount++;
#endif
            numParents = 0; //HACK
            requeue(nextCycle);
            return;
        }
    }

    //Runs if called
    //assert_msg(simCycle <= doneCycle+preSlack+postSlack+1, "simCycle %ld doneCycle %ld, preSlack %d postSlack %d simCount %ld child %s", simCycle, doneCycle, preSlack, postSlack, simCount, typeid(*child).name());
    zinfo->contentionSim->setPrio(domain, 0);

#if PROFILE_CROSSINGS
    zinfo->contentionSim->profileCrossing(srcDomain, domain, simCount);
#endif

    uint64_t dCycle = MAX(simCycle, doneCycle);
    //info("Crossing %d->%d done %ld", srcDomain, domain, dCycle);
    done(dCycle);
}

