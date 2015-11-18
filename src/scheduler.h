/** $glic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
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

#ifndef SCHEDULER_H_
#define SCHEDULER_H_

#include <algorithm>
#include <iomanip>
#include <list>
#include <sstream>
#include <vector>
#include "barrier.h"
#include "constants.h"
#include "core.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_unordered_set.h"
#include "g_std/g_vector.h"
#include "intrusive_list.h"
#include "proc_stats.h"
#include "process_stats.h"
#include "stats.h"
#include "zsim.h"

/**
 * TODO (dsm): This class is due for a heavy pass or rewrite. Some things are more complex than they should:
 * - The OUT state is unnecessary. It is done as a weak link between a thread that left and its context to preserve affinity, but
 *   there are far easier ways to implement this.
 * - Should allow for complete separation of scheduling policies. Done to some degree (schedContext, etc.), but it should be cleaner.
 * - wakeup() takes a needsJoin param that is computed per thread, but the barrier operates per core. This discrepancy manifests itself
 *   in a corner case: if we kill a process, the watchdog reclaims its slots, and the system is overcommitted, sometimes we don't do
 *   a join when we should.
 * - It should be clearer who transisions threads/contexts among states (the thread taking the context, the one giving the context?),
 *   I think this can potentially lead to races.
 */


/* Performs (pid, tid) -> cid translation; round-robin scheduling with no notion of locality or heterogeneity... */

class Scheduler : public GlobAlloc, public Callee {
    private:
        enum ThreadState {
            STARTED, //transient state, thread will do a join immediately after
            RUNNING, //has cid assigned, managed by the phase barrier
            OUT, //in leave() this phase, can rejoin immediately
            BLOCKED, //inside a system call, no cid assigned, not in the barrier or the runqueue
            SLEEPING, //inside a patched sleep syscall; no cid assigned, in sleepQueue; it is our responsibility to wake this thread up when its deadline arrives
            QUEUED //in the runqueue
        };

        enum ContextState {
            IDLE,
            USED
        };

        void (*atSyncFunc)(void); //executed by syncing thread while others are waiting. Good for non-thread-safe stuff
        Barrier bar;
        uint32_t numCores;
        uint32_t schedQuantum; //in phases

        struct FakeLeaveInfo;

        enum FutexJoinAction {FJA_NONE, FJA_WAKE, FJA_WAIT};
        struct FutexJoinInfo {
            FutexJoinAction action;
            uint32_t maxWakes;
            uint32_t wokenUp;
        };

        struct ThreadInfo : GlobAlloc, InListNode<ThreadInfo> {
            const uint32_t gid;
            const uint32_t linuxPid;
            const uint32_t linuxTid;

            ThreadState state;
            uint32_t cid; //only current if RUNNING; otherwise, it's the last one used.

            volatile ThreadInfo* handoffThread; //if at the end of a sync() this is not nullptr, we need to transfer our current context to the thread pointed here.
            volatile uint32_t futexWord;
            volatile bool needsJoin; //after waiting on the scheduler, should we join the barrier, or is our cid good to go already?

            bool markedForSleep; //if true, we will go to sleep on the next leave()
            uint64_t wakeupPhase; //if SLEEPING, when do we have to wake up?

            g_vector<bool> mask;

            FakeLeaveInfo* fakeLeave; // for accurate join-leaves, see below

            FutexJoinInfo futexJoin;

            ThreadInfo(uint32_t _gid, uint32_t _linuxPid, uint32_t _linuxTid, const g_vector<bool>& _mask) :
                InListNode<ThreadInfo>(), gid(_gid), linuxPid(_linuxPid), linuxTid(_linuxTid), mask(_mask)
            {
                state = STARTED;
                cid = 0;
                handoffThread = nullptr;
                futexWord = 0;
                markedForSleep = false;
                wakeupPhase = 0;
                assert(mask.size() == zinfo->numCores);
                uint32_t count = 0;
                for (auto b : mask) if (b) count++;
                if (count == 0) panic("Empty mask on gid %d!", gid);
                fakeLeave = nullptr;
                futexJoin.action = FJA_NONE;
            }
        };

        struct ContextInfo : InListNode<ContextInfo> {
            uint32_t cid;
            ContextState state;
            ThreadInfo* curThread; //only current if used, otherwise nullptr
        };

        g_unordered_map<uint32_t, ThreadInfo*> gidMap;
        g_vector<ContextInfo> contexts;

