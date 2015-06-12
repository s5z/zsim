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

#include <syscall.h>
#include "constants.h"
#include "log.h"
#include "virt/common.h"
#include "virt/syscall_name.h"
#include "virt/virt.h"

#define MAX_SYSCALLS 350  // doesn't need to be accurate

PrePatchFn prePatchFunctions[MAX_SYSCALLS];
PostPatchFn postPatchFunctions[MAX_THREADS];

const PostPatchFn NullPostPatch = [](PostPatchArgs) {return PPA_NOTHING;};

// Common prepatch functions
PostPatchFn NullPatch(PrePatchArgs) {
    return NullPostPatch;
}

PostPatchFn WarnTimingRelated(PrePatchArgs args) {
    uint32_t syscall = PIN_GetSyscallNumber(args.ctxt, args.std);
    warn("[%d] Executing unvirtualized potentially timing-sensitive syscall: %s (%d)", args.tid, GetSyscallName(syscall), syscall);
    return NullPostPatch;
}

// Define all patch functions
#define PF(syscall, pfn) PostPatchFn pfn(PrePatchArgs args);
#include "virt/patchdefs.h"
#undef PF

void VirtInit() {
    for (uint32_t i = 0; i < MAX_SYSCALLS; i++) prePatchFunctions[i] = NullPatch;

    // Issue warnings on timing-sensitive syscalls
    for (uint32_t syscall : {SYS_select, SYS_getitimer, SYS_alarm, SYS_setitimer, SYS_semop,
            SYS_gettimeofday, SYS_times, SYS_rt_sigtimedwait, SYS_time, SYS_futex, SYS_mq_timedsend,
            SYS_mq_timedreceive, SYS_pselect6, SYS_ppoll}) {
        prePatchFunctions[syscall] = WarnTimingRelated;
    }

    // Bind all patch functions
    #define PF(syscall, pfn) prePatchFunctions[syscall] = pfn;
    #include "virt/patchdefs.h"
    #undef PF
}


// Dispatch methods
void VirtSyscallEnter(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std, const char* patchRoot, bool isNopThread) {
    uint32_t syscall = PIN_GetSyscallNumber(ctxt, std);
    if (syscall >= MAX_SYSCALLS) {
        warn("syscall %d out of range", syscall);
        postPatchFunctions[tid] = NullPostPatch;
    } else {
        postPatchFunctions[tid] = prePatchFunctions[syscall]({tid, ctxt, std, patchRoot, isNopThread});
    }
}

PostPatchAction VirtSyscallExit(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std) {
    return postPatchFunctions[tid]({tid, ctxt, std});
}

