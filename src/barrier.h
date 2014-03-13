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

/* Implements a barrier with join-leave semantics and parallelism control.
 * JOIN-LEAVE SEMANTICS: Threads can join or leave the barrier at any point in time.
 * Threads in the barrier call sync and synchronize with all other threads
 * participating in the barrier. Threads can leave a barrier at any point in time
 * (e.g. when other threads have started the sync).
 *
 * PARALLELISM CONTROL: The barrier limits the number of threads that run at the same time.
 *
 * Author: Daniel Sanchez <sanchezd@stanford.edu>
 * Date: Apr 2011
 */

#ifndef BARRIER_H_
#define BARRIER_H_

#include <errno.h>
#include <linux/futex.h>
#include <stdint.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>
#include "constants.h"
#include "galloc.h"
#include "locks.h"
#include "log.h"
#include "mtrand.h"

// Configure futex timeouts (die rather than deadlock)
#define TIMEOUT_LENGTH 20 //seconds
#define MAX_TIMEOUTS 10

//#define DEBUG_BARRIER(args...) info(args)
#define DEBUG_BARRIER(args...)

class Callee {
    public:
        virtual void callback() = 0;
};


class Barrier : public GlobAlloc {
    private:
        uint32_t parallelThreads;

        enum State {OFFLINE, WAITING, RUNNING, LEFT};

        struct ThreadSyncInfo {
            volatile State state;
            volatile uint32_t futexWord;
            uint32_t lastIdx;
            uint32_t pad;
        };

        ThreadSyncInfo threadList[MAX_THREADS];

        uint32_t* runList;
        uint32_t runListSize;
        uint32_t curThreadIdx;

        uint32_t runningThreads; //threads in RUNNING state
        uint32_t leftThreads; //threads in LEFT state
        //Threads in OFFLINE state are not on the runlist, so runListSize - runningThreads - leftThreads == waitingThreads

        uint32_t phaseCount; //INTERNAL, for LEFT->OFFLINE bookkeeping overhead reduction purposes

        uint32_t pad[16];

        /* NOTE(dsm): I was initially misled that having a single lock protecting the barrier was a performance hog, and coded a lock-free version.
         * Profiling doesn't show that, however. What happened was that shorter phases caused a worse interaction with PIN locks in the memory
         * hierarchy (which use yield, not futex?). The lock-free version was actually a bit slower, as we're already serializing on curThreadIdx and
         * the lock-free version required to volatilize pretty much every variable. If serialization on sync() ever becomes an issue, ask me for the
         * lock-free code.
         */
        //lock_t barrierLock; //not used anymore, using the scheduler lock instead since this is called from the scheduler

        MTRand rnd;
        Callee* sched; //FIXME: I don't like this organization, but don't have time to refactor the barrier code, this is used for a callback when the phase is done

    public:
        Barrier(uint32_t _parallelThreads, Callee* _sched) : parallelThreads(_parallelThreads), rnd(0xBA77137), sched(_sched) {
            for (uint32_t t = 0; t < MAX_THREADS; t++) {
                threadList[t].state = OFFLINE;
                threadList[t].futexWord = 0;
            }

            runList = gm_calloc<uint32_t>(MAX_THREADS);
            runListSize = 0;
            curThreadIdx = 0;

            runningThreads = 0;
            leftThreads = 0;
            phaseCount = 0;
            //barrierLock = 0;
        }

        ~Barrier() {}

        //Called with schedLock held; returns with schedLock unheld
        void join(uint32_t tid, lock_t* schedLock) {
            DEBUG_BARRIER("[%d] Joining, runningThreads %d, prevState %d", tid, runningThreads, threadList[tid].state);
            assert(threadList[tid].state == LEFT || threadList[tid].state == OFFLINE);
            if (threadList[tid].state == OFFLINE) {
                runList[runListSize++] = tid;
            } else {
                leftThreads--;
                //If we have already run in this phase, reschedule ourselves in it
                uint32_t lastIdx = threadList[tid].lastIdx;
                if (curThreadIdx > lastIdx) { //curThreadIdx points to the FIRST thread that tryWakeNext checks
                    DEBUG_BARRIER("[%d] Doing same-phase join reschedule", tid);
                    curThreadIdx--;
                    //Swap our runlist tid with the last thread's
                    assert(tid == runList[lastIdx]);
                    uint32_t otherTid = runList[curThreadIdx];

                    runList[lastIdx] = otherTid;
                    runList[curThreadIdx] = tid;
                    threadList[otherTid].lastIdx = lastIdx;
                    threadList[tid].lastIdx = curThreadIdx;
                    //now we'll be scheduled next :)
                }
            }


            threadList[tid].state = WAITING;
            threadList[tid].futexWord = 1;
            tryWakeNext(tid); //NOTE: You can't cause a phase to end here.
            futex_unlock(schedLock);

            if (threadList[tid].state == WAITING) {
                DEBUG_BARRIER("[%d] Waiting on join", tid);
                while (true) {
                    int futex_res = syscall(SYS_futex, &threadList[tid].futexWord, FUTEX_WAIT, 1 /*a racing thread waking us up will change value to 0, and we won't block*/, NULL, NULL, 0);
                    if (futex_res == 0 || threadList[tid].futexWord != 1) break;
                }
                //The thread that wakes us up changes this
                assert(threadList[tid].state == RUNNING);
            }
        }