        InList<ContextInfo> freeList;

        InList<ThreadInfo> runQueue;
        InList<ThreadInfo> outQueue;
        InList<ThreadInfo> sleepQueue; //contains all the sleeping threads, it is ORDERED by wakeup time

        PAD();
        lock_t schedLock;
        PAD();

        PAD();
        lock_t gidMapLock;
        PAD();

        uint64_t curPhase;
        //uint32_t nextVictim;
        MTRand rnd;

        volatile bool terminateWatchdogThread;

        g_vector<std::pair<uint32_t, uint32_t>> pendingPidCleanups; //(pid, osPid) pairs of abruptly terminated processes

        //Stats
        Counter threadsCreated, threadsFinished;
        Counter scheduleEvents, waitEvents, handoffEvents, sleepEvents;
        Counter idlePhases, idlePeriods;
        VectorCounter occHist, runQueueHist;
        uint32_t scheduledThreads;

        // gid <-> (pid, tid) xlat functions
        inline uint32_t getGid(uint32_t pid, uint32_t tid) const {return (pid << 16) | tid;}
        inline uint32_t getPid(uint32_t gid) const {return gid >> 16;}
        inline uint32_t getTid(uint32_t gid) const {return gid & 0x0FFFF;}

    public:
        Scheduler(void (*_atSyncFunc)(void), uint32_t _parallelThreads, uint32_t _numCores, uint32_t _schedQuantum) :
            atSyncFunc(_atSyncFunc), bar(_parallelThreads, this), numCores(_numCores), schedQuantum(_schedQuantum), rnd(0x5C73D9134)
        {
            contexts.resize(numCores);
            for (uint32_t i = 0; i < numCores; i++) {
                contexts[i].cid = i;
                contexts[i].state = IDLE;
                contexts[i].curThread = nullptr;
                freeList.push_back(&contexts[i]);
            }
            schedLock = 0;
            gidMapLock = 0;
            //nextVictim = 0; //only used when freeList is empty.
            curPhase = 0;
            scheduledThreads = 0;

            maxAllowedFutexWakeups = 0;
            unmatchedFutexWakeups = 0;

            blockingSyscalls.resize(MAX_THREADS /* TODO: max # procs */);

            info("Started RR scheduler, quantum=%d phases", schedQuantum);
            terminateWatchdogThread = false;
            startWatchdogThread();
        }

        ~Scheduler() {}

        void initStats(AggregateStat* parentStat) {
            AggregateStat* schedStats = new AggregateStat();
            schedStats->init("sched", "Scheduler stats");
            threadsCreated.init("thCr", "Threads created"); schedStats->append(&threadsCreated);
            threadsFinished.init("thFn", "Threads finished"); schedStats->append(&threadsFinished);
            scheduleEvents.init("schedEvs", "Schedule events"); schedStats->append(&scheduleEvents);
            waitEvents.init("waitEvs", "Wait events"); schedStats->append(&waitEvents);
            handoffEvents.init("handoffEvs", "Handoff events"); schedStats->append(&handoffEvents);
            sleepEvents.init("sleepEvs", "Sleep events"); schedStats->append(&sleepEvents);
            idlePhases.init("idlePhases", "Phases with no thread active"); schedStats->append(&idlePhases);
            idlePeriods.init("idlePeriods", "Periods with no thread active"); schedStats->append(&idlePeriods);
            occHist.init("occHist", "Occupancy histogram", numCores+1); schedStats->append(&occHist);
            uint32_t runQueueHistSize = ((numCores > 16)? numCores : 16) + 1;
            runQueueHist.init("rqSzHist", "Run queue size histogram", runQueueHistSize); schedStats->append(&runQueueHist);
            parentStat->append(schedStats);
        }

        void start(uint32_t pid, uint32_t tid, const g_vector<bool>& mask) {
            futex_lock(&schedLock);
            uint32_t gid = getGid(pid, tid);
            //info("[G %d] Start", gid);
            assert((gidMap.find(gid) == gidMap.end()));
            // Get pid and tid straight from the OS
            // - SYS_gettid because glibc does not implement gettid()
            // - SYS_getpid because after a fork (where zsim calls ThreadStart),
            //   getpid() returns the parent's pid (getpid() caches, and I'm
            //   guessing it hasn't flushed its cached pid at this point)
            futex_lock(&gidMapLock);
            gidMap[gid] = new ThreadInfo(gid, syscall(SYS_getpid), syscall(SYS_gettid), mask);
            futex_unlock(&gidMapLock);
            threadsCreated.inc();
            futex_unlock(&schedLock);
        }

