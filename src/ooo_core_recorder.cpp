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

#include "ooo_core_recorder.h"
#include <string>
#include "timing_event.h"
#include "zsim.h"

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

#define TRACE_MSG(args...)
//#define TRACE_MSG(args...) info(args)

class OOOIssueEvent : public TimingEvent {
    private:
        uint64_t zllStartCycle; //minStartCycle - gapCycles, stable across readjustments of gapCycles
        OOOCoreRecorder* cRec;
        uint64_t id;

    public:
        OOOIssueEvent(uint32_t preDelay, uint64_t _zllStartCycle, OOOCoreRecorder* _cRec, int32_t domain = -1) : TimingEvent(preDelay, 0, domain), zllStartCycle(_zllStartCycle), cRec(_cRec) {}

        void simulate(uint64_t startCycle) {
            TRACE_MSG("Issue %ld zllStartCycle %ld startCycle %ld minStartCycle %ld", id, zllStartCycle, startCycle, getMinStartCycle());
            cRec->reportIssueEventSimulated(this, startCycle);
            done(startCycle);
        }

        virtual std::string str() {
            std::string res = "rec: ";
            res += cRec->getName().c_str();
            return res;
        }

        friend class OOOCoreRecorder;
};


class OOODispatchEvent : public TimingEvent {
    private:
        uint64_t zllStartCycle; //minStartCycle - gapCycles, stable across readjustments of gapCycles
        uint64_t id;

    public:
        OOODispatchEvent(uint64_t preDelay, uint64_t _zllStartCycle, int32_t domain = -1) : TimingEvent(preDelay, 0, domain), zllStartCycle(_zllStartCycle) {}

        void simulate(uint64_t startCycle) {
            TRACE_MSG("Dispatch %ld zllStartCycle %ld startCycle %ld minStartCycle %ld", id, zllStartCycle, startCycle, getMinStartCycle());
            done(startCycle);
        }

        friend class OOOCoreRecorder;
};

class OOORespEvent : public TimingEvent {
    private:
        // Now tracked in FutureResponse wrapper, as event may go stale
        //uint64_t zllStartCycle; //minStartCycle - gapCycles, stable across readjustments of gapCycles
        OOOCoreRecorder* cRec;  // used to track whether event has finished
        uint64_t id;

    public:
        OOORespEvent(uint64_t preDelay, OOOCoreRecorder* _cRec, int32_t domain = -1) : TimingEvent(preDelay, 0, domain), cRec(_cRec) {}

        void simulate(uint64_t _startCycle) {
            TRACE_MSG("Resp %ld startCycle %ld minStartCycle %ld", id, startCycle, getMinStartCycle());
            cRec = nullptr;
            done(_startCycle);
        }

        friend class OOOCoreRecorder;
};

//For the futureResponses min-heap
bool OOOCoreRecorder::CompareRespEvents::operator()(const FutureResponse& e1, const FutureResponse& e2) const {
    return (e1.zllStartCycle > e2.zllStartCycle);
}


OOOCoreRecorder::OOOCoreRecorder(uint32_t _domain, g_string& _name)
    : domain(_domain), name(_name + "-rec")
{
    state = HALTED;
    gapCycles = 0;
    eventRecorder.setGapCycles(gapCycles);

    lastUnhaltedCycle = 0;
    totalGapCycles = 0;
    totalHaltedCycles = 0;

    curId = 0;

    lastEvProduced = nullptr;
    lastEvSimulatedZllStartCycle = 0;
    lastEvSimulatedStartCycle = 0;
}


