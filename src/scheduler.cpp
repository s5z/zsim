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

#include "scheduler.h"
#include <fstream>
#include <regex>
#include <sys/stat.h>
#include "config.h" // for ParseList
#include "pin.H"
#include "process_tree.h"
#include "profile_stats.h"
#include "str.h"
#include "virt/syscall_name.h"

//The scheduler class started simple, but at some point having it all in the header is too ridiculous. Migrate non perf-intensive calls here! (all but sync, really)

#define WATCHDOG_INTERVAL_USEC (50)
#define WATCHDOG_MAX_MULTIPLER (40) //50us-2ms waits
#define WATCHDOG_STALL_THRESHOLD (100)

//#define DEBUG_FL(args...) info(args)
#define DEBUG_FL(args...)

//#define DEBUG_FUTEX(args...) info(args)
#define DEBUG_FUTEX(args...)

// Unlike glibc's sleep functions suck, this ensures guaranteed minimum sleep time
static void TrueSleep(uint32_t usecs) {
    struct timespec req;
    struct timespec rem;

    req.tv_sec = usecs/1000000;
    req.tv_nsec = (usecs*1000) % 1000000000;

    while (req.tv_sec != 0 || req.tv_nsec != 0) {
        int res = syscall(SYS_nanosleep, &req, &rem); //we don't call glibc's nanosleep because errno is not thread-safe in pintools.
        if (res == 0) break;
        req = rem;
        if (res != -EINTR && res != 0) panic("nanosleep() returned an unexpected error code %d", res);
        //info("nanosleep() interrupted!");
    }
}

/* Hacky way to figure out if a thread is sleeping on a certain futex.
 *
 * Uses /proc/<pid>/task/<tid>/syscall, which is only set when the process is
 * actually sleeping on the syscall, not just in the kernel (see Linux kernel
 * docs). This interface has been available since ~2008.
 */
bool IsSleepingInFutex(uint32_t linuxPid, uint32_t linuxTid, uintptr_t futexAddr) {
    std::string fname = "/proc/" + Str(linuxPid) + "/task/" + Str(linuxTid) + "/syscall";
    std::ifstream fs(fname);
    if (!fs.is_open()) {
        warn("Could not open %s", fname.c_str());
        return false;
    }

    std::stringstream ss;
    ss << fs.rdbuf();
    fs.close();

    std::vector<std::string> argList = ParseList<std::string>(ss.str());
    bool match = argList.size() >= 2 &&
        strtoul(argList[0].c_str(), nullptr, 0) == SYS_futex &&
        (uintptr_t)strtoul(argList[1].c_str(), nullptr, 0) == futexAddr;
    //info("%s | %s | SYS_futex = %d futexAddr = 0x%lx | match = %d ", ss.str().c_str(), Str(argList).c_str(), SYS_futex, futexAddr, match);
    return match;
}


