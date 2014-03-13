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

#include "core_recorder.h"
#include "timing_event.h"
#include "zsim.h"

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

class TimingCoreEvent : public TimingEvent {
    private:
        uint64_t origStartCycle;
        uint64_t startCycle;
        CoreRecorder* cRec;

    public:
        //NOTE: Only the first TimingCoreEvent after a thread join needs to be in a domain, hence the default parameter. Because these are inherently sequential and have a fixed delay, subsequent events can inherit the parent's domain, reducing domain xings and improving slack and performance
        TimingCoreEvent(uint64_t _delay, uint64_t _origStartCycle, CoreRecorder* _cRec, int32_t domain = -1) : TimingEvent(0, _delay, domain), origStartCycle(_origStartCycle), cRec(_cRec) {}

        void simulate(uint64_t _startCycle) {
            startCycle = _startCycle;
            cRec->reportEventSimulated(this);
            done(startCycle);
        }

        friend class CoreRecorder;
};

CoreRecorder::CoreRecorder(uint32_t _domain, g_string& _name)
    : domain(_domain), name(_name + "-rec")
{
    prevRespEvent = NULL;
    state = HALTED;
    gapCycles = 0;
    eventRecorder.setGapCycles(gapCycles);

    lastUnhaltedCycle = 0;
    totalGapCycles = 0;
    totalHaltedCycles = 0;
}


uint64_t CoreRecorder::notifyJoin(uint64_t curCycle) {
    if (state == HALTED) {
        assert(!prevRespEvent);
        curCycle = zinfo->globPhaseCycles; //start at beginning of the phase

        totalGapCycles += gapCycles;
        gapCycles = 0;
        eventRecorder.setGapCycles(gapCycles);
        assert(lastUnhaltedCycle <= curCycle);
        totalHaltedCycles += curCycle - lastUnhaltedCycle;

        prevRespEvent = new (eventRecorder) TimingCoreEvent(0, curCycle, this, domain);
        prevRespCycle = curCycle;
        prevRespEvent->setMinStartCycle(curCycle);
        prevRespEvent->queue(curCycle);
        eventRecorder.setStartSlack(0);
        DEBUG_MSG("[%s] Joined, was HALTED, curCycle %ld halted %ld", name.c_str(), curCycle, totalHaltedCycles);
    } else if (state == DRAINING) {
        assert(curCycle >= zinfo->globPhaseCycles); //should not have gone out of sync...
        DEBUG_MSG("[%s] Joined, was DRAINING, curCycle %ld", name.c_str(), curCycle);
    } else {
        panic("[%s] Invalid state %d on join()", name.c_str(), state);
    }

    //Common actions
    state = RUNNING;
    return curCycle;
}


void CoreRecorder::notifyLeave(uint64_t curCycle) {
    assert(state == RUNNING);
    state = DRAINING;
    assert(prevRespEvent);
    //Taper off the event
    // Cover delay to curCycle
    uint64_t delay = curCycle - prevRespCycle;
    TimingCoreEvent* ev = new (eventRecorder) TimingCoreEvent(delay, prevRespCycle-gapCycles, this);
    ev->setMinStartCycle(prevRespCycle);
    prevRespEvent->addChild(ev, eventRecorder);
    prevRespEvent = ev;
    prevRespCycle = curCycle;

    // Then put a zero-delay event that finishes the sequence
    ev = new (eventRecorder) TimingCoreEvent(0, prevRespCycle-gapCycles, this);
    ev->setMinStartCycle(prevRespCycle);
    prevRespEvent->addChild(ev, eventRecorder);
    prevRespEvent = ev;

    DEBUG_MSG("[%s] Left, curCycle %ld", name.c_str(), curCycle);
}

void CoreRecorder::recordAccess(uint64_t startCycle) {
    assert(eventRecorder.numRecords() <= 2);
    TimingRecord tr = eventRecorder.getRecord(0);
    TimingEvent* origPrevResp = prevRespEvent;

    if (tr.type == PUTS || tr.type == PUTX) {
        //info("Handling PUT+GET");
        assert(eventRecorder.numRecords() == 2);
        TimingRecord tr1 = eventRecorder.getRecord(1);
        assert(tr1.type == GETX || tr1.type == GETS);
        assert(startCycle >= prevRespCycle);
        assert(tr1.reqCycle >= startCycle);
        assert(tr.reqCycle >= startCycle);

        uint64_t delay = startCycle - prevRespCycle;
        TimingCoreEvent* ev = new (eventRecorder) TimingCoreEvent(delay, prevRespCycle - gapCycles, this);
        ev->setMinStartCycle(prevRespCycle);
        prevRespEvent->addChild(ev, eventRecorder);
        DelayEvent* dr = new (eventRecorder) DelayEvent(tr.reqCycle-startCycle);
        DelayEvent* dr1 = new (eventRecorder) DelayEvent(tr1.reqCycle-startCycle);
        dr->setMinStartCycle(startCycle);
        dr1->setMinStartCycle(startCycle);
        ev->addChild(dr, eventRecorder)->addChild(tr.startEvent, eventRecorder);
        ev->addChild(dr1, eventRecorder)->addChild(tr1.startEvent, eventRecorder);

        //tr.endEvent not linked to anything
        prevRespEvent = tr1.endEvent;
        prevRespCycle = tr1.respCycle;

    } else {
        //info("Handling single GET");
        assert(tr.type == GETX || tr.type == GETS);
        assert(eventRecorder.numRecords() == 1);
        uint64_t delay = tr.reqCycle - prevRespCycle;
        TimingEvent* ev = new (eventRecorder) TimingCoreEvent(delay, prevRespCycle - gapCycles, this);
        ev->setMinStartCycle(prevRespCycle);
        prevRespEvent->addChild(ev, eventRecorder)->addChild(tr.startEvent, eventRecorder);
        prevRespEvent = tr.endEvent;
        prevRespCycle = tr.respCycle;
    }

    origPrevResp->produceCrossings(&eventRecorder);
    eventRecorder.getCrossingStack().clear();
    eventRecorder.clearRecords();
}


