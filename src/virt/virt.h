/** $glic$
 * Copyright (C) 2012-2014 by Massachusetts Institute of Technology
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

#ifndef VIRT_VIRT_H_
#define VIRT_VIRT_H_

// External virt interface

#include "pin.H"

enum PostPatchAction {
    PPA_NOTHING,
    PPA_USE_NOP_PTRS,
    PPA_USE_JOIN_PTRS,
};

void VirtInit();  // per-process, not global
void VirtSyscallEnter(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std, const char* patchRoot, bool isNopThread);
PostPatchAction VirtSyscallExit(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std);

// VDSO / external virt functions
void VirtGettimeofday(uint32_t tid, ADDRINT arg0);
void VirtTime(uint32_t tid, REG* retVal, ADDRINT arg0);
void VirtClockGettime(uint32_t tid, ADDRINT arg0, ADDRINT arg1);
void VirtGetcpu(uint32_t tid, uint32_t cpu, ADDRINT arg0, ADDRINT arg1);

// Time virtualization direct functions
void VirtCaptureClocks(bool isDeffwd);  // called on start and ffwd to get all clocks together
uint64_t VirtGetPhaseRDTSC();

#endif  // VIRT_VIRT_H_