void Scheduler::watchdogThreadFunc() {
    info("Started scheduler watchdog thread");
    uint64_t lastPhase = 0;
    int multiplier = 1;
    uint64_t lastMs = 0;
    uint64_t fakeLeaveStalls = 0;
    while (true) {
        TrueSleep(multiplier*WATCHDOG_INTERVAL_USEC);

        if (zinfo->terminationConditionMet) {
            // Synchronize to avoid racing with EndOfPhaseActions code
            // (zinfo->terminationConditionMet is set on EndOfPhaseActions,
            // which has schedLock held, we must let it finish)
            futex_lock(&schedLock);
            info("Terminating scheduler watchdog thread");
            futex_unlock(&schedLock);
            SimEnd();
        }

        //Fastpath (unlocked, benign read races, only modifies local state)
        if (lastPhase != curPhase && pendingPidCleanups.size() == 0) {
            lastPhase = curPhase;
            fakeLeaveStalls = 0;
            if (multiplier < WATCHDOG_MAX_MULTIPLER) multiplier++;
            continue;
        }

        //if (lastPhase == curPhase && scheduledThreads == outQueue.size() && !sleepQueue.empty()) info("Mult %d curPhase %ld", multiplier, curPhase);

        futex_lock(&schedLock);

        if (lastPhase == curPhase && !fakeLeaves.empty() && (fakeLeaves.front()->th->futexJoin.action != FJA_WAKE)) {
            if (++fakeLeaveStalls >= WATCHDOG_STALL_THRESHOLD) {
                info("Detected possible stall due to fake leaves (%ld current)", fakeLeaves.size());
                // Uncomment to print all leaves
                FakeLeaveInfo* pfl = fakeLeaves.front();
                while (pfl) {
                    info(" [%d/%d] %s (%d) @ 0x%lx", getPid(pfl->th->gid), getTid(pfl->th->gid), GetSyscallName(pfl->syscallNumber), pfl->syscallNumber, pfl->pc);
                    pfl = pfl->next;
                }

                // Trigger a leave() on the first process, if the process's blacklist regex allows it
                FakeLeaveInfo* fl = fakeLeaves.front();
                ThreadInfo* th = fl->th;
                uint32_t pid = getPid(th->gid);
                uint32_t tid = getTid(th->gid);
                uint32_t cid = th->cid;

                const g_string& sbRegexStr = zinfo->procArray[pid]->getSyscallBlacklistRegex();
                std::regex sbRegex(sbRegexStr.c_str());
                if (std::regex_match(GetSyscallName(fl->syscallNumber), sbRegex)) {
                    // If this is the last leave we catch, it is the culprit for sure -> blacklist it
                    // Over time, this will blacklist every blocking syscall
                    // The root reason for being conservative though is that we don't have a sure-fire
                    // way to distinguish IO waits from truly blocking syscalls (TODO)
                    if (fakeLeaves.size() == 1) {
                        info("Blacklisting from future fake leaves: [%d] %s @ 0x%lx | arg0 0x%lx arg1 0x%lx", pid, GetSyscallName(fl->syscallNumber), fl->pc, fl->arg0, fl->arg1);
                        blockingSyscalls[pid].insert(fl->pc);
                    }

                    uint64_t pc = fl->pc;
                    do {
                        finishFakeLeave(th);

                        futex_unlock(&schedLock);
                        leave(pid, tid, cid);
                        futex_lock(&schedLock);

                        // also do real leave for other threads blocked at the same pc ...
                        fl = fakeLeaves.front();
                        if (fl == nullptr || getPid(th->gid) != pid || fl->pc != pc)
                            break;
                        th = fl->th;
                        tid = getTid(th->gid);
                        cid = th->cid;
                        // ... until a lower bound on queue size, in order to make blacklist work
                    } while (fakeLeaves.size() > 8);
                } else {
                    info("Skipping, [%d] %s @ 0x%lx | arg0 0x%lx arg1 0x%lx does not match blacklist regex (%s)",
                            pid, GetSyscallName(fl->syscallNumber), fl->pc, fl->arg0, fl->arg1, sbRegexStr.c_str());
                }
                fakeLeaveStalls = 0;
            }
        } else {
            fakeLeaveStalls = 0;
        }

        if (lastPhase == curPhase && scheduledThreads == outQueue.size() && !sleepQueue.empty()) {
            //info("Watchdog Thread: Sleep dep detected...")
            int64_t wakeupPhase = sleepQueue.front()->wakeupPhase;
            int64_t wakeupCycles = (wakeupPhase - curPhase)*zinfo->phaseLength;
            int64_t wakeupUsec = (wakeupCycles > 0)? wakeupCycles/zinfo->freqMHz : 0;

            //info("Additional usecs of sleep %ld", wakeupUsec);
            if (wakeupUsec > 10*1000*1000) warn("Watchdog sleeping for a long time due to long sleep, %ld secs", wakeupUsec/1000/1000);

            futex_unlock(&schedLock);
            TrueSleep(WATCHDOG_INTERVAL_USEC + wakeupUsec);
            futex_lock(&schedLock);

            if (lastPhase == curPhase && scheduledThreads == outQueue.size() && !sleepQueue.empty()) {
                ThreadInfo* sth = sleepQueue.front();
                uint64_t curMs = curPhase*zinfo->phaseLength/zinfo->freqMHz/1000;
                uint64_t endMs = sth->wakeupPhase*zinfo->phaseLength/zinfo->freqMHz/1000;
                (void)curMs; (void)endMs; //make gcc happy
                if (curMs > lastMs + 1000) {
                    info("Watchdog Thread: Driving time forward to avoid deadlock on sleep (%ld -> %ld ms)", curMs, endMs);
                    lastMs += 1000;
                }
                while (sth->state == SLEEPING) {
                    idlePhases.inc();
                    callback(); //sth will eventually get woken up

                    if (futex_haswaiters(&schedLock)) {
                        //happens commonly with multiple sleepers and very contended I/O...
                        //info("Sched: Threads waiting on advance, startPhase %ld curPhase %ld", lastPhase, curPhase);
                        break;
                    }

                    if (zinfo->terminationConditionMet) {
                        info("Termination condition met inside watchdog thread loop, exiting");
                        break;
                    }
                }
                idlePeriods.inc();
                multiplier = 0;
            }
        }

        if (multiplier < WATCHDOG_MAX_MULTIPLER) {
            multiplier++;
        }

        lastPhase = curPhase;

        //Lazily clean state of processes that terminated abruptly
        //NOTE: For now, we rely on the process explicitly telling us that it's going to terminate.
        //We could make this self-checking by periodically checking for liveness of the processes we're supposedly running.
        //The bigger problem is that if we get SIGKILL'd, we may not even leave a consistent zsim state behind.
        while (pendingPidCleanups.size()) {
            std::pair<uint32_t, uint32_t> p = pendingPidCleanups.back();
            uint32_t pid = p.first; //the procIdx pid
            uint32_t osPid = p.second;

            std::stringstream ss;
            ss << "/proc/" << osPid;
            struct stat dummy;
            if (stat(ss.str().c_str(), &dummy) == 0) {
                info("[watchdog] Deferring cleanup of pid %d (%d), not finished yet", pid, osPid);
                break;
            }

            pendingPidCleanups.pop_back(); //must happen while we have the lock

            futex_unlock(&schedLock);
            processCleanup(pid);
            futex_lock(&schedLock);
        }

        if (terminateWatchdogThread) {
            futex_unlock(&schedLock);
            break;
        } else {
            futex_unlock(&schedLock);
        }
    }
    info("Finished scheduler watchdog thread");
}