uint64_t OOOCoreRecorder::notifyJoin(uint64_t curCycle) {
    if (state == HALTED) {
        assert(!lastEvProduced);
        curCycle = zinfo->globPhaseCycles; //start at beginning of the phase

        totalGapCycles += gapCycles;
        gapCycles = 0;
        eventRecorder.setGapCycles(gapCycles);
        assert(lastUnhaltedCycle <= curCycle);
        totalHaltedCycles += curCycle - lastUnhaltedCycle;

        lastEvProduced = new (eventRecorder) OOOIssueEvent(0, curCycle - gapCycles, this, domain);
        lastEvProduced->id = curId++;
        lastEvProduced->setMinStartCycle(curCycle);
        lastEvProduced->queue(curCycle);
        eventRecorder.setStartSlack(0);
        DEBUG_MSG("[%s] Joined, was HALTED, curCycle %ld halted %ld", name.c_str(), curCycle, totalHaltedCycles);
    } else if (state == DRAINING) {
        assert(curCycle >= zinfo->globPhaseCycles); //should not have gone out of sync...
        DEBUG_MSG("[%s] Joined, was DRAINING, curCycle %ld", name.c_str(), curCycle);
        assert(lastEvProduced);
        addIssueEvent(curCycle);
    } else {
        panic("[%s] Invalid state %d on join()", name.c_str(), state);
    }

    //Common actions
    state = RUNNING;
    return curCycle;
}

//Properly stitches a previous event against prior events properly
//After the call, lastEvProduced is updated to this event
void OOOCoreRecorder::addIssueEvent(uint64_t evCycle) {
    assert(lastEvProduced);
    uint64_t zllCycle = evCycle - gapCycles;
    assert_msg(zllCycle >= lastEvProduced->zllStartCycle, "zllCycle %ld last %ld", zllCycle, lastEvProduced->zllStartCycle);
    OOOIssueEvent* ev = new (eventRecorder) OOOIssueEvent(0, zllCycle, this, domain);
    ev->id = curId++;
    // 1. Link with prior (<) outstanding responses
    uint64_t maxCycle = 0;
    while (!futureResponses.empty()) {
        FutureResponse firstResp = futureResponses.top();
        if (firstResp.zllStartCycle >= zllCycle) break;
        if (firstResp.ev) {
            TRACE_MSG("linked Issue zll %ld with Resp zll %ld", zllCycle, firstResp.zllStartCycle);
            firstResp.ev->addChild(ev, eventRecorder);
            assert(maxCycle <= firstResp.zllStartCycle);
            assert(firstResp.zllStartCycle >= lastEvProduced->zllStartCycle);
            maxCycle = firstResp.zllStartCycle;
        }  // else this event was already simulated (see cSimEnd)
        futureResponses.pop();
    }

    uint32_t preDelay = maxCycle? ((maxCycle < zllCycle)? (zllCycle - maxCycle) : 0) : 0;
    ev->setPreDelay(preDelay);

    //2. Link with prior issue event
    //We need a delay of at least the min-lat delay to avoid negative skews
    uint32_t issueDelay = zllCycle - lastEvProduced->zllStartCycle - preDelay;
    DelayEvent* dIssue = new (eventRecorder) DelayEvent(issueDelay);
    dIssue->setMinStartCycle(lastEvProduced->getMinStartCycle());
    lastEvProduced->addChild(dIssue, eventRecorder)->addChild(ev, eventRecorder);

    TRACE_MSG("linked Issue zll %ld with prev Issue, delay %d", zllCycle, issueDelay);

    ev->setMinStartCycle(evCycle);
    lastEvProduced = ev;
}

void OOOCoreRecorder::notifyLeave(uint64_t curCycle) {
    assert_msg(state == RUNNING, "invalid state = %d on leave", state);
    state = DRAINING;
    assert(lastEvProduced);
    // Cover delay to curCycle
    uint64_t zllCycle = curCycle - gapCycles;
    assert(zllCycle >= lastEvProduced->zllStartCycle);
    addIssueEvent(curCycle);

    TRACE_MSG("LEAVING, curCycle %ld", curCycle);
    DEBUG_MSG("[%s] Left, curCycle %ld", name.c_str(), curCycle);
}

// See http://stackoverflow.com/questions/1185252/is-there-a-way-to-access-the-underlying-container-of-stl-container-adaptors
template <class T, class S, class C>
S& GetPrioQueueContainer(std::priority_queue<T, S, C>& q) {
    struct PQE : private std::priority_queue<T, S, C> {
        static S& Container(std::priority_queue<T, S, C>& q) {
            return q.*&PQE::c;
        }
    };
    return PQE::Container(q);
}