        //Must be called with schedLock held
        void leave(uint32_t tid) {
            DEBUG_BARRIER("[%d] Leaving, runningThreads %d", tid, runningThreads);
            if (threadList[tid].state == RUNNING) {
                threadList[tid].state = LEFT;
                leftThreads++;
                runningThreads--;
                tryWakeNext(tid); //can trigger phase end
            } else {
                assert_msg(threadList[tid].state == WAITING, "leave, tid %d, incorrect state %d", tid, threadList[tid].state);
                threadList[tid].state = LEFT;
                leftThreads++;
            }
        }

        //Called with schedLock held, returns with schedLock unheld
        void sync(uint32_t tid, lock_t* schedLock) {
            DEBUG_BARRIER("[%d] Sync", tid);
            assert_msg(threadList[tid].state == RUNNING, "[%d] sync: state was supposed to be %d, it is %d", tid, RUNNING, threadList[tid].state);
            threadList[tid].futexWord = 1;
            threadList[tid].state = WAITING;
            runningThreads--;
            tryWakeNext(tid); //can trigger phase end
            futex_unlock(schedLock);

            if (threadList[tid].state == WAITING) {
                while (true) {
                    int futex_res = syscall(SYS_futex, &threadList[tid].futexWord, FUTEX_WAIT, 1 /*a racing thread waking us up will change value to 0, and we won't block*/, NULL, NULL, 0);
                    if (futex_res == 0 || threadList[tid].futexWord != 1) break;
                }
                //The thread that wakes us up changes this
                assert(threadList[tid].state == RUNNING);
            }
        }

    private:
        inline void checkEndPhase(uint32_t tid) {
            if (curThreadIdx == runListSize && runningThreads == 0) {
                if (leftThreads == runListSize) {
                    DEBUG_BARRIER("[%d] All threads left barrier, not ending current phase", tid);
                    return; //watch the early return
                }
                DEBUG_BARRIER("[%d] Phase ended", tid);
                // End of phase actions
                sched->callback();
                curThreadIdx = 0; //rewind list

                if (((phaseCount++) & (32-1)) == 0) { //one out of 32 times, do
                    /* Pass over the whole array, OFFLINE the threads that LEFT. If they are on a syscall, they will rejoin;
                     * If they left for good, we avoid long-term traversal overheads on apps with a varying number of threads.
                     */
                    assert(runListSize > 0);
                    uint32_t idx = 0;
                    uint32_t newSize = runListSize;
                    while (idx < newSize) {
                        uint32_t wtid = runList[idx];
                        if (threadList[wtid].state == LEFT) {
                            threadList[wtid].state = OFFLINE;
                            uint32_t stid = runList[newSize-1];
                            runList[idx] = stid;
                            threadList[stid].lastIdx = idx;

                            newSize--; //last elem is now garbage
                        } else {
                            idx++; //this one is OK, keep going
                        }
                    }
                    assert(runListSize - newSize == leftThreads);
                    leftThreads = 0;
                    DEBUG_BARRIER("[%d] Cleanup pass, initial runListSize %d, now %d", tid, runListSize, newSize);
                    runListSize = newSize;
                }

                //NOTE: If this is a performance hog, the algorithm can be rewritten to be top-down and threads can be woken up as soon as they are reordered. So far, I've seen this has negligible overheads though.
                if (parallelThreads < runListSize) {
                    //Randomly shuffle thread list to avoid systemic biases and reduce contention on cache hierarchy (Fisher-Yates shuffle)
                    for (uint32_t i = runListSize-1; i > 0; i--) {
                        uint32_t j = rnd.randInt(i); //j is in {0,...,i}
                        uint32_t itid = runList[i];
                        uint32_t jtid = runList[j];

                        runList[i] = jtid;
                        runList[j] = itid;

                        threadList[itid].lastIdx = j;
                        threadList[jtid].lastIdx = i;
                    }
                }
            }
        }

        inline void checkRunList(uint32_t tid) {
            while (runningThreads < parallelThreads && curThreadIdx < runListSize) {
                //Wake next thread
                uint32_t idx = curThreadIdx++;
                uint32_t wtid = runList[idx];
                if (threadList[wtid].state == WAITING) {
                    DEBUG_BARRIER("[%d] Waking %d runningThreads %d", tid, wtid, runningThreads);
                    threadList[wtid].state = RUNNING; //must be set before writing to futexWord to avoid wakeup race
                    threadList[wtid].lastIdx = idx;
                    bool succ = __sync_bool_compare_and_swap(&threadList[wtid].futexWord, 1, 0);
                    if (!succ) panic("Wakeup race in barrier?");
                    syscall(SYS_futex, &threadList[wtid].futexWord, FUTEX_WAKE, 1, NULL, NULL, 0);
                    runningThreads++;
                } else {
                    DEBUG_BARRIER("[%d] Skipping %d state %d", tid, wtid, threadList[wtid].state);
                }
            }
        }

        void tryWakeNext(uint32_t tid) {
            checkRunList(tid); //wake up threads on this phase, may reach EOP
            checkEndPhase(tid); //see if we've reached EOP, execute if if so
            checkRunList(tid); //if we started a new phase, wake up threads
        }
};

#endif  // BARRIER_H_