void Scheduler::threadTrampoline(void* arg) {
    Scheduler* sched = static_cast<Scheduler*>(arg);
    sched->watchdogThreadFunc();
}

void Scheduler::startWatchdogThread() {
    PIN_SpawnInternalThread(threadTrampoline, this, 64*1024, nullptr);
}


// Accurate join-leave implementation
void Scheduler::syscallLeave(uint32_t pid, uint32_t tid, uint32_t cid, uint64_t pc, int syscallNumber, uint64_t arg0, uint64_t arg1) {
    futex_lock(&schedLock);
    uint32_t gid = getGid(pid, tid);
    ThreadInfo* th = contexts[cid].curThread;
    assert(th->gid == gid);
    assert_msg(th->cid == cid, "%d != %d", th->cid, cid);
    assert(th->state == RUNNING);
    assert_msg(pid < blockingSyscalls.size(), "%d >= %ld?", pid, blockingSyscalls.size());

    bool blacklisted = blockingSyscalls[pid].find(pc) != blockingSyscalls[pid].end();
    if (blacklisted || th->markedForSleep) {
        DEBUG_FL("%s @ 0x%lx calling leave(), reason: %s", GetSyscallName(syscallNumber), pc, blacklisted? "blacklist" : "sleep");
        futex_unlock(&schedLock);
        leave(pid, tid, cid);
    } else {
        DEBUG_FL("%s @ 0x%lx skipping leave()", GetSyscallName(syscallNumber), pc);
        FakeLeaveInfo* si = new FakeLeaveInfo(pc, th, syscallNumber, arg0, arg1);
        fakeLeaves.push_back(si);
        // FIXME(dsm): zsim.cpp's SyscallEnter may be checking whether we are in a syscall and not calling us.
        // If that's the case, this would be stale, which may lead to some false positives/negatives
        futex_unlock(&schedLock);
    }
}

/* Wake/wait matching code */

// External interface, must be non-blocking
void Scheduler::notifyFutexWakeStart(uint32_t pid, uint32_t tid, uint32_t maxWakes) {
    futex_lock(&schedLock);
    ThreadInfo* th = gidMap[getGid(pid, tid)];
    DEBUG_FUTEX("[%d/%d] wakeStart max %d", pid, tid, maxWakes);
    assert(th->futexJoin.action == FJA_NONE);

    // Programs sometimes call FUTEX_WAIT with maxWakes = UINT_MAX to wake
    // everyone waiting on it; we cap to a reasonably high number to avoid
    // overflows on maxAllowedFutexWakeups
    maxWakes = MIN(maxWakes, 1<<24 /*16M wakes*/);

    maxAllowedFutexWakeups += maxWakes;
    th->futexJoin.maxWakes = maxWakes;
    futex_unlock(&schedLock);
}