void OOOCoreRecorder::recordAccess(uint64_t curCycle, uint64_t dispatchCycle, uint64_t respCycle) {
    assert(eventRecorder.hasRecord());
    TimingRecord tr = eventRecorder.popRecord();

    if (IsGet(tr.type)) {
        assert(tr.endEvent);
        //info("Handling GET: curCycle %ld ev(reqCycle %ld respCycle %ld) respCycle %ld", curCycle, tr.reqCycle, tr.respCycle, respCycle);

        addIssueEvent(curCycle);

        //Delay
        DelayEvent* dDisp = new (eventRecorder) DelayEvent(dispatchCycle - curCycle);
        dDisp->setMinStartCycle(curCycle);

        //Dispatch event
        OOODispatchEvent* dispEv = new (eventRecorder) OOODispatchEvent(/*dispatchCycle - curCycle*/ 0, dispatchCycle);
        dispEv->setMinStartCycle(dispatchCycle);
        dispEv->id = curId++;

        uint64_t zllDispatchCycle = dispatchCycle - gapCycles;
        //Traverse min heap, link with preceding resps...
        for (FutureResponse& fr : GetPrioQueueContainer(futureResponses)) {
            if (fr.zllStartCycle < zllDispatchCycle && fr.ev) {
                DelayEvent* dl = new (eventRecorder) DelayEvent(zllDispatchCycle - fr.zllStartCycle);
                fr.ev->addChild(dl, eventRecorder)->addChild(dispEv, eventRecorder);
            }
        }
        //Link request
        DelayEvent* dUp = new (eventRecorder) DelayEvent(tr.reqCycle - dispatchCycle); //TODO: remove, postdelay in dispatch...
        dUp->setMinStartCycle(dispatchCycle);
        lastEvProduced->addChild(dDisp, eventRecorder)->addChild(dispEv, eventRecorder)->addChild(dUp, eventRecorder)->addChild(tr.startEvent, eventRecorder);

        //Link response
        assert(respCycle >= tr.respCycle);
        uint32_t downDelay = respCycle - tr.respCycle;
        uint64_t zllStartCycle = respCycle - gapCycles;
        OOORespEvent* respEvent = new (eventRecorder) OOORespEvent(downDelay, this, domain);
        respEvent->id = curId++;
        respEvent->setMinStartCycle(respCycle);
        tr.endEvent->addChild(respEvent, eventRecorder);
        TRACE_MSG("Adding resp zllCycle %ld delay %ld", respCycle - gapCycles, respCycle-curCycle);
        futureResponses.push({zllStartCycle, respEvent});
    } else {
        //info("Handling PUT: curCycle %ld", curCycle);
        assert(IsPut(tr.type));

        //Link request
        DelayEvent* putUp = new (eventRecorder) DelayEvent(tr.reqCycle-curCycle);
        putUp->setMinStartCycle(curCycle);
        lastEvProduced->addChild(putUp, eventRecorder)->addChild(tr.startEvent, eventRecorder);

        //PUT's endEvent not linked to anything, it's a wback in some cache above and we should not capture it
    }

    // For multi-domain
    lastEvProduced->produceCrossings(&eventRecorder);
    eventRecorder.getCrossingStack().clear();
}


uint64_t OOOCoreRecorder::cSimStart(uint64_t curCycle) {
    if (state == HALTED) return curCycle; //nothing to do

    DEBUG_MSG("[%s] Cycle %ld cSimStart %d", name.c_str(), curCycle, state);

    uint64_t nextPhaseCycle = zinfo->globPhaseCycles + zinfo->phaseLength;

    uint64_t zllCycle = curCycle - gapCycles;
    uint64_t zllNextPhaseCycle = nextPhaseCycle - gapCycles;

    // If needed, bring us to the next phase
    if (state == RUNNING) {
        assert(curCycle > nextPhaseCycle);
        assert(lastEvProduced->zllStartCycle <= zllCycle);

        // Taper phase if it's not already tapered
        if (lastEvProduced->zllStartCycle < zllNextPhaseCycle) {
            addIssueEvent(nextPhaseCycle);
        }
    } else if (state == DRAINING) { // add no event --- that's how we detect we're done draining
        //Drain futureResponses... we could be a bit more exact by doing partial drains,
        //but if the thread has not joined back by the end of phase, chances are this is a long leave
        while (!futureResponses.empty()) futureResponses.pop();
        if (curCycle < nextPhaseCycle) curCycle = nextPhaseCycle; // bring cycle up
    }
    return curCycle;
}