uint64_t CoreRecorder::cSimStart(uint64_t curCycle) {
    if (state == HALTED) return curCycle; //nothing to do

    DEBUG_MSG("[%s] Cycle %ld cSimStart %d", name.c_str(), curCycle, state);

    uint64_t nextPhaseCycle = zinfo->globPhaseCycles + zinfo->phaseLength;

    // If needed, bring us to the current cycle
    if (state == RUNNING) {
        assert(curCycle >= nextPhaseCycle);

        // Cover delay to curCycle
        if (prevRespCycle < curCycle) {
            uint64_t delay = curCycle - prevRespCycle;
            TimingCoreEvent* ev = new (eventRecorder) TimingCoreEvent(delay, prevRespCycle-gapCycles, this);
            ev->setMinStartCycle(prevRespCycle);
            prevRespEvent->addChild(ev, eventRecorder);
            prevRespEvent = ev;
            prevRespCycle = curCycle;
        }

        // Add an event that STARTS in the next phase, so it never gets simulated on the current phase
        TimingCoreEvent* ev = new (eventRecorder) TimingCoreEvent(0, prevRespCycle-gapCycles, this);
        ev->setMinStartCycle(prevRespCycle);
        prevRespEvent->addChild(ev, eventRecorder);
        prevRespEvent = ev;
    } else if (state == DRAINING) { // add no event --- that's how we detect we're done draining
         if (curCycle < nextPhaseCycle) curCycle = nextPhaseCycle; // bring cycle up
    }
    return curCycle;
}

uint64_t CoreRecorder::cSimEnd(uint64_t curCycle) {
    if (state == HALTED) return curCycle; //nothing to do

    DEBUG_MSG("[%s] Cycle %ld done state %d", name.c_str(), curCycle, state);

    assert(lastEventSimulated);

    // Adjust curCycle to account for contention simulation delay

    // In our current clock, when did the last event start (1) before contention simulation, and (2) after contention simulation
    uint64_t lastEvCycle1 = lastEventSimulated->origStartCycle + gapCycles; //we add gapCycles because origStartCycle is in zll clocks
    uint64_t lastEvCycle2 = lastEventSimulated->startCycle;

    assert(lastEvCycle1 <= curCycle);
    assert_msg(lastEvCycle2 <= curCycle, "[%s] lec2 %ld cc %ld, state %d", name.c_str(), lastEvCycle2, curCycle, state);
    if (unlikely(lastEvCycle1 > lastEvCycle2)) panic("[%s] Contention simulation introduced a negative skew, curCycle %ld, lc1 %ld lc2 %ld", name.c_str(), curCycle, lastEvCycle1, lastEvCycle2);

    uint64_t skew = lastEvCycle2 - lastEvCycle1;

    // Skew clock
    // Note that by adding to gapCycles, we keep the zll clock (defined as curCycle - gapCycles) constant.
    // We use the zll clock to translate origStartCycle correctly, even if it's coming from several phases back.
    curCycle += skew;
    gapCycles += skew;
    prevRespCycle += skew;
    eventRecorder.setGapCycles(gapCycles);

    //NOTE: Suppose that we had a really long event, so long that in the next phase, lastEventSimulated is still the same. In this case, skew will be 0, so we do not need to remove it.

    /*DEBUG_MSG*/ //info("[%s] curCycle %ld zllCurCycle %ld lec1 %ld lec2 %ld skew %ld", name.c_str(), curCycle, curCycle-gapCycles, lastEvCycle1, lastEvCycle2, skew);

    /* Advance the recorder: we set the current dead cycle as the last event's cycle,
     * but we mark any live events with some slack (we need the slack to account for events
     * that linger a bit longer).
     */
    //eventRecorder.advance(curCycle + zinfo->phaseLength + 10000 +100000, lastEvCycle2);
    eventRecorder.advance(curCycle - gapCycles + zinfo->phaseLength, lastEventSimulated->origStartCycle);

    if (!lastEventSimulated->getNumChildren()) {
        //if we were RUNNING, the phase would have been tapered off
        assert_msg(state == DRAINING, "[%s] state %d lastEventSimulated %p (startCycle %ld) curCycle %ld", name.c_str(), state, lastEventSimulated, lastEventSimulated->startCycle, curCycle);
        assert(prevRespEvent == lastEventSimulated);
        lastUnhaltedCycle = lastEventSimulated->startCycle; //the taper is a 0-delay event
        assert(lastEventSimulated->getPostDelay() == 0);
        state = HALTED;
        DEBUG_MSG("[%s] lastEventSimulated reached (startCycle %ld), DRAINING -> HALTED", name.c_str(), lastEventSimulated->startCycle);

        lastEventSimulated = NULL;
        prevRespEvent = NULL;
    }
    return curCycle;
}

void CoreRecorder::reportEventSimulated(TimingCoreEvent* ev) {
    lastEventSimulated = ev;
    eventRecorder.setStartSlack(ev->startCycle - ev->origStartCycle);
}

//Stats
uint64_t CoreRecorder::getUnhaltedCycles(uint64_t curCycle) const {
    uint64_t cycle = MAX(curCycle, zinfo->globPhaseCycles);
    uint64_t haltedCycles =  totalHaltedCycles + ((state == HALTED)? (cycle - lastUnhaltedCycle) : 0);
    return cycle - haltedCycles;
}

uint64_t CoreRecorder::getContentionCycles() const {
    return totalGapCycles + gapCycles;
}



