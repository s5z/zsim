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

#ifndef TICK_EVENT_H_
#define TICK_EVENT_H_

#include "contention_sim.h"
#include "timing_event.h"
#include "zsim.h"

//FIXME: Rearchitect this SENSIBLY
template <class T>
class TickEvent : public TimingEvent, public GlobAlloc { //this one should be allocated from glob mem
    private:
        T* obj;
        bool active;

    public:
        TickEvent(T* _obj, int32_t domain) : TimingEvent(0, 0, domain), obj(_obj), active(false) {
            setMinStartCycle(0);
        }

        void parentDone(uint64_t startCycle) {
            panic("This is queued directly");
        }

        void queue(uint64_t startCycle) {
            assert(!active);
            active = true;
            zinfo->contentionSim->enqueueSynced(this, startCycle);
        }

        void simulate(uint64_t startCycle) {
            uint32_t delay = obj->tick(startCycle);
            if (delay) {
                requeue(startCycle+delay);
            } else {
                active = false;
            }
        }

        using GlobAlloc::operator new; //grrrrrrrrr
        using GlobAlloc::operator delete;
};

#endif  // TICK_EVENT_H_
