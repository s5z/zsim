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

#include "cpuenum.h"
#include "log.h"
#include "virt/common.h"

// SYS_getcpu

// Call without CPU from vdso, with CPU from syscall version
void VirtGetcpu(uint32_t tid, uint32_t cpu, ADDRINT arg0, ADDRINT arg1) {
    unsigned resCpu;
    unsigned resNode = 0;
    if (!arg0) {
        info("getcpu() called with null cpu arg");
    }
    if (!safeCopy((unsigned*)arg0, &resCpu)) {
        info("getcpu() called with invalid cpu arg");
        return;
    }
    if (arg1 && !safeCopy((unsigned*)arg1, &resNode)) {
        info("getcpu() called with invalid node arg");
        return;
    }

    trace(TimeVirt, "Patching getcpu()");
    trace(TimeVirt, "Orig cpu %d, node %d, patching core %d / node 0", resCpu, resNode, cpu);
    resCpu = cpu;
    resNode = 0;

    safeCopy(&resCpu, (unsigned*)arg0);
    if (arg1) safeCopy(&resNode, (unsigned*)arg1);
}

PostPatchFn PatchGetcpu(PrePatchArgs args) {
    uint32_t cpu = cpuenumCpu(procIdx, getCid(args.tid));  // still valid, may become invalid when we leave()
    assert(cpu != (uint32_t)-1);
    return [cpu](PostPatchArgs args) {
        trace(TimeVirt, "[%d] Post-patching SYS_getcpu", args.tid);
        ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
        ADDRINT arg1 = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
        VirtGetcpu(args.tid, cpu, arg0, arg1);
        return PPA_NOTHING;
    };
}

// Scheduler affinity

PostPatchFn PatchSchedGetaffinity(PrePatchArgs args) {
    return [](PostPatchArgs args) {
        uint32_t size = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
        cpu_set_t* set = (cpu_set_t*)PIN_GetSyscallArgument(args.ctxt, args.std, 2);
        if (set) { //TODO: use SafeCopy, this can still segfault
            CPU_ZERO_S(size, set);
            std::vector<bool> cpumask = cpuenumMask(procIdx);
            for (uint32_t i = 0; i < MIN(cpumask.size(), size*8 /*size is in bytes, supports 1 cpu/bit*/); i++) {
                if (cpumask[i]) CPU_SET_S(i, (size_t)size, set);
            }
        }
        info("[%d] Post-patching SYS_sched_getaffinity size %d cpuset %p", args.tid, size, set);
        return PPA_NOTHING;
    };
}

PostPatchFn PatchSchedSetaffinity(PrePatchArgs args) {
    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT) SYS_getpid);  // squash
    return [](PostPatchArgs args) {
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EPERM);  // make it a proper failure
        return PPA_NOTHING;
    };
}

