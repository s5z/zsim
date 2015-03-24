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

#ifndef TIMING_EVENT_H_
#define TIMING_EVENT_H_

#include <stdint.h>
#include <string>
#include <typeinfo>
#include "bithacks.h"
#include "event_recorder.h"
#include "galloc.h"

#define TIMING_BLOCK_EVENTS 3
struct TimingEventBlock {
    TimingEvent* events[TIMING_BLOCK_EVENTS];
    TimingEventBlock* next;

    TimingEventBlock() {
        for (uint32_t i = 0; i < TIMING_BLOCK_EVENTS; i++) events[i] = nullptr;
        next = nullptr;
    }

    void* operator new (size_t sz, EventRecorder* evRec) {
        return evRec->alloc(sz);
    }

    void operator delete(void*, size_t) {
        panic("TimingEventBlock::delete should never be called");
    }

    //Placement delete... make ICC happy. This would only fire on an exception
    void operator delete (void* p, EventRecorder* evRec) {
        panic("TimingEventBlock::delete PLACEMENT delete called");
    }

    private:
        void* operator new (size_t);
};

enum EventState {EV_NONE, EV_QUEUED, EV_RUNNING, EV_HELD, EV_DONE};

class CrossingEvent;

class TimingEvent {
    private:
        uint64_t privCycle; //only touched by ContentionSim

    public:
        TimingEvent* next; //used by PrioQueue --- PRIVATE

    private:
        EventState state;
        uint64_t cycle;

        uint64_t minStartCycle;
        union {
            TimingEvent* child;
            TimingEventBlock* children;
        };
        int32_t domain; //-1 if none; if none, it acquires it from the parent. Cannot be a starting event (no parents at enqueue time) and get -1 as domain
        uint32_t numChildren;
        uint32_t numParents;
        uint32_t preDelay;
        uint32_t postDelay; //we could get by with one delay, but pre/post makes it easier to code

    public:
        TimingEvent(uint32_t _preDelay, uint32_t _postDelay, int32_t _domain = -1) : next(nullptr), state(EV_NONE), cycle(0), minStartCycle(-1L), child(nullptr),
                    domain(_domain), numChildren(0), numParents(0), preDelay(_preDelay), postDelay(_postDelay) {}
        explicit TimingEvent(int32_t _domain = -1) : next(nullptr), state(EV_NONE), minStartCycle(-1L), child(nullptr),
                    domain(_domain), numChildren(0), numParents(0), preDelay(0), postDelay(0) {} //no delegating constructors until gcc 4.7...

        inline uint32_t getDomain() const {return domain;}
        inline uint32_t getNumChildren() const {return numChildren;}
        inline uint32_t getPreDelay() const {return preDelay;}
        inline uint32_t getPostDelay() const {return postDelay;}

        inline void setPreDelay(uint32_t d) {preDelay = d;}
        inline void setPostDelay(uint32_t d) {postDelay = d;}

        inline uint64_t getMinStartCycle() const {return minStartCycle;}
        inline void setMinStartCycle(uint64_t c) {minStartCycle = c;}

        TimingEvent* addChild(TimingEvent* childEv, EventRecorder* evRec) {
            assert_msg(state == EV_NONE || state == EV_QUEUED, "adding child in invalid state %d %s -> %s", state, typeid(*this).name(), typeid(*childEv).name()); //either not scheduled or not executed yet
            assert(childEv->state == EV_NONE);

            TimingEvent* res = childEv;

            if (numChildren == 0) {
                numChildren = 1;
                child = childEv;
            } else if (numChildren == 1) {
                TimingEvent* firstChild = child;
                children = new (evRec) TimingEventBlock();
                children->events[0] = firstChild;
                children->events[1] = childEv;
                numChildren = 2;
            } else {
                uint32_t idx = numChildren % TIMING_BLOCK_EVENTS;
                if (idx == 0) {
                    TimingEventBlock* tmp = children;
                    children = new (evRec) TimingEventBlock();
                    children->next = tmp;
                }
                children->events[idx] = childEv;
                numChildren++;
            }

            if (domain != -1 && childEv->domain == -1) {
                childEv->propagateDomain(domain);
            }

            childEv->numParents++;
            return res; //useful for chaining
        }

        TimingEvent* addChild(TimingEvent* childEv, EventRecorder& evRec) {
            return addChild(childEv, &evRec);
        }

        virtual void parentDone(uint64_t startCycle); // see cpp

        //queue for the first time
        //always happens on PHASE 1 (bound), and is synchronized
        void queue(uint64_t qCycle); //see cpp

        //mark an already-dequeued event for reexecution (simulate will be called again at the specified cycle)
        //always happens on PHASE 2 (weave), and is unsynchronized
        void requeue(uint64_t cycle); //see cpp

        virtual void simulate(uint64_t startCycle) = 0;

        inline void run(uint64_t startCycle) {
            assert(this);
            assert_msg(state == EV_NONE || state == EV_QUEUED, "state %d expected %d (%s)", state, EV_QUEUED, typeid(*this).name());
            state = EV_RUNNING;
            assert_msg(startCycle >= minStartCycle, "startCycle %ld < minStartCycle %ld (%s), preDelay %d postDelay %d numChildren %d str %s",
                    startCycle, minStartCycle, typeid(*this).name(), preDelay, postDelay, numChildren, str().c_str());
            simulate(startCycle);
            assert_msg(state == EV_DONE || state == EV_QUEUED || state == EV_HELD, "post-sim state %d (%s)", state, typeid(*this).name());
        }

        // Used when an external, event-driven object takes control of the object --- it becomes queued, but externally
        inline void hold() {
            assert_msg(state == EV_RUNNING, "called hold() with state %d", state);
            state = EV_HELD;
        }