uint64_t OOOCoreRecorder::cSimEnd(uint64_t curCycle) {
    if (state == HALTED) return curCycle; //nothing to do

    DEBUG_MSG("[%s] Cycle %ld done state %d", name.c_str(), curCycle, state);

    // Adjust curCycle to account for contention simulation delay

    // In our current clock, when did the last event start (1) before contention simulation, and (2) after contention simulation
    uint64_t lastEvCycle1 = lastEvSimulatedZllStartCycle + gapCycles; //we add gapCycles because zllStartCycle is in zll clocks
    uint64_t lastEvCycle2 = lastEvSimulatedStartCycle;

    assert(lastEvCycle1 <= curCycle);
    assert_msg(lastEvCycle2 <= curCycle, "[%s] lec2 %ld cc %ld, state %d", name.c_str(), lastEvCycle2, curCycle, state);
    if (unlikely(lastEvCycle1 > lastEvCycle2)) panic("[%s] Contention simulation introduced a negative skew, curCycle %ld, lc1 %ld lc2 %ld, gapCycles %ld", name.c_str(), curCycle, lastEvCycle1, lastEvCycle2, gapCycles);

    uint64_t skew = lastEvCycle2 - lastEvCycle1;

    // Skew clock
    // Note that by adding to gapCycles, we keep the zll clock (defined as curCycle - gapCycles) constant.
    // We use the zll clock to translate origStartCycle correctly, even if it's coming from several phases back.
    curCycle += skew;
    gapCycles += skew;
    eventRecorder.setGapCycles(gapCycles);
    //We deal with all our events in zllCycles, so no need to update any event counts

    //NOTE: Suppose that we had a really long event, so long that in the next phase, lastEvSimulated is still the same. In this case, skew will be 0, so we do not need to remove it.

    DEBUG_MSG("[%s] curCycle %ld zllCurCycle %ld lec1 %ld lec2 %ld skew %ld", name.c_str(), curCycle, curCycle-gapCycles, lastEvCycle1, lastEvCycle2, skew);

    // Remove simulated responses
    // NOTE: This violates use-after-free rules, because we're dereferencing
    // pointers to events that may be done. However, because we're at the end
    // of the weave phase and we check every phase, their space should not have
    // been recycled. Fully solving this can be done if we allow the events to
    // clear their pointers themselves...
    for (FutureResponse& fr : GetPrioQueueContainer(futureResponses)) {
        if (fr.ev && fr.ev->cRec != this) {
            //info("Removed already-simulated response");
            fr.ev = nullptr;
        }
    }

    if (!lastEvProduced) {
        //if we were RUNNING, the phase would have been tapered off
        assert_msg(state == DRAINING, "[%s] state %d lastEvSimulated startCycle %ld curCycle %ld", name.c_str(), state, lastEvSimulatedStartCycle, curCycle);
        lastUnhaltedCycle = lastEvSimulatedStartCycle; //the taper is a 0-delay event
        state = HALTED;
        DEBUG_MSG("[%s] lastEvSimulated reached (startCycle %ld), DRAINING -> HALTED", name.c_str(), lastEvSimulatedStartCycle);
        assert(futureResponses.empty());
        // This works (because we flush on leave()) but would be inaccurate if we called leave() very frequently; now leave() only happens on blocking syscalls though
    }
    return curCycle;
}

void OOOCoreRecorder::reportIssueEventSimulated(OOOIssueEvent* ev, uint64_t startCycle) {
    lastEvSimulatedZllStartCycle = ev->zllStartCycle;
    lastEvSimulatedStartCycle = startCycle;
    if (lastEvProduced == ev) lastEvProduced = nullptr;
    eventRecorder.setStartSlack(startCycle - ev->zllStartCycle);
}

//Stats
uint64_t OOOCoreRecorder::getUnhaltedCycles(uint64_t curCycle) const {
    uint64_t cycle = MAX(curCycle, zinfo->globPhaseCycles);
    uint64_t haltedCycles =  totalHaltedCycles + ((state == HALTED)? (cycle - lastUnhaltedCycle) : 0);
    return cycle - haltedCycles;
}

uint64_t OOOCoreRecorder::getContentionCycles() const {
    return totalGapCycles + gapCycles;
}