        void finish(uint32_t pid, uint32_t tid) {
            futex_lock(&schedLock);
            uint32_t gid = getGid(pid, tid);
            //info("[G %d] Finish", gid);
            assert((gidMap.find(gid) != gidMap.end()));
            ThreadInfo* th = gidMap[gid];
            futex_lock(&gidMapLock);
            gidMap.erase(gid);
            futex_unlock(&gidMapLock);

            // Check for suppressed syscall leave(), execute it
            if (th->fakeLeave) {
                finishFakeLeave(th);
                futex_unlock(&schedLock);
                leave(pid, tid, th->cid);
                futex_lock(&schedLock);
            }

            //dsm: Added this check; the normal sequence is leave, finish, but with fastFwd you never know
            if (th->state == RUNNING) {
                warn("RUNNING thread %d (cid %d) called finish(), trying leave() first", tid, th->cid);
                futex_unlock(&schedLock); //FIXME: May be racey...
                leave(pid, tid, th->cid);
                futex_lock(&schedLock);
            }

            assert_msg(th->state == STARTED /*might be started but in fastFwd*/ ||th->state == OUT || th->state == BLOCKED || th->state == QUEUED, "gid %d finish with state %d", gid, th->state);
            if (th->state == QUEUED) {
                assert(th->owner == &runQueue);
                runQueue.remove(th);
            } else if (th->owner) {
                assert(th->owner == &outQueue);
                outQueue.remove(th);
                ContextInfo* ctx = &contexts[th->cid];
                deschedule(th, ctx, BLOCKED);
                freeList.push_back(ctx);
                //no need to try to schedule anything; this context was already being considered while in outQueue
                //assert(runQueue.empty()); need not be the case with masks
                //info("[G %d] Removed from outQueue and descheduled", gid);
            }
            //At this point noone holds pointer to th, it's out from all queues, and either on OUT or BLOCKED means it's not pending a handoff
            delete th;
            threadsFinished.inc();
            futex_unlock(&schedLock);
        }

        uint32_t join(uint32_t pid, uint32_t tid) {
            futex_lock(&schedLock);
            //If leave was in this phase, call bar.join()
            //Otherwise, try to grab a free context; if all are taken, queue up
            uint32_t gid = getGid(pid, tid);
            ThreadInfo* th = gidMap[gid];

            //dsm 25 Oct 2012: Failed this assertion right after a fork when trying to simulate gedit. Very weird, cannot replicate.
            //dsm 10 Apr 2013: I think I got it. We were calling sched->finish() too early when following exec.
            assert_msg(th, "gid not found %d pid %d tid %d", gid, pid, tid);

            if (unlikely(th->futexJoin.action != FJA_NONE)) {
                if (th->futexJoin.action == FJA_WAIT) futexWaitJoin(th);
                else futexWakeJoin(th);  // may release and grab schedLock to delay our join, this is fine at this point
                th->futexJoin.action = FJA_NONE;
            }

            // If we're in a fake leave, no need to do anything
            if (th->fakeLeave) {
                finishFakeLeave(th);
                uint32_t cid = th->cid;
                futex_unlock(&schedLock);
                return cid;
            }

            assert(!th->markedForSleep);

            if (th->state == SLEEPING) {
                /*panic(*/ warn("[%d] called join() while SLEEPING, early sleep termination, moving to BLOCKED", tid);
                sleepQueue.remove(th);
                th->state = BLOCKED;
            }

            if (th->state == OUT) {
                th->state = RUNNING;
                outQueue.remove(th);
                zinfo->cores[th->cid]->join();
                bar.join(th->cid, &schedLock); //releases lock
            } else {
                assert(th->state == BLOCKED || th->state == STARTED);

                ContextInfo* ctx = schedThread(th);
                if (ctx) {
                    schedule(th, ctx);
                    zinfo->cores[th->cid]->join();
                    bar.join(th->cid, &schedLock); //releases lock
                } else {
                    th->state = QUEUED;
                    runQueue.push_back(th);
                    waitForContext(th); //releases lock, might join
                }
            }

            return th->cid;
        }