        inline void release() {
            assert_msg(state == EV_HELD, "state should be %d, %d instead", EV_HELD, state);
            state = EV_RUNNING;
        }

        void done(uint64_t doneCycle) {
            assert(state == EV_RUNNING); //ContentionSim sets it when calling simulate()
            state = EV_DONE;
            auto vLambda = [this, doneCycle](TimingEvent** childPtr) {
                checkDomain(*childPtr);
                (*childPtr)->parentDone(doneCycle+postDelay);
            };
            visitChildren< decltype(vLambda) >(vLambda);

            // Free timing event blocks and ourselves
            if (numChildren > 1) {
                TimingEventBlock* teb = children;
                while (teb) {
                    TimingEventBlock* next = teb->next;
                    slab::freeElem((void*)teb);
                    teb = next;
                }
                children = nullptr;
                numChildren = 0;
            }
            slab::freeElem((void*)this);
        }

        void produceCrossings(EventRecorder* evRec);

        void* operator new (size_t sz, EventRecorder* evRec) {
            return evRec->alloc(sz);
        }

        void* operator new (size_t sz, EventRecorder& evRec) {
            return evRec.alloc(sz);
        }

        void operator delete(void*, size_t) {
            panic("TimingEvent::delete should never be called");
        }

        //Placement deletes... make ICC happy. This would only fire on an exception
        void operator delete (void* p, EventRecorder* evRec) {
            panic("TimingEvent::delete PLACEMENT delete called");
        }
        void operator delete (void* p, EventRecorder& evRec) {
            panic("TimingEvent::delete PLACEMENT delete called");
        }

        //Describe yourself, useful for debugging
        virtual std::string str() { std::string res; return res; }

    private:
        void* operator new (size_t);

        void propagateDomain(int32_t dom) {
            assert(domain == -1);
            domain = dom;
            auto vLambda = [this](TimingEvent** childPtr) {
                TimingEvent* child = *childPtr;
                if (child->domain == -1) child->propagateDomain(domain);
            };
            visitChildren< decltype(vLambda) >(vLambda);
        }

        template <typename F> //F has to be decltype(f)
        inline void visitChildren(F f) {
            if (numChildren == 0) return;
            //info("visit %p nc %d", this, numChildren);
            if (numChildren == 1) {
                f(&child);
            } else {
                TimingEventBlock* curBlock = children;
                while (curBlock) {
                    for (uint32_t i = 0; i < TIMING_BLOCK_EVENTS; i++) {
                        //info("visit %p i %d %p", this, i, curBlock->events[i]);
                        if (!curBlock->events[i]) {break;}
                        //info("visit %p i %d %p PASS", this, i, curBlock->events[i]);
                        f(&(curBlock->events[i]));
                    }
                    curBlock = curBlock->next;
                }
                //info("visit %p multi done", this);
            }
        }

        TimingEvent* handleCrossing(TimingEvent* child, EventRecorder* evRec, bool unlinkChild);

        void checkDomain(TimingEvent* ch);

    protected:

        // If an event is externally handled, and has no parents or children,
        // it can call this at initialization to always be between RUNNING and
        // QUEUED (through requeue())
        void setRunning() {
            assert(state == EV_NONE);
            state = EV_RUNNING;
        }


    friend class ContentionSim;
    friend class DelayEvent; //DelayEvent is, for now, the only child of TimingEvent that should do anything other than implement simulate
    friend class CrossingEvent;
};

class DelayEvent : public TimingEvent {
    public:
        explicit DelayEvent(uint32_t delay) : TimingEvent(delay, 0) {}

        virtual void parentDone(uint64_t startCycle) {
            cycle = MAX(cycle, startCycle);
            numParents--;
            if (!numParents) {
                uint64_t doneCycle = cycle + preDelay;
                state = EV_RUNNING;
                done(doneCycle);
            }
        }

        virtual void simulate(uint64_t simCycle) {
            panic("DelayEvent::simulate() was called --- DelayEvent wakes its children directly");
        }
};

class CrossingEvent : public TimingEvent {
    private:
        uint32_t srcDomain;
        volatile bool called;
        volatile uint64_t doneCycle;
        volatile uint64_t srcDomainCycleAtDone;
        EventRecorder* evRec;
        uint64_t origStartCycle;
        uint64_t simCount;
        TimingEvent* parentEv; //stored exclusively for resp-req xing chaining

        uint32_t preSlack, postSlack;

        class CrossingSrcEvent : public TimingEvent {
            private:
                CrossingEvent* ce;
            public:
                CrossingSrcEvent(CrossingEvent* _ce, uint32_t dom) : TimingEvent(0, 0, dom), ce(_ce) {
                    //These are never connected to anything, but substitute an existing event; so, this never gets
                    //numParents incremented, but we set it to 1 to maintain semantics in case we have a walk
                    assert(numParents == 0);
                    numParents = 1;
                }

                virtual void parentDone(uint64_t startCycle) {
                    assert_msg(numParents == 1, "CSE: numParents %d", numParents);
                    numParents = 0;
                    assert(numChildren == 0);
                    ce->markSrcEventDone(startCycle);
                    assert(state == EV_NONE);
                    state = EV_DONE;
                }

                virtual void simulate(uint64_t simCycle) {
                    panic("DelayEvent::simulate() called");
                }
        };

        CrossingSrcEvent cpe;

    public:
        CrossingEvent(TimingEvent* parent, TimingEvent* child, uint64_t _minStartCycle, EventRecorder* _evRec);

        TimingEvent* getSrcDomainEvent() {return &cpe;}

        virtual void parentDone(uint64_t startCycle);

        virtual void simulate(uint64_t simCycle);

    private:
        void markSrcEventDone(uint64_t cycle);

        friend class ContentionSim;
};


#endif  // TIMING_EVENT_H_
