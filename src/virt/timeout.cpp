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

#include "constants.h"
#include "log.h"
#include "scheduler.h"
#include "process_tree.h"
#include "virt/common.h"
#include "virt/syscall_name.h"
#include "virt/time_conv.h"
#include "zsim.h"

static struct timespec fakeTimeouts[MAX_THREADS]; //for syscalls that use timespec to indicate a timeout
static bool inFakeTimeoutMode[MAX_THREADS];

static bool SkipTimeoutVirt(PrePatchArgs args) {
    // having both conditions ensures that we don't virtualize in the interim of toggling ff ON
    return args.isNopThread || zinfo->procArray[procIdx]->isInFastForward();
}

// Helper function, see /usr/include/linux/futex.h
static bool isFutexWaitOp(int op) {
    switch (op & FUTEX_CMD_MASK) { //handles PRIVATE / REALTIME as well
        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
        case FUTEX_WAIT_REQUEUE_PI:
            return true;
        default:
            return false;
    }
}

static bool isFutexWakeOp(int op) {
    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAKE:
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
        case FUTEX_WAKE_OP:
        case FUTEX_WAKE_BITSET:
        case FUTEX_CMP_REQUEUE_PI:
            return true;
        default:
            return false;
    }
}


static int getTimeoutArg(int syscall) {
    if (syscall == SYS_poll) return 2;
    return 3;  // futex, epoll_wait, epoll_pwait
}

static bool PrePatchTimeoutSyscall(uint32_t tid, CONTEXT* ctxt, SYSCALL_STANDARD std, int syscall) {
    assert(!inFakeTimeoutMode[tid]);  // canary: this will probably fail...
    int64_t waitNsec = 0;

    // Per-syscall manipulation. This code either succeeds, fakes timeout value and sets waitNsec, or returns false
    int timeoutArg = getTimeoutArg(syscall);
    if (syscall == SYS_futex) {
        // Check preconditions
        assert(timeoutArg == 3);
        int* uaddr = (int*) PIN_GetSyscallArgument(ctxt, std, 0);
        int op = (int) PIN_GetSyscallArgument(ctxt, std, 1);
        const struct timespec* timeout = (const struct timespec*) PIN_GetSyscallArgument(ctxt, std, 3);

        //info("FUTEX op %d  waitOp %d uaddr %p ts %p", op, isFutexWaitOp(op), uaddr, timeout);
        if (!(uaddr && isFutexWaitOp(op) && timeout)) return false;  // not a timeout FUTEX_WAIT

        waitNsec = timeout->tv_sec*1000000000L + timeout->tv_nsec;

        if (op | FUTEX_CLOCK_REALTIME) {
            // NOTE: FUTEX_CLOCK_REALTIME is not a documented interface AFAIK, but looking at the Linux source code + with some verification, this is the xlat
            uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
            uint64_t simNs = cyclesToNs(zinfo->globPhaseCycles);
            uint64_t offsetNs = simNs + zinfo->clockDomainInfo[domain].realtimeOffsetNs;
            //info(" REALTIME FUTEX: %ld %ld %ld %ld", waitNsec, simNs, offsetNs, waitNsec-offsetNs);
            waitNsec = (waitNsec > (int64_t)offsetNs)? (waitNsec - offsetNs) : 0;
        }

        if (waitNsec <= 0) return false;  // while technically waiting, this does not block. I'm guessing this is done for trylocks? It's weird.

        fakeTimeouts[tid].tv_sec = 0;
        fakeTimeouts[tid].tv_nsec = 20*1000*1000;  // timeout every 20ms of actual host time
        PIN_SetSyscallArgument(ctxt, std, 3, (ADDRINT)&fakeTimeouts[tid]);
    } else {
        assert(syscall == SYS_epoll_wait || syscall == SYS_epoll_pwait || syscall == SYS_poll);
        int timeout = (int) PIN_GetSyscallArgument(ctxt, std, timeoutArg);
        if (timeout <= 0) return false;
        //info("[%d] pre-patch epoll_wait/pwait", tid);

        PIN_SetSyscallArgument(ctxt, std, timeoutArg, 20); // 20ms timeout
        waitNsec = ((uint64_t)timeout)*1000*1000;  // timeout is in ms
    }

    //info("[%d] pre-patch %s (%d) waitNsec = %ld", tid, GetSyscallName(syscall), syscall, waitNsec);

    uint64_t waitCycles = waitNsec*zinfo->freqMHz/1000;
    uint64_t waitPhases = waitCycles/zinfo->phaseLength;
    if (waitPhases < 2) waitPhases = 2;  // at least wait 2 phases; this should basically eliminate the chance that we get a SIGSYS before we start executing the syscal instruction
    uint64_t wakeupPhase = zinfo->numPhases + waitPhases;

    /*volatile uint32_t* futexWord =*/ zinfo->sched->markForSleep(procIdx, tid, wakeupPhase);  // we still want to mark for sleep, bear with me...
    inFakeTimeoutMode[tid] = true;
    return true;
}