        void leave(uint32_t pid, uint32_t tid, uint32_t cid) {
            futex_lock(&schedLock);
            //Just call bar.leave
            uint32_t gid = getGid(pid, tid);
            ThreadInfo* th = contexts[cid].curThread;
            assert(th->gid == gid);
            assert(th->state == RUNNING);
            zinfo->cores[cid]->leave();

            if (th->markedForSleep) { //transition to SLEEPING, eagerly deschedule
                trace(Sched, "Sched: %d going to SLEEP, wakeup on phase %ld", gid, th->wakeupPhase);
                th->markedForSleep = false;
                ContextInfo* ctx = &contexts[cid];
                deschedule(th, ctx, SLEEPING);

                //Ordered insert into sleepQueue
                if (sleepQueue.empty() || sleepQueue.front()->wakeupPhase > th->wakeupPhase) {
                    sleepQueue.push_front(th);
                } else {
                    ThreadInfo* cur = sleepQueue.front();
                    while (cur->next && cur->next->wakeupPhase <= th->wakeupPhase) {
                        cur = cur->next;
                    }
                    trace(Sched, "Put %d in sleepQueue (deadline %ld), after %d (deadline %ld)", gid, th->wakeupPhase, cur->gid, cur->wakeupPhase);
                    sleepQueue.insertAfter(cur, th);
                }
                sleepEvents.inc();

                ThreadInfo* inTh = schedContext(ctx);
                if (inTh) {
                    schedule(inTh, ctx);
                    zinfo->cores[ctx->cid]->join(); //inTh does not do a sched->join, so we need to notify the core since we just called leave() on it
                    wakeup(inTh, false /*no join, we did not leave*/);
                } else {
                    freeList.push_back(ctx);
                    bar.leave(cid); //may trigger end of phase
                }
            } else { //lazily transition to OUT, where we retain our context
                ContextInfo* ctx = &contexts[cid];
                ThreadInfo* inTh = schedContext(ctx);
                if (inTh) { //transition to BLOCKED, sched inTh
                    deschedule(th, ctx, BLOCKED);
                    schedule(inTh, ctx);
                    zinfo->cores[ctx->cid]->join(); //inTh does not do a sched->join, so we need to notify the core since we just called leave() on it
                    wakeup(inTh, false /*no join, we did not leave*/);
                } else { //lazily transition to OUT, where we retain our context
                    th->state = OUT;
                    outQueue.push_back(th);
                    bar.leave(cid); //may trigger end of phase
                }
            }

            futex_unlock(&schedLock);
        }

        uint32_t sync(uint32_t pid, uint32_t tid, uint32_t cid) {
            futex_lock(&schedLock);
            ThreadInfo* th = contexts[cid].curThread;
            assert(!th->markedForSleep);
            bar.sync(cid, &schedLock); //releases lock, may trigger end of phase, may block us

            //No locks at this point; we need to check whether we need to hand off our context
            if (th->handoffThread) {
                futex_lock(&schedLock);  // this can be made lock-free, but it's not worth the effort
                ThreadInfo* dst = const_cast<ThreadInfo*>(th->handoffThread);  // de-volatilize
                th->handoffThread = nullptr;
                ContextInfo* ctx = &contexts[th->cid];
                deschedule(th, ctx, QUEUED);
                schedule(dst, ctx);
                wakeup(dst, false /*no join needed*/);
                handoffEvents.inc();
                //info("%d starting handoff cid %d to gid %d", th->gid, ctx->cid, dst->gid);

                //We're descheduled and have completed the handoff. Now we need to see if we can be scheduled somewhere else.
                ctx = schedThread(th);
                if (ctx) {
                    //TODO: This should only arise in very weird cases (e.g., partially overlapping process masks), and has not been tested
                    warn("Sched: untested code path, check with Daniel if you see this");
                    schedule(th, ctx);
                    //We need to do a join, because dst will not join
                    zinfo->cores[ctx->cid]->join();
                    bar.join(ctx->cid, &schedLock); //releases lock
                } else {
                    runQueue.push_back(th);
                    waitForContext(th); //releases lock, might join
                }
            }

            assert(th->state == RUNNING);
            return th->cid;
        }