void Scheduler::notifyFutexWakeEnd(uint32_t pid, uint32_t tid, uint32_t wokenUp) {
    futex_lock(&schedLock);
    ThreadInfo* th = gidMap[getGid(pid, tid)];
    DEBUG_FUTEX("[%d/%d] wakeEnd woken %d", pid, tid, wokenUp);
    th->futexJoin.action = FJA_WAKE;
    th->futexJoin.wokenUp = wokenUp;
    futex_unlock(&schedLock);
}

void Scheduler::notifyFutexWaitWoken(uint32_t pid, uint32_t tid) {
    futex_lock(&schedLock);
    ThreadInfo* th = gidMap[getGid(pid, tid)];
    DEBUG_FUTEX("[%d/%d] waitWoken", pid, tid);
    th->futexJoin = {FJA_WAIT, 0, 0};
    futex_unlock(&schedLock);
}

// Internal, called with schedLock held
void Scheduler::futexWakeJoin(ThreadInfo* th) {  // may release schedLock
    assert(th->futexJoin.action == FJA_WAKE);

    uint32_t maxWakes = th->futexJoin.maxWakes;
    uint32_t wokenUp = th->futexJoin.wokenUp;

    // Adjust allowance
    assert(maxWakes <= maxAllowedFutexWakeups);
    assert(wokenUp <= maxWakes);
    maxAllowedFutexWakeups -= (maxWakes - wokenUp);

    assert(unmatchedFutexWakeups <= maxAllowedFutexWakeups); // should panic...

    DEBUG_FUTEX("Futex wake matching %d %d", unmatchedFutexWakeups, maxAllowedFutexWakeups);

    while (true) {
        futex_unlock(&schedLock);
        uint64_t startNs = getNs();
        uint32_t iters = 0;
        while (wokenUp > unmatchedFutexWakeups) {
            TrueSleep(10*(1 + iters));  // linear backoff, start small but avoid overwhelming the OS with short sleeps
            iters++;
            uint64_t curNs = getNs();
            if (curNs - startNs > (2L<<31L) /* ~2s */) {
                futex_lock(&schedLock);
                warn("Futex wake matching failed (%d/%d) (external/ff waiters?)", unmatchedFutexWakeups, wokenUp);
                unmatchedFutexWakeups = 0;
                maxAllowedFutexWakeups -= wokenUp;
                return;
            }
        }

        futex_lock(&schedLock);

        // Recheck after acquire, may have concurrent wakes here
        if (wokenUp <= unmatchedFutexWakeups) {
            unmatchedFutexWakeups -= wokenUp;
            maxAllowedFutexWakeups -= wokenUp;
            break;
        }
    }

    DEBUG_FUTEX("Finished futex wake matching");
}

void Scheduler::futexWaitJoin(ThreadInfo* th) {
    assert(th->futexJoin.action == FJA_WAIT);
    if (unmatchedFutexWakeups >= maxAllowedFutexWakeups) {
        warn("External futex wakes? (%d/%d)", unmatchedFutexWakeups, maxAllowedFutexWakeups);
    } else {
        unmatchedFutexWakeups++;
    }
}

void Scheduler::finishFakeLeave(ThreadInfo* th) {
    assert(th->fakeLeave);
    DEBUG_FL("%s (%d)  @ 0x%lx finishFakeLeave()", GetSyscallName(th->fakeLeave->syscallNumber), th->fakeLeave->syscallNumber, th->fakeLeave->pc);
    assert_msg(th->state == RUNNING, "gid 0x%x invalid state %d", th->gid, th->state);
    FakeLeaveInfo* si = th->fakeLeave;
    fakeLeaves.remove(si);
    delete si;
    assert(th->fakeLeave == nullptr);
}

void Scheduler::waitUntilQueued(ThreadInfo* th) {
    uint64_t startNs = getNs();
    uint32_t sleepUs = 1;
    while(!IsSleepingInFutex(th->linuxPid, th->linuxTid, (uintptr_t)&schedLock)) {
        TrueSleep(sleepUs++); // linear backoff, start small but avoid overwhelming the OS with short sleeps
        uint64_t curNs = getNs();
        if (curNs - startNs > (2L<<31L) /* ~2s */) {
            warn("waitUntilQueued for pid %d tid %d timed out", getPid(th->gid), getTid(th->gid));
            return;
        }
    }
}