static bool PostPatchTimeoutSyscall(uint32_t tid, CONTEXT* ctxt, SYSCALL_STANDARD std, int syscall, ADDRINT prevIp, ADDRINT timeoutArgVal) {
    assert(inFakeTimeoutMode[tid]);
    int res = (int)PIN_GetSyscallNumber(ctxt, std);

    // Decide if it timed out
    bool timedOut;
    if (syscall == SYS_futex) {
        timedOut = (res == -ETIMEDOUT);
    } else {
        timedOut = (res == 0);
    }

    bool isSleeping = zinfo->sched->isSleeping(procIdx, tid);

    // Decide whether to retry
    bool retrySyscall;
    if (!timedOut) {
        if (isSleeping) zinfo->sched->notifySleepEnd(procIdx, tid);
        retrySyscall = false;
    } else {
        retrySyscall = isSleeping;
    }

    if (retrySyscall && zinfo->procArray[procIdx]->isInFastForward()) {
        warn("[%d] Fast-forwarding started, not retrying timeout syscall (%s)", tid, GetSyscallName(syscall));
        retrySyscall = false;
        assert(isSleeping);
        zinfo->sched->notifySleepEnd(procIdx, tid);
    }

    if (retrySyscall) {
        // ADDRINT curIp = PIN_GetContextReg(ctxt, REG_INST_PTR);
        //info("[%d] post-patch, retrying, IP: 0x%lx -> 0x%lx", tid, curIp, prevIp);
        PIN_SetContextReg(ctxt, REG_INST_PTR, prevIp);
        PIN_SetSyscallNumber(ctxt, std, syscall);
    } else {
        // Restore timeout arg
        PIN_SetSyscallArgument(ctxt, std, getTimeoutArg(syscall), timeoutArgVal);
        inFakeTimeoutMode[tid] = false;

        // Restore arg? I don't think we need this!
        /*if (syscall == SYS_futex) {
            PIN_SetSyscallNumber(ctxt, std, -ETIMEDOUT);
        } else {
            assert(syscall == SYS_epoll_wait || syscall == SYS_epoll_pwait || syscall == SYS_poll);
            PIN_SetSyscallNumber(ctxt, std, 0); //no events returned
        }*/
    }

    //info("[%d] post-patch %s (%d), timedOut %d, sleeping (orig) %d, retrying %d, orig res %d, patched res %d", tid, GetSyscallName(syscall), syscall, timedOut, isSleeping, retrySyscall, res, (int)PIN_GetSyscallNumber(ctxt, std));
    return retrySyscall;
}

/* Notify scheduler about FUTEX_WAITs woken up by FUTEX_WAKEs, FUTEX_WAKE entries, and FUTEX_WAKE exits */

struct FutexInfo {
    int op;
    int val;
};

FutexInfo PrePatchFutex(uint32_t tid, CONTEXT* ctxt, SYSCALL_STANDARD std) {
    FutexInfo fi;
    fi.op = (int) PIN_GetSyscallArgument(ctxt, std, 1);
    fi.val = (int) PIN_GetSyscallArgument(ctxt, std, 2);
    if (isFutexWakeOp(fi.op)) {
        zinfo->sched->notifyFutexWakeStart(procIdx, tid, fi.val);
    }
    return fi;
}

void PostPatchFutex(uint32_t tid, FutexInfo fi, CONTEXT* ctxt, SYSCALL_STANDARD std) {
    int res = (int) PIN_GetSyscallNumber(ctxt, std);
    if (isFutexWaitOp(fi.op) && res == 0) {
        zinfo->sched->notifyFutexWaitWoken(procIdx, tid);
    } else if (isFutexWakeOp(fi.op) && res >= 0) {
        /* In contrast to the futex manpage, from the kernel's futex.c
         * (do_futex), WAKE and WAKE_OP return the number of threads woken up,
         * but the REQUEUE and CMP_REQUEUE and REQUEUE_PI ops return the number
         * of threads woken up + requeued. However, these variants
         * (futex_requeue) first try to wake the specified threads, then
         * requeue as many other threads as they can.
         *
         * Therefore, this wokenUp expression should be correct for all variants
         * of SYS_futex that wake up threads (WAKE, REQUEUE, CMP_REQUEUE, ...)
         */
        uint32_t wokenUp = std::min(res, fi.val);
        zinfo->sched->notifyFutexWakeEnd(procIdx, tid, wokenUp);
    }
}

PostPatchFn PatchTimeoutSyscall(PrePatchArgs args) {
    if (SkipTimeoutVirt(args)) return NullPostPatch;

    int syscall = PIN_GetSyscallNumber(args.ctxt, args.std);
    assert_msg(syscall == SYS_futex || syscall == SYS_epoll_wait || syscall == SYS_epoll_pwait || syscall == SYS_poll,
            "Invalid timeout syscall %d", syscall);

    FutexInfo fi = {0, 0};
    if (syscall == SYS_futex) fi = PrePatchFutex(args.tid, args.ctxt, args.std);

    if (PrePatchTimeoutSyscall(args.tid, args.ctxt, args.std, syscall)) {
        ADDRINT prevIp = PIN_GetContextReg(args.ctxt, REG_INST_PTR);
        ADDRINT timeoutArgVal = PIN_GetSyscallArgument(args.ctxt, args.std, getTimeoutArg(syscall));
        return [syscall, prevIp, timeoutArgVal, fi](PostPatchArgs args) {
            if (PostPatchTimeoutSyscall(args.tid, args.ctxt, args.std, syscall, prevIp, timeoutArgVal)) {
                return PPA_USE_NOP_PTRS;  // retry
            } else {
                if (syscall == SYS_futex) PostPatchFutex(args.tid, fi, args.ctxt, args.std);
                return PPA_USE_JOIN_PTRS;  // finish
            }
        };
    } else {
        if (syscall == SYS_futex) {
            return [fi](PostPatchArgs args) {
                PostPatchFutex(args.tid, fi, args.ctxt, args.std);
                return PPA_NOTHING;
            };
        } else {
            return NullPostPatch;
        }
    }
}