        // This is called with schedLock held, and must not release it!
        virtual void callback() {
            //End of phase stats
            assert(scheduledThreads <= numCores);
            occHist.inc(scheduledThreads);
            uint32_t rqPos = (runQueue.size() < (runQueueHist.size()-1))? runQueue.size() : (runQueueHist.size()-1);
            runQueueHist.inc(rqPos);

            if (atSyncFunc) atSyncFunc(); //call the simulator-defined actions external to the scheduler

            /* End of phase accounting */
            zinfo->numPhases++;
            zinfo->globPhaseCycles += zinfo->phaseLength;
            curPhase++;

            assert(curPhase == zinfo->numPhases); //check they don't skew

            //Wake up all sleeping threads where deadline is met
            if (!sleepQueue.empty()) {
                ThreadInfo* th = sleepQueue.front();
                while (th && th->wakeupPhase <= curPhase) {
                    assert(th->wakeupPhase == curPhase);
                    trace(Sched, "%d SLEEPING -> BLOCKED, waking up from timeout syscall (curPhase %ld, wakeupPhase %ld)", th->gid, curPhase, th->wakeupPhase);

                    // Try to deschedule ourselves
                    th->state = BLOCKED;
                    wakeup(th, false /*no join, this is sleeping out of the scheduler*/);

                    sleepQueue.pop_front();
                    th = sleepQueue.front();
                }
            }

            //Handle rescheduling
            if (runQueue.empty()) return;

            if ((curPhase % schedQuantum) == 0) {
                schedTick();
            }
        }

        volatile uint32_t* markForSleep(uint32_t pid, uint32_t tid, uint64_t wakeupPhase) {
            futex_lock(&schedLock);
            uint32_t gid = getGid(pid, tid);
            trace(Sched, "%d marking for sleep", gid);
            ThreadInfo* th = gidMap[gid];
            assert(!th->markedForSleep);
            th->markedForSleep = true;
            th->wakeupPhase = wakeupPhase;
            th->futexWord = 1; //to avoid races, this must be set here.
            futex_unlock(&schedLock);
            return &(th->futexWord);
        }

        bool isSleeping(uint32_t pid, uint32_t tid) {
            uint32_t gid = getGid(pid, tid);
            futex_lock(&gidMapLock);
            ThreadInfo* th = gidMap[gid];
            futex_unlock(&gidMapLock);
            bool res = th->state == SLEEPING;
            return res;
        }

        // Returns the number of remaining phases to sleep
        uint64_t notifySleepEnd(uint32_t pid, uint32_t tid) {
            futex_lock(&schedLock);
            uint32_t gid = getGid(pid, tid);
            ThreadInfo* th = gidMap[gid];
            assert(th->markedForSleep == false);
            //Move to BLOCKED; thread will join pretty much immediately
            assert(th->state == SLEEPING || th->state == BLOCKED);
            if (th->state == BLOCKED) {
                warn("Scheduler:notifySleepEnd: Benign race on SLEEPING->BLOCKED transition, thread is already blocked");
            } else {
                sleepQueue.remove(th);
                th->state = BLOCKED;
            }
            futex_unlock(&schedLock);
            return th->wakeupPhase - zinfo->numPhases;
        }

        void printThreadState(uint32_t pid, uint32_t tid) {
            futex_lock(&schedLock);
            uint32_t gid = getGid(pid, tid);
            ThreadInfo* th = gidMap[gid];
            info("[%d] is in scheduling state %d", tid, th->state);
            futex_unlock(&schedLock);
        }

        void notifyTermination() {
            /* dsm 2013-06-15: Traced a deadlock at termination down here... looks like with MT apps this lock is held at SimEnd.
             * Leaving the lock off is safe now, but if this function gets more complex, we may have to rethink this.
             */
            //futex_lock(&schedLock);
            terminateWatchdogThread = true;
            //futex_unlock(&schedLock);
        }

        //Should be called when a process is terminated abruptly (e.g., through a signal).
        //Walks the gidMap and calls leave/finish on all threads of the process. Not quite race-free,
        //we could have private unlocked versions of leave, finifh, etc, but the key problem is that
        //if you call this and any other thread in the process is still alive, then there is a
        //much bigger problem.
        void processCleanup(uint32_t pid) {
            futex_lock(&schedLock);
            std::vector<uint32_t> doomedTids;
            g_unordered_map<uint32_t, ThreadInfo*>::iterator it;
            for (it = gidMap.begin(); it != gidMap.end(); it++) {
                uint32_t gid = it->first;
                if (getPid(gid) == pid) doomedTids.push_back(getTid(gid));
            }
            futex_unlock(&schedLock);

            if (doomedTids.size()) {
                for (uint32_t tid : doomedTids) {
                    if (isSleeping(pid, tid)) {
                        notifySleepEnd(pid, tid);
                    }
                    finish(pid, tid);
                }
                info("[sched] Cleaned up pid %d, %ld tids", pid, doomedTids.size());
            }
        }

