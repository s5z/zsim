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

#ifndef EVENT_QUEUE_H_
#define EVENT_QUEUE_H_

#include <stdint.h>
#include "g_std/g_multimap.h"
#include "galloc.h"
#include "zsim.h"

class Event : public GlobAlloc {
    protected:
        uint64_t period;

    public:
        explicit Event(uint64_t _period) : period(_period) {} //period == 0 events are one-shot
        uint64_t getPeriod() const {return period;}
        virtual void callback()=0;
};

/* Adjusts period to fire on the first phase following the target. Sets exponentially decreasing periods,
 * so even if maxRate is horribly overestimated, it should have a very small cost (but there's room for
 * optimization if this becomes an issue).
 */
template<typename G, typename F>
class AdaptiveEvent : public Event {
    private:
        G get;
        F fire;
        uint64_t target;
        uint64_t maxRate;

    public:
        AdaptiveEvent(G _get, F _fire, uint64_t _start, uint64_t _target, uint64_t _maxRate) : Event(0), get(_get), fire(_fire), target(_target), maxRate(_maxRate) {
            assert(target >= _start);
            period = (target - _start)/maxRate;
            if (!period) period = 1;
        }

        // This will fire a bunch of times, we adjust the period to get the exact phase
        // Gets called from an arbitrary process, cannot touch any proc-local state (including FFI)
        void callback() {
            uint64_t cur = get();
            if (cur >= target) {
                assert(cur - target <= maxRate); //otherwise, maxRate was wrong...
                fire();
                period = 0; //event queue will dispose of us
            } else {
                period = (target - cur)/maxRate;
                if (period == 0) period = 1;
            }
        }
};

template <typename G, typename F>
AdaptiveEvent<G, F>* makeAdaptiveEvent(G get, F fire, uint64_t start, uint64_t target, uint64_t maxRate) {
    return new AdaptiveEvent<G, F>(get, fire, start, target, maxRate);
}


class EventQueue : public GlobAlloc {
    private:
        g_multimap<uint64_t, Event*> evMap;
        lock_t qLock;

    public:
        EventQueue() { futex_init(&qLock); }

        void tick() {
            futex_lock(&qLock);
            uint64_t curPhase = zinfo->numPhases;
            g_multimap<uint64_t, Event*>::iterator it = evMap.begin();
            while (it != evMap.end() && it->first <= curPhase) {
                if (unlikely(it->first != curPhase)) panic("First event should have ticked on phase %ld, this is %ld", it->first, curPhase);
                //if (it->first != curPhase) warn("First event should have ticked on phase %ld, this is %ld", it->first, curPhase);
                Event* ev = it->second;
                evMap.erase(it);
                ev->callback(); //NOTE: Callback cannot call insert(), will deadlock (could use recursive locks if needed)
                if (ev->getPeriod()) {
                    evMap.insert(std::pair<uint64_t, Event*>(curPhase + ev->getPeriod(), ev));
                } else {
                    delete ev;
                }
                it = evMap.begin();
            }
            futex_unlock(&qLock);
        }

        void insert(Event* ev, int64_t startDelay = -1) {
            futex_lock(&qLock);
            uint64_t curPhase = zinfo->numPhases;
            uint64_t eventPhase = (startDelay == -1)? (curPhase + ev->getPeriod()) : (curPhase + startDelay);
            assert(eventPhase >= curPhase);
            evMap.insert(std::pair<uint64_t, Event*>(eventPhase, ev));
            futex_unlock(&qLock);
        }
};

#endif  // EVENT_QUEUE_H_