        //Calling doProcessCleanup on multithreaded processes leads to races,
        //so we'll just have the watchdog thread to it once we're gone
        void queueProcessCleanup(uint32_t pid, uint32_t osPid) {
            futex_lock(&schedLock);
            pendingPidCleanups.push_back(std::make_pair(pid, osPid));
            futex_unlock(&schedLock);
        }

        uint32_t getScheduledPid(uint32_t cid) const { return (contexts[cid].state == USED)? getPid(contexts[cid].curThread->gid) : (uint32_t)-1; }

    private:
        void schedule(ThreadInfo* th, ContextInfo* ctx) {
            assert(th->state == STARTED || th->state == BLOCKED || th->state == QUEUED);
            assert(ctx->state == IDLE);
            assert(ctx->curThread == nullptr);
            th->state = RUNNING;
            th->cid = ctx->cid;
            ctx->state = USED;
            ctx->curThread = th;
            scheduleEvents.inc();
            scheduledThreads++;
            //info("Scheduled %d <-> %d", th->gid, ctx->cid);
            zinfo->cores[ctx->cid]->contextSwitch(th->gid);
        }

        void deschedule(ThreadInfo* th, ContextInfo* ctx, ThreadState targetState) {
            assert(th->state == RUNNING || th->state == OUT);
            assert(ctx->state == USED);
            assert(ctx->cid == th->cid);
            assert(ctx->curThread == th);
            assert(targetState == BLOCKED || targetState == QUEUED || targetState == SLEEPING);
            if (zinfo->procStats) zinfo->procStats->notifyDeschedule();  // FIXME: Interface
            th->state = targetState;
            ctx->state = IDLE;
            ctx->curThread = nullptr;
            scheduledThreads--;
            //Notify core of context-switch eagerly.
            //TODO: we may need more callbacks in the cores, e.g. in schedule(). Revise interface as needed...
            zinfo->cores[ctx->cid]->contextSwitch(-1);
            zinfo->processStats->notifyDeschedule(ctx->cid, getPid(th->gid));
            //info("Descheduled %d <-> %d", th->gid, ctx->cid);
        }

        void waitForContext(ThreadInfo* th) {
            th->futexWord = 1;
            waitEvents.inc();
            //info("%d waiting to be scheduled", th->gid);
            //printState();
            futex_unlock(&schedLock);
            while (true) {
                int futex_res = syscall(SYS_futex, &th->futexWord, FUTEX_WAIT, 1 /*a racing thread waking us up will change value to 0, and we won't block*/, nullptr, nullptr, 0);
                if (futex_res == 0 || th->futexWord != 1) break;
            }
            //info("%d out of sched wait, got cid = %d, needsJoin = %d", th->gid, th->cid, th->needsJoin);
            if (th->needsJoin) {
                futex_lock(&schedLock);
                assert(th->needsJoin); //re-check after the lock
                zinfo->cores[th->cid]->join();
                bar.join(th->cid, &schedLock);
                //info("%d join done", th->gid);
            }
        }

        void wakeup(ThreadInfo* th, bool needsJoin) {
            th->needsJoin = needsJoin;
            bool succ = __sync_bool_compare_and_swap(&th->futexWord, 1, 0);
            if (!succ) panic("Wakeup race in barrier?");
            syscall(SYS_futex, &th->futexWord, FUTEX_WAKE, 1, nullptr, nullptr, 0);
            waitUntilQueued(th);
        }

        void printState() {
            std::stringstream ss;
            for (uint32_t c = 0; c < numCores; c++) {
                if (contexts[c].state == IDLE) {
                    ss << " " << "___";
                } else {
                    ss << " " << std::setw(2) << contexts[c].curThread->gid;
                    if (contexts[c].curThread->state == RUNNING) ss << "r";
                    else if (contexts[c].curThread->state == OUT) ss << "o";
                    else panic("Invalid state cid=%d, threadState=%d", c, contexts[c].curThread->state);
                }
            }
            info(" State: %s", ss.str().c_str());
        }


        //Core scheduling functions
        /* This is actually the interface that an abstract OS scheduler would have, and implements the scheduling policy:
         * - schedThread(): Here's a thread that just became available; return either a ContextInfo* where to schedule it, or nullptr if none are available
         * - schedContext(): Here's a context that just became available; return either a ThreadInfo* to schedule on it, or nullptr if none are available
         * - schedTick(): Current quantum is over, hand off contexts to other threads as you see fit
         * These functions can REMOVE from runQueue, outQueue, and freeList, but do not INSERT. These are filled in elsewhere. They also have minimal concerns
         * for thread and context states. Those state machines are implemented and handled elsewhere, except where strictly necessary.
         */
        ContextInfo* schedThread(ThreadInfo* th) {
            ContextInfo* ctx = nullptr;

            //First, try to get scheduled in the last context we were running at
            assert(th->cid < numCores); //though old, it should be in a valid range
            if (contexts[th->cid].state == IDLE && th->mask[th->cid]) {
                ctx = &contexts[th->cid];
                freeList.remove(ctx);
            }

            //Second, check the freeList
            if (!ctx && !freeList.empty()) {
                ContextInfo* c = freeList.front();
                while (c) {
                    if (th->mask[c->cid]) {
                        ctx = c;
                        freeList.remove(ctx);
                        break;
                    } else {
                        c = c->next;
                    }
                }
            }

            //Third, try to steal from the outQueue (block a thread, take its cid)
            if (!ctx && !outQueue.empty()) {
                ThreadInfo* outTh = outQueue.front();
                while (outTh) {
                    if (th->mask[outTh->cid]) {
                        ctx = &contexts[outTh->cid];
                        outQueue.remove(outTh);
                        deschedule(outTh, ctx, BLOCKED);
                        break;
                    } else {
                        outTh = outTh->next;
                    }
                }
            }

            if (ctx) assert(th->mask[ctx->cid]);

            //info("schedThread done, gid %d, success %d", th->gid, ctx != nullptr);
            //printState();
            return ctx;
        }

        ThreadInfo* schedContext(ContextInfo* ctx) {
            ThreadInfo* th = nullptr;
            ThreadInfo* blockedTh = runQueue.front();  // null if empty
            while (blockedTh) {
                if (blockedTh->mask[ctx->cid]) {
                    th = blockedTh;
                    runQueue.remove(blockedTh);
                    break;
                } else {
                    blockedTh = blockedTh->next;
                }
            }

            //info("schedContext done, cid %d, success %d (gid %d)", ctx->cid, th != nullptr, th? th->gid : 0);
            //printState();
            return th;
        }

        void schedTick() {
            std::vector<uint32_t> availVec;
            availVec.resize(zinfo->numCores);
            for (uint32_t i = 0; i < zinfo->numCores; i++) availVec[i] = i;

            //Random shuffle (Fisher-Yates)
            for (uint32_t i = zinfo->numCores - 1; i > 0; i--) {
                uint32_t j = rnd.randInt(i); //j is in 0,...,i
                std::swap(availVec[i], availVec[j]);
            }

            std::list<uint32_t> avail(availVec.begin(), availVec.end());

            /* NOTE: avail has all cores, including those in freeList, which may not be empty.
             * But we will never match anything in the freeList, because schedContext and
             * schedThread would have matched them out. So, no need to prioritize the freeList.
             */

            uint32_t contextSwitches = 0;

            ThreadInfo* th = runQueue.front();
            while (th && !avail.empty()) {
                bool scheduled = false;
                for (std::list<uint32_t>::iterator it = avail.begin(); it != avail.end(); it++) {
                    uint32_t cid = *it;
                    if (th->mask[cid]) {
                        ContextInfo* ctx = &contexts[cid];
                        ThreadInfo* victimTh = ctx->curThread;
                        assert(victimTh);
                        victimTh->handoffThread = th;
                        contextSwitches++;

                        scheduled = true;
                        avail.erase(it);
                        break;
                    }
                }

                ThreadInfo* pth = th;
                th = th->next;
                if (scheduled) runQueue.remove(pth);
            }

            info("Time slice ended, context-switched %d threads, runQueue size %ld, available %ld", contextSwitches, runQueue.size(), avail.size());
            printState();
        }

        //Watchdog thread functions
        /* With sleeping threads, we have to drive time forward if no thread is scheduled and some threads are sleeping; otherwise, we can deadlock.
         * This initially was the responsibility of the last leaving thread, but led to horribly long syscalls being simulated. For example, if you
         * have 2 threads, 1 is sleeping and the other one goes on a syscall, it had to drive time fwd to wake the first thread up, on the off-chance
         * that the impending syscall was blocking, to avoid deadlock.
         * Instead, we have an auxiliary thread check for this condition periodically, and if all threads are sleeping or blocked, we just drive time
         * forward.
         */
        void startWatchdogThread();
        void watchdogThreadFunc();

        static void threadTrampoline(void* arg);

    /* Accurate and adaptive join-leave
     *
     * Threads leave() on a syscall enter and join() when they return, which desyncs them from the simulation to prevent deadlock through syscalls.
     * In practice this is often not an issue because most syscalls are short enough that they finish before the phase changes. However, with highly
     * overcommitted systems and system-intensive apps, we've started seeing some timeing leakage. The old syscall_funcs reduced this problem by avoiding
     * a leave on safe syscalls, but that solution was quite restrictive: there are many syscalls that could theoretically block, but never do. Additionally,
     * futexes and sleeps, which are blocking but for which we can accurately infer their join phase, may suffer from inaccurate joins.
     *
     * To this end, the following interface supports an adaptive join-leave implementation that avoids most desyncs:
     * - Threads should call syscallLeave() and syscallJoin(), passing their PC and a small syscall descriptor for a few syscalls of interest.
     * - The scheduler adaptively decides whether we should wait for a syscall to join or to start the next phase. It avoids deadlock by having
     *   the watchdog detect potential deadlocks, and desyncing the threads. To avoid frequent desyncs, it blacklists syscalls
     * - When the scheduler wakes up a sleeping thread (e.g., in a timeout syscall), it ensures the phase does not slip by.
     * - When the scheduler sees a FUTEX_WAKE, it ensures we wait for the woken-up thread(s).
     *
     * TODO: This code is currently written to be as independent as possible from the other sched and barrier code.
     * If it works well, the code should be reorganized and simplified.
     */
    private:
        // All structures protected by schedLock

        // Per-process per-PC blacklist
        g_vector< g_unordered_set<uint64_t> > blockingSyscalls;

        struct FakeLeaveInfo : GlobAlloc, InListNode<FakeLeaveInfo> {
            const uint64_t pc;
            ThreadInfo* const th;
            const int syscallNumber;
            const uint64_t arg0; // kept for reference
            const uint64_t arg1; // kept for reference

            FakeLeaveInfo(uint64_t _pc, ThreadInfo* _th, int _syscallNumber, uint64_t _arg0, uint64_t _arg1) :
                pc(_pc), th(_th), syscallNumber(_syscallNumber), arg0(_arg0), arg1(_arg1)
            {
                assert(th->fakeLeave == nullptr);
                th->fakeLeave = this;
            }

            ~FakeLeaveInfo() {
                assert(th->fakeLeave == this);
                th->fakeLeave = nullptr;
            }
        };

        // All active syscalls that are still in the simulator (no leave()) have an entry here
        InList<FakeLeaveInfo> fakeLeaves;

        // TODO: Futex wait/wake matching code

    public:
        // Externally, has the exact same behavior as leave(); internally, may choose to not actually leave;
        // join() and finish() handle this state
        void syscallLeave(uint32_t pid, uint32_t tid, uint32_t cid, uint64_t pc, int syscallNumber, uint64_t arg0, uint64_t arg1);

        // Futex wake/wait matching interface
        void notifyFutexWakeStart(uint32_t pid, uint32_t tid, uint32_t maxWakes);
        void notifyFutexWakeEnd(uint32_t pid, uint32_t tid, uint32_t wokenUp);
        void notifyFutexWaitWoken(uint32_t pid, uint32_t tid);

    private:
        volatile uint32_t maxAllowedFutexWakeups;
        volatile uint32_t unmatchedFutexWakeups;

        // Called with schedLock held, at the start of a join
        void futexWakeJoin(ThreadInfo* th);  // may release and re-acquire schedLock
        void futexWaitJoin(ThreadInfo* th);


        void finishFakeLeave(ThreadInfo* th);

        /* Must be called with schedLock held. Waits until the given thread is
         * queued in schedLock. Used for accurate wakeups, by calling here we
         * ensure that the waking thread won't skip a phase. May cause deadlock
         * if used incorrectly.
         */
        void waitUntilQueued(ThreadInfo* th);
};


#endif  // SCHEDULER_H_
