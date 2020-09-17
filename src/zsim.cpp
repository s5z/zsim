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

/* The Pin-facing part of the simulator */

#include "zsim.h"
#include <algorithm>
#define _SIGNAL_H
#include <bits/signum.h>
#undef _SIGNAL_H
#include <dlfcn.h>
#include <execinfo.h>
#include <fstream>
#include <iostream>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include "access_tracing.h"
#include "constants.h"
#include "contention_sim.h"
#include "core.h"
#include "cpuenum.h"
#include "cpuid.h"
#include "debug_zsim.h"
#include "event_queue.h"
#include "galloc.h"
#include "init.h"
#include "log.h"
#include "pin.H"
#include "pin_cmd.h"
#include "process_tree.h"
#include "profile_stats.h"
#include "scheduler.h"
#include "stats.h"
#include "trace_driver.h"
#include "virt/virt.h"

//#include <signal.h> //can't include this, conflicts with PIN's

/* Command-line switches (used to pass info from harness that cannot be passed through the config file, most config is file-based) */

KNOB<INT32> KnobProcIdx(KNOB_MODE_WRITEONCE, "pintool",
        "procIdx", "0", "zsim process idx (internal)");

KNOB<INT32> KnobShmid(KNOB_MODE_WRITEONCE, "pintool",
        "shmid", "0", "SysV IPC shared memory id used when running in multi-process mode");

KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool",
        "config", "zsim.cfg", "config file name (only needed for the first simulated process)");

//We need to know these as soon as we start, otherwise we could not log anything until we attach and read the config
KNOB<bool> KnobLogToFile(KNOB_MODE_WRITEONCE, "pintool",
        "logToFile", "false", "true if all messages should be logged to a logfile instead of stdout/err");

KNOB<string> KnobOutputDir(KNOB_MODE_WRITEONCE, "pintool",
        "outputDir", "./", "absolute path to write output files into");



/* ===================================================================== */

INT32 Usage() {
    cerr << "zsim simulator pintool" << endl;
    cerr << KNOB_BASE::StringKnobSummary();
    cerr << endl;
    return -1;
}

/* Global Variables */

GlobSimInfo* zinfo;

/* Per-process variables */

uint32_t procIdx;
uint32_t lineBits; //process-local for performance, but logically global
Address procMask;

static ProcessTreeNode* procTreeNode;

//tid to cid translation
#define INVALID_CID ((uint32_t)-1)
#define UNINITIALIZED_CID ((uint32_t)-2) //Value set at initialization

static uint32_t cids[MAX_THREADS];

// Per TID core pointers (TODO: phase out cid/tid state --- this is enough)
Core* cores[MAX_THREADS];

static inline void clearCid(uint32_t tid) {
    assert(tid < MAX_THREADS);
    assert(cids[tid] != INVALID_CID);
    cids[tid] = INVALID_CID;
    cores[tid] = nullptr;
}

static inline void setCid(uint32_t tid, uint32_t cid) {
    assert(tid < MAX_THREADS);
    assert(cids[tid] == INVALID_CID);
    assert(cid < zinfo->numCores);
    cids[tid] = cid;
    cores[tid] = zinfo->cores[cid];
}

uint32_t getCid(uint32_t tid) {
    //assert(tid < MAX_THREADS); //these assertions are fine, but getCid is called everywhere, so they are expensive!
    uint32_t cid = cids[tid];
    //assert(cid != INVALID_CID);
    return cid;
}

// Internal function declarations
void EnterFastForward();
void ExitFastForward();

VOID SimThreadStart(THREADID tid);
VOID SimThreadFini(THREADID tid);
VOID SimEnd();

VOID HandleMagicOp(THREADID tid, ADDRINT op);

VOID FakeCPUIDPre(THREADID tid, REG eax, REG ecx);
VOID FakeCPUIDPost(THREADID tid, ADDRINT* eax, ADDRINT* ebx, ADDRINT* ecx, ADDRINT* edx); //REG* eax, REG* ebx, REG* ecx, REG* edx);

VOID FakeRDTSCPost(THREADID tid, REG* eax, REG* edx);

VOID VdsoInstrument(INS ins);
VOID FFThread(VOID* arg);

/* Indirect analysis calls to work around PIN's synchronization
 *
 * NOTE(dsm): Be extremely careful when modifying this code. It is simple, but
 * it runs VERY frequently.  For example, with 24-byte structs on a fairly
 * unoptimized L1 cache, this code introduced a 4% overhead, down to 2% with
 * 32-byte structs. Also, be aware that a miss or unpredictable indirect jump
 * is about the worst kind of pain you can inflict on an ooo core, so ensure
 * that 1) there's no false sharing, and 2) these pointers are modified
 * sparingly.
 */

InstrFuncPtrs fPtrs[MAX_THREADS] ATTR_LINE_ALIGNED; //minimize false sharing

VOID PIN_FAST_ANALYSIS_CALL IndirectLoadSingle(THREADID tid, ADDRINT addr) {
    fPtrs[tid].loadPtr(tid, addr);
}

VOID PIN_FAST_ANALYSIS_CALL IndirectStoreSingle(THREADID tid, ADDRINT addr) {
    fPtrs[tid].storePtr(tid, addr);
}

VOID PIN_FAST_ANALYSIS_CALL IndirectBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    fPtrs[tid].bblPtr(tid, bblAddr, bblInfo);
}

VOID PIN_FAST_ANALYSIS_CALL IndirectRecordBranch(THREADID tid, ADDRINT branchPc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    fPtrs[tid].branchPtr(tid, branchPc, taken, takenNpc, notTakenNpc);
}

VOID PIN_FAST_ANALYSIS_CALL IndirectPredLoadSingle(THREADID tid, ADDRINT addr, BOOL pred) {
    fPtrs[tid].predLoadPtr(tid, addr, pred);
}

VOID PIN_FAST_ANALYSIS_CALL IndirectPredStoreSingle(THREADID tid, ADDRINT addr, BOOL pred) {
    fPtrs[tid].predStorePtr(tid, addr, pred);
}


//Non-simulation variants of analysis functions

// Join variants: Call join on the next instrumentation poin and return to analysis code
void Join(uint32_t tid) {
    assert(fPtrs[tid].type == FPTR_JOIN);
    uint32_t cid = zinfo->sched->join(procIdx, tid); //can block
    setCid(tid, cid);

    if (unlikely(zinfo->terminationConditionMet)) {
        info("Caught termination condition on join, exiting");
        zinfo->sched->leave(procIdx, tid, cid);
        SimEnd();
    }

    fPtrs[tid] = cores[tid]->GetFuncPtrs(); //back to normal pointers
}

VOID JoinAndLoadSingle(THREADID tid, ADDRINT addr) {
    Join(tid);
    fPtrs[tid].loadPtr(tid, addr);
}

VOID JoinAndStoreSingle(THREADID tid, ADDRINT addr) {
    Join(tid);
    fPtrs[tid].storePtr(tid, addr);
}

VOID JoinAndBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    Join(tid);
    fPtrs[tid].bblPtr(tid, bblAddr, bblInfo);
}

VOID JoinAndRecordBranch(THREADID tid, ADDRINT branchPc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    Join(tid);
    fPtrs[tid].branchPtr(tid, branchPc, taken, takenNpc, notTakenNpc);
}

VOID JoinAndPredLoadSingle(THREADID tid, ADDRINT addr, BOOL pred) {
    Join(tid);
    fPtrs[tid].predLoadPtr(tid, addr, pred);
}

VOID JoinAndPredStoreSingle(THREADID tid, ADDRINT addr, BOOL pred) {
    Join(tid);
    fPtrs[tid].predStorePtr(tid, addr, pred);
}

// NOP variants: Do nothing
VOID NOPLoadStoreSingle(THREADID tid, ADDRINT addr) {}
VOID NOPBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {}
VOID NOPRecordBranch(THREADID tid, ADDRINT addr, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {}
VOID NOPPredLoadStoreSingle(THREADID tid, ADDRINT addr, BOOL pred) {}

// FF is basically NOP except for basic blocks
VOID FFBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    if (unlikely(!procTreeNode->isInFastForward())) {
        SimThreadStart(tid);
    }
}

// FFI is instruction-based fast-forwarding
/* FFI works as follows: when in fast-forward, we install a special FF BBL func
 * ptr that counts instructions and checks whether we have reached the switch
 * point. Then, it exits FF, and queues an event that counts the instructions
 * where the app should be scheduled. That event cannot access any local state,
 * so when it hits the limit, it just makes the process enter FF. On that
 * entry, we install a special handler that advances to the next FFI point and
 * installs the normal FFI handlers (pretty much like joins work).
 *
 * REQUIREMENTS: Single-threaded during FF (non-FF can be MT)
 */

//TODO (dsm): Went for quick, dirty and contained here. This could use a cleanup.

// FFI state
static bool ffiEnabled;
static uint32_t ffiPoint;
static uint64_t ffiInstrsDone;
static uint64_t ffiInstrsLimit;
static bool ffiNFF;

//Track the non-FF instructions executed at the beginning of this and last interval.
//Can only be updated at ends of phase, by the NFF tracking event.
static uint64_t* ffiFFStartInstrs; //hack, needs to be a pointer, written to outside this process
static uint64_t* ffiPrevFFStartInstrs;

static const InstrFuncPtrs& GetFFPtrs();

VOID FFITrackNFFInterval() {
    assert(!procTreeNode->isInFastForward());
    assert(ffiInstrsDone < ffiInstrsLimit); //unless you have ~10-instr FFWds, this does not happen

    //Queue up an event to detect and end FF
    //Note vars are captured, so these lambdas can be called from any process
    uint64_t startInstrs = *ffiFFStartInstrs;
    uint32_t p = procIdx;
    uint64_t* _ffiFFStartInstrs = ffiFFStartInstrs;
    uint64_t* _ffiPrevFFStartInstrs = ffiPrevFFStartInstrs;
    auto ffiGet = [p, startInstrs]() { return zinfo->processStats->getProcessInstrs(p) - startInstrs; };
    auto ffiFire = [p, _ffiFFStartInstrs, _ffiPrevFFStartInstrs]() {
        info("FFI: Entering fast-forward for process %d", p);
        /* Note this is sufficient due to the lack of reinstruments on FF, and this way we do not need to touch global state */
        futex_lock(&zinfo->ffLock);
        assert(!zinfo->procArray[p]->isInFastForward());
        zinfo->procArray[p]->enterFastForward();
        futex_unlock(&zinfo->ffLock);
        *_ffiPrevFFStartInstrs = *_ffiFFStartInstrs;
        *_ffiFFStartInstrs = zinfo->processStats->getProcessInstrs(p);
    };
    zinfo->eventQueue->insert(makeAdaptiveEvent(ffiGet, ffiFire, 0, ffiInstrsLimit - ffiInstrsDone, MAX_IPC*zinfo->phaseLength));

    ffiNFF = true;
}

// Called on process start
VOID FFIInit() {
    const g_vector<uint64_t>& ffiPoints = procTreeNode->getFFIPoints();
    if (!ffiPoints.empty()) {
        if (zinfo->ffReinstrument) panic("FFI and reinstrumenting on FF switches are incompatible");
        ffiEnabled = true;
        ffiPoint = 0;
        ffiInstrsDone = 0;
        ffiInstrsLimit = ffiPoints[0];

        ffiFFStartInstrs = gm_calloc<uint64_t>(1);
        ffiPrevFFStartInstrs = gm_calloc<uint64_t>(1);
        ffiNFF = false;
        info("FFI mode initialized, %ld ffiPoints", ffiPoints.size());
        if (!procTreeNode->isInFastForward()) FFITrackNFFInterval();
    } else {
        ffiEnabled = false;
    }
}

//Set the next ffiPoint, or finish
VOID FFIAdvance() {
    const g_vector<uint64_t>& ffiPoints = procTreeNode->getFFIPoints();
    ffiPoint++;
    if (ffiPoint >= ffiPoints.size()) {
        info("Last ffiPoint reached, %ld instrs, limit %ld", ffiInstrsDone, ffiInstrsLimit);
        SimEnd();
    } else {
        info("ffiPoint reached, %ld instrs, limit %ld", ffiInstrsDone, ffiInstrsLimit);
        ffiInstrsLimit += ffiPoints[ffiPoint];
    }
}

VOID FFIBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    ffiInstrsDone += bblInfo->instrs;
    if (unlikely(ffiInstrsDone >= ffiInstrsLimit)) {
        FFIAdvance();
        assert(procTreeNode->isInFastForward());
        futex_lock(&zinfo->ffLock);
        info("FFI: Exiting fast-forward");
        ExitFastForward();
        futex_unlock(&zinfo->ffLock);
        FFITrackNFFInterval();

        SimThreadStart(tid);
    }
}

// One-off, called after we go from NFF to FF
VOID FFIEntryBasicBlock(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    ffiInstrsDone += *ffiFFStartInstrs - *ffiPrevFFStartInstrs; //add all instructions executed in the NFF phase
    FFIAdvance();
    assert(ffiNFF);
    ffiNFF = false;
    fPtrs[tid] = GetFFPtrs();
    FFIBasicBlock(tid, bblAddr, bblInfo);
}

// Non-analysis pointer vars
static const InstrFuncPtrs joinPtrs = {JoinAndLoadSingle, JoinAndStoreSingle, JoinAndBasicBlock, JoinAndRecordBranch, JoinAndPredLoadSingle, JoinAndPredStoreSingle, FPTR_JOIN};
static const InstrFuncPtrs nopPtrs = {NOPLoadStoreSingle, NOPLoadStoreSingle, NOPBasicBlock, NOPRecordBranch, NOPPredLoadStoreSingle, NOPPredLoadStoreSingle, FPTR_NOP};
static const InstrFuncPtrs retryPtrs = {NOPLoadStoreSingle, NOPLoadStoreSingle, NOPBasicBlock, NOPRecordBranch, NOPPredLoadStoreSingle, NOPPredLoadStoreSingle, FPTR_RETRY};
static const InstrFuncPtrs ffPtrs = {NOPLoadStoreSingle, NOPLoadStoreSingle, FFBasicBlock, NOPRecordBranch, NOPPredLoadStoreSingle, NOPPredLoadStoreSingle, FPTR_NOP};

static const InstrFuncPtrs ffiPtrs = {NOPLoadStoreSingle, NOPLoadStoreSingle, FFIBasicBlock, NOPRecordBranch, NOPPredLoadStoreSingle, NOPPredLoadStoreSingle, FPTR_NOP};
static const InstrFuncPtrs ffiEntryPtrs = {NOPLoadStoreSingle, NOPLoadStoreSingle, FFIEntryBasicBlock, NOPRecordBranch, NOPPredLoadStoreSingle, NOPPredLoadStoreSingle, FPTR_NOP};

static const InstrFuncPtrs& GetFFPtrs() {
    return ffiEnabled? (ffiNFF? ffiEntryPtrs : ffiPtrs) : ffPtrs;
}

//Fast-forwarding
void EnterFastForward() {
    assert(!procTreeNode->isInFastForward());
    procTreeNode->enterFastForward();
    __sync_synchronize(); //Make change globally visible

    //Re-instrument; VM/client lock are not needed
    if (zinfo->ffReinstrument) {
        PIN_RemoveInstrumentation();
    }
    //Transition to FF; we have the ff lock, this should be safe with end of phase code. This avoids profiling the end of a simulation as bound time
    //NOTE: Does not work well with multiprocess runs
    zinfo->profSimTime->transition(PROF_FF);
}


void ExitFastForward() {
    assert(procTreeNode->isInFastForward());

    VirtCaptureClocks(true /*exiting ffwd*/);

    procTreeNode->exitFastForward();
    __sync_synchronize(); //make change globally visible

    //Re-instrument; VM/client lock are not needed
    if (zinfo->ffReinstrument) {
        PIN_RemoveInstrumentation();
    }
}



//Termination
volatile uint32_t perProcessEndFlag;

VOID SimEnd();

VOID CheckForTermination() {
    assert(zinfo->terminationConditionMet == false);
    if (zinfo->maxPhases && zinfo->numPhases >= zinfo->maxPhases) {
        zinfo->terminationConditionMet = true;
        info("Max phases reached (%ld)", zinfo->numPhases);
        return;
    }

    if (zinfo->maxMinInstrs) {
        uint64_t minInstrs = zinfo->cores[0]->getInstrs();
        for (uint32_t i = 1; i < zinfo->numCores; i++) {
            uint64_t coreInstrs = zinfo->cores[i]->getInstrs();
            if (coreInstrs < minInstrs && coreInstrs > 0) {
                minInstrs = coreInstrs;
            }
        }

        if (minInstrs >= zinfo->maxMinInstrs) {
            zinfo->terminationConditionMet = true;
            info("Max min instructions reached (%ld)", minInstrs);
            return;
        }
    }

    if (zinfo->maxTotalInstrs) {
        uint64_t totalInstrs = 0;
        for (uint32_t i = 0; i < zinfo->numCores; i++) {
            totalInstrs += zinfo->cores[i]->getInstrs();
        }

        if (totalInstrs >= zinfo->maxTotalInstrs) {
            zinfo->terminationConditionMet = true;
            info("Max total (aggregate) instructions reached (%ld)", totalInstrs);
            return;
        }
    }

    if (zinfo->maxSimTimeNs) {
        uint64_t simNs = zinfo->profSimTime->count(PROF_BOUND) + zinfo->profSimTime->count(PROF_WEAVE);
        if (simNs >= zinfo->maxSimTimeNs) {
            zinfo->terminationConditionMet = true;
            info("Max simulation time reached (%ld ns)", simNs);
            return;
        }
    }

    if (zinfo->externalTermPending) {
        zinfo->terminationConditionMet = true;
        info("Terminating due to external notification");
        return;
    }
}

/* This is called by the scheduler at the end of a phase. At that point, zinfo->numPhases
 * has not incremented, so it denotes the END of the current phase
 */
VOID EndOfPhaseActions() {
    zinfo->profSimTime->transition(PROF_WEAVE);
    if (zinfo->globalPauseFlag) {
        info("Simulation entering global pause");
        zinfo->profSimTime->transition(PROF_FF);
        while (zinfo->globalPauseFlag) usleep(20*1000);
        zinfo->profSimTime->transition(PROF_WEAVE);
        info("Global pause DONE");
    }

    // Done before tick() to avoid deadlock in most cases when entering synced ffwd (can we still deadlock with sleeping threads?)
    if (unlikely(zinfo->globalSyncedFFProcs)) {
        info("Simulation paused due to synced fast-forwarding");
        zinfo->profSimTime->transition(PROF_FF);
        while (zinfo->globalSyncedFFProcs) usleep(20*1000);
        zinfo->profSimTime->transition(PROF_WEAVE);
        info("Synced fast-forwarding done, resuming simulation");
    }

    CheckForTermination();
    zinfo->contentionSim->simulatePhase(zinfo->globPhaseCycles + zinfo->phaseLength);
    zinfo->eventQueue->tick();
    zinfo->profSimTime->transition(PROF_BOUND);
}


uint32_t TakeBarrier(uint32_t tid, uint32_t cid) {
    uint32_t newCid = zinfo->sched->sync(procIdx, tid, cid);
    clearCid(tid); //this is after the sync for a hack needed to make EndOfPhase reliable
    setCid(tid, newCid);

    if (procTreeNode->isInFastForward()) {
        info("Thread %d entering fast-forward", tid);
        clearCid(tid);
        zinfo->sched->leave(procIdx, tid, newCid);
        newCid = INVALID_CID;
        SimThreadFini(tid);
        fPtrs[tid] = GetFFPtrs();
    } else if (zinfo->terminationConditionMet) {
        info("Termination condition met, exiting");
        zinfo->sched->leave(procIdx, tid, newCid);
        SimEnd(); //need to call this on a per-process basis...
    } else {
        // Set fPtrs to those of the new core after possible context switch
        fPtrs[tid] = cores[tid]->GetFuncPtrs();
    }

    return newCid;
}

/* ===================================================================== */

#if 0
static void PrintIp(THREADID tid, ADDRINT ip) {
    if (zinfo->globPhaseCycles > 1000000000L /*&& zinfo->globPhaseCycles < 1000030000L*/) {
        info("[%d] %ld 0x%lx", tid, zinfo->globPhaseCycles, ip);
    }
}
#endif

VOID Instruction(INS ins) {
    //Uncomment to print an instruction trace
    //INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)PrintIp, IARG_THREAD_ID, IARG_REG_VALUE, REG_INST_PTR, IARG_END);

    if (!procTreeNode->isInFastForward() || !zinfo->ffReinstrument) {
        AFUNPTR LoadFuncPtr = (AFUNPTR) IndirectLoadSingle;
        AFUNPTR StoreFuncPtr = (AFUNPTR) IndirectStoreSingle;

        AFUNPTR PredLoadFuncPtr = (AFUNPTR) IndirectPredLoadSingle;
        AFUNPTR PredStoreFuncPtr = (AFUNPTR) IndirectPredStoreSingle;

        if (INS_IsMemoryRead(ins)) {
            if (!INS_IsPredicated(ins)) {
                INS_InsertCall(ins, IPOINT_BEFORE, LoadFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYREAD_EA, IARG_END);
            } else {
                INS_InsertCall(ins, IPOINT_BEFORE, PredLoadFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYREAD_EA, IARG_EXECUTING, IARG_END);
            }
        }

        if (INS_HasMemoryRead2(ins)) {
            if (!INS_IsPredicated(ins)) {
                INS_InsertCall(ins, IPOINT_BEFORE, LoadFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYREAD2_EA, IARG_END);
            } else {
                INS_InsertCall(ins, IPOINT_BEFORE, PredLoadFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYREAD2_EA, IARG_EXECUTING, IARG_END);
            }
        }

        if (INS_IsMemoryWrite(ins)) {
            if (!INS_IsPredicated(ins)) {
                INS_InsertCall(ins, IPOINT_BEFORE,  StoreFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYWRITE_EA, IARG_END);
            } else {
                INS_InsertCall(ins, IPOINT_BEFORE,  PredStoreFuncPtr, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_MEMORYWRITE_EA, IARG_EXECUTING, IARG_END);
            }
        }

        // Instrument only conditional branches
        if (INS_Category(ins) == XED_CATEGORY_COND_BR) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) IndirectRecordBranch, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID,
                    IARG_INST_PTR, IARG_BRANCH_TAKEN, IARG_BRANCH_TARGET_ADDR, IARG_FALLTHROUGH_ADDR, IARG_END);
        }
    }

    //Intercept and process magic ops
    /* xchg %rcx, %rcx is our chosen magic op. It is effectively a NOP, but it
     * is never emitted by any x86 compiler, as they use other (recommended) nop
     * instructions or sequences.
     */
    if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_RCX && INS_OperandReg(ins, 1) == REG_RCX) {
        //info("Instrumenting magic op");
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) HandleMagicOp, IARG_THREAD_ID, IARG_REG_VALUE, REG_ECX, IARG_END);
    }

    if (INS_Opcode(ins) == XED_ICLASS_CPUID) {
       INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) FakeCPUIDPre, IARG_THREAD_ID, IARG_REG_VALUE, REG_EAX, IARG_REG_VALUE, REG_ECX, IARG_END);
       INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) FakeCPUIDPost, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_EAX,
               IARG_REG_REFERENCE, REG_EBX, IARG_REG_REFERENCE, REG_ECX, IARG_REG_REFERENCE, REG_EDX, IARG_END);
    }

    if (INS_IsRDTSC(ins)) {
        //No pre; note that this also instruments RDTSCP
        INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) FakeRDTSCPost, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_EAX, IARG_REG_REFERENCE, REG_EDX, IARG_END);
    }

    //Must run for every instruction
    VdsoInstrument(ins);
}


VOID Trace(TRACE trace, VOID *v) {
    if (!procTreeNode->isInFastForward() || !zinfo->ffReinstrument) {
        // Visit every basic block in the trace
        for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
            BblInfo* bblInfo = Decoder::decodeBbl(bbl, zinfo->oooDecode);
            BBL_InsertCall(bbl, IPOINT_BEFORE /*could do IPOINT_ANYWHERE if we redid load and store simulation in OOO*/, (AFUNPTR)IndirectBasicBlock, IARG_FAST_ANALYSIS_CALL,
                 IARG_THREAD_ID, IARG_ADDRINT, BBL_Address(bbl), IARG_PTR, bblInfo, IARG_END);
        }
    }

    //Instruction instrumentation now here to ensure proper ordering
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            Instruction(ins);
        }
    }
}

/***** vDSO instrumentation and patching code *****/

// Helper function to find section address
// adapted from http://outflux.net/aslr/aslr.c
struct Section {
    uintptr_t start;
    uintptr_t end;
};

static Section FindSection(const char* sec) {
    /* locate the vdso from the maps file */
    char buf[129];
    buf[128] = '\0';
    FILE * fp = fopen("/proc/self/maps", "r");
    Section res = {0x0, 0x0};
    if (fp) {
        while (fgets(buf, 128, fp)) {
            if (strstr(buf, sec)) {
                char * dash = strchr(buf, '-');
                if (dash) {
                    *dash='\0';
                    res.start = strtoul(buf, nullptr, 16);
                    res.end   = strtoul(dash+1, nullptr, 16);
                }
            }
        }
    }

    //Uncomment to print maps
    //fseek(fp, 0, SEEK_SET);
    //while (fgets(buf, 128, fp)) info("%s", buf);
    return res;
}

// Initialization code and global per-process data

enum VdsoFunc {VF_CLOCK_GETTIME, VF_GETTIMEOFDAY, VF_TIME, VF_GETCPU};

static std::unordered_map<ADDRINT, VdsoFunc> vdsoEntryMap;
static uintptr_t vdsoStart;
static uintptr_t vdsoEnd;

//Used to warn
static uintptr_t vsyscallStart;
static uintptr_t vsyscallEnd;
static bool vsyscallWarned = false;

// Helper function from parse_vsdo.cpp
extern void vdso_init_from_sysinfo_ehdr(uintptr_t base);
extern void *vdso_sym(const char *version, const char *name);

void VdsoInsertFunc(const char* fName, VdsoFunc func) {
    ADDRINT vdsoFuncAddr = (ADDRINT) vdso_sym("LINUX_2.6", fName);
    if (vdsoFuncAddr == 0) {
        warn("Did not find %s in vDSO", fName);
    } else {
        vdsoEntryMap[vdsoFuncAddr] = func;
    }
}

void VdsoInit() {
    Section vdso = FindSection("vdso");
    vdsoStart = vdso.start;
    vdsoEnd = vdso.end;

    if (!vdsoEnd) {
        // Non-fatal, but should not happen --- even static binaries get vDSO AFAIK
        warn("vDSO not found");
        return;
    }

    vdso_init_from_sysinfo_ehdr(vdsoStart);

    VdsoInsertFunc("clock_gettime", VF_CLOCK_GETTIME);
    VdsoInsertFunc("__vdso_clock_gettime", VF_CLOCK_GETTIME);

    VdsoInsertFunc("gettimeofday", VF_GETTIMEOFDAY);
    VdsoInsertFunc("__vdso_gettimeofday", VF_GETTIMEOFDAY);

    VdsoInsertFunc("time", VF_TIME);
    VdsoInsertFunc("__vdso_time", VF_TIME);

    VdsoInsertFunc("getcpu", VF_GETCPU);
    VdsoInsertFunc("__vdso_getcpu", VF_GETCPU);

    info("vDSO info initialized");

    Section vsyscall = FindSection("vsyscall");
    vsyscallStart = vsyscall.start;
    vsyscallEnd = vsyscall.end;
    // Could happen in the future when vsyscall is phased out, kill the warn then
    if (!vsyscallEnd) warn("vsyscall page not found");
}

// Register hooks to intercept and virtualize time-related vsyscalls and vdso syscalls, as they do not show up as syscalls!
// NOTE: getcpu is also a VDSO syscall, but is not patched for now

// Per-thread VDSO data
struct VdsoPatchData {
    // Input arguments --- must save them because they are not caller-saved
    // Careful: REG is 32 bits; PIN_REGISTER, which is the actual type of the
    // pointer, is 64 bits but opaque. We just use ADDRINT, it works
    ADDRINT arg0, arg1;
    VdsoFunc func;
    uint32_t level;  // if 0, invalid. Used for VDSO-internal calls
};
VdsoPatchData vdsoPatchData[MAX_THREADS];

// Analysis functions

VOID VdsoEntryPoint(THREADID tid, uint32_t func, ADDRINT arg0, ADDRINT arg1) {
    if (vdsoPatchData[tid].level) {
        // common, in Ubuntu 11.10 several vdso functions jump back to the callpoint
        // info("vDSO function (%d) called from vdso (%d), level %d, skipping", func, vdsoPatchData[tid].func, vdsoPatchData[tid].level);
    } else {
        vdsoPatchData[tid].arg0 = arg0;
        vdsoPatchData[tid].arg1 = arg1;
        vdsoPatchData[tid].func = (VdsoFunc)func;
        vdsoPatchData[tid].level++;
    }
}

VOID VdsoCallPoint(THREADID tid) {
    assert(vdsoPatchData[tid].level);
    vdsoPatchData[tid].level++;
    // info("vDSO internal callpoint, now level %d", vdsoPatchData[tid].level); //common
}

VOID VdsoRetPoint(THREADID tid, REG* raxPtr) {
    if (vdsoPatchData[tid].level == 0) {
        warn("vDSO return without matching call --- did we instrument all the functions?");
        return;
    }
    vdsoPatchData[tid].level--;
    if (vdsoPatchData[tid].level) {
        // info("vDSO return post level %d, skipping ret handling", vdsoPatchData[tid].level); //common
        return;
    }
    if (fPtrs[tid].type != FPTR_NOP || vdsoPatchData[tid].func == VF_GETCPU) {
        // info("vDSO patching for func %d", vdsoPatchData[tid].func);  // common
        ADDRINT arg0 = vdsoPatchData[tid].arg0;
        ADDRINT arg1 = vdsoPatchData[tid].arg1;
        switch (vdsoPatchData[tid].func) {
            case VF_CLOCK_GETTIME:
                VirtClockGettime(tid, arg0, arg1);
                break;
            case VF_GETTIMEOFDAY:
                VirtGettimeofday(tid, arg0);
                break;
            case VF_TIME:
                VirtTime(tid, raxPtr, arg0);
                break;
            case VF_GETCPU:
                {
                uint32_t cpu = cpuenumCpu(procIdx, getCid(tid));
                VirtGetcpu(tid, cpu, arg0, arg1);
                }
                break;
            default:
                panic("vDSO garbled func %d", vdsoPatchData[tid].func);
        }
    }
}

// Instrumentation function, called for EVERY instruction
VOID VdsoInstrument(INS ins) {
    ADDRINT insAddr = INS_Address(ins);
    if (unlikely(insAddr >= vdsoStart && insAddr < vdsoEnd)) {
        if (vdsoEntryMap.find(insAddr) != vdsoEntryMap.end()) {
            VdsoFunc func = vdsoEntryMap[insAddr];
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) VdsoEntryPoint, IARG_THREAD_ID, IARG_UINT32, (uint32_t)func, IARG_REG_VALUE, REG_RDI, IARG_REG_VALUE, REG_RSI, IARG_END);
        } else if (INS_IsCall(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) VdsoCallPoint, IARG_THREAD_ID, IARG_END);
        } else if (INS_IsRet(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) VdsoRetPoint, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_RAX /* return val */, IARG_END);
        }
    }

    //Warn on the first vsyscall code translation
    if (unlikely(insAddr >= vsyscallStart && insAddr < vsyscallEnd && !vsyscallWarned)) {
        warn("Instrumenting vsyscall page code --- this process executes vsyscalls, which zsim does not virtualize!");
        vsyscallWarned = true;
    }
}

/* ===================================================================== */


bool activeThreads[MAX_THREADS];  // set in ThreadStart, reset in ThreadFini, we need this for exec() (see FollowChild)
bool inSyscall[MAX_THREADS];  // set in SyscallEnter, reset in SyscallExit, regardless of state. We MAY need this for ContextChange

uint32_t CountActiveThreads() {
    // Finish all threads in this process w.r.t. the global scheduler
    uint32_t activeCount = 0;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        if (activeThreads[i]) activeCount++;
    }
    return activeCount;
}

void SimThreadStart(THREADID tid) {
    info("Thread %d starting", tid);
    if (tid > MAX_THREADS) panic("tid > MAX_THREADS");
    zinfo->sched->start(procIdx, tid, procTreeNode->getMask());
    activeThreads[tid] = true;

    //Pinning
#if 0
    if (true) {
        uint32_t nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(tid % nprocs, &cpuset);
        //HMM, can we do this? I doubt it
        //int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        //Since we're running multiprocess, this suffices for now:
        int result = sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset);
        assert(result == 0);
    }
#endif

    //Initialize this thread's process-local data
    fPtrs[tid] = joinPtrs; //delayed, MT-safe barrier join
    clearCid(tid); //just in case, set an invalid cid
}

VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v) {
    /* This should only fire for the first thread; I know this is a callback,
     * everything is serialized etc; that's the point, we block everything.
     * It's here and not in main() because that way the auxiliary threads can
     * start.
     */
    if (procTreeNode->isInPause()) {
        futex_lock(&zinfo->pauseLocks[procIdx]);  // initialize
        info("Pausing until notified");
        futex_lock(&zinfo->pauseLocks[procIdx]);  // block
        procTreeNode->exitPause();
        info("Unpaused");
    }

    if (procTreeNode->isInFastForward()) {
        info("FF thread %d starting", tid);
        fPtrs[tid] = GetFFPtrs();
    } else if (zinfo->registerThreads) {
        info("Shadow thread %d starting", tid);
        fPtrs[tid] = nopPtrs;
    } else {
        //Start normal thread
        SimThreadStart(tid);
    }
}

VOID SimThreadFini(THREADID tid) {
    // zinfo->sched->leave(); //exit syscall (SyscallEnter) already leaves
    zinfo->sched->finish(procIdx, tid);
    activeThreads[tid] = false;
    cids[tid] = UNINITIALIZED_CID; //clear this cid, it might get reused
}

VOID ThreadFini(THREADID tid, const CONTEXT *ctxt, INT32 flags, VOID *v) {
    //NOTE: Thread has no valid cid here!
    if (fPtrs[tid].type == FPTR_NOP) {
        info("Shadow/NOP thread %d finished", tid);
        return;
    } else {
        SimThreadFini(tid);
        info("Thread %d finished", tid);
    }
}

//Need to remove ourselves from running threads in case the syscall is blocking
VOID SyscallEnter(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v) {
    bool isNopThread = fPtrs[tid].type == FPTR_NOP;
    bool isRetryThread = fPtrs[tid].type == FPTR_RETRY;

    if (!isRetryThread) {
        VirtSyscallEnter(tid, ctxt, std, procTreeNode->getPatchRoot(), isNopThread);
    }

    assert(!inSyscall[tid]); inSyscall[tid] = true;

    if (isNopThread || isRetryThread) return;

    /* NOTE: It is possible that we take 2 syscalls back to back with any
     * intervening instrumentation, so we need to check. In that case, this is
     * treated as a single syscall scheduling-wise (no second leave without
     * join).
     */
    if (fPtrs[tid].type != FPTR_JOIN && !zinfo->blockingSyscalls) {
        uint32_t cid = getCid(tid);
        // set an invalid cid, ours is property of the scheduler now!
        clearCid(tid);

        zinfo->sched->syscallLeave(procIdx, tid, cid, PIN_GetContextReg(ctxt, REG_INST_PTR),
                PIN_GetSyscallNumber(ctxt, std), PIN_GetSyscallArgument(ctxt, std, 0),
                PIN_GetSyscallArgument(ctxt, std, 1));
        //zinfo->sched->leave(procIdx, tid, cid);
        fPtrs[tid] = joinPtrs;  // will join at the next instr point
        //info("SyscallEnter %d", tid);
    }
}

VOID SyscallExit(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v) {
    assert(inSyscall[tid]); inSyscall[tid] = false;

    PostPatchAction ppa = VirtSyscallExit(tid, ctxt, std);
    if (ppa == PPA_USE_JOIN_PTRS) {
        if (!zinfo->blockingSyscalls) {
            fPtrs[tid] = joinPtrs;
        } else {
            fPtrs[tid] = cores[tid]->GetFuncPtrs(); //go back to normal pointers, directly
        }
    } else if (ppa == PPA_USE_RETRY_PTRS) {
        fPtrs[tid] = retryPtrs;
    } else {
        assert(ppa == PPA_NOTHING);
    }

    //Avoid joining at all if we are in FF!
    if (fPtrs[tid].type == FPTR_JOIN && procTreeNode->isInFastForward()) {
        assert(activeThreads[tid]);
        info("Thread %d entering fast-forward (from syscall exit)", tid);
        //We are not in the scheduler, and have no cid assigned. So, no need to leave()
        SimThreadFini(tid);
        fPtrs[tid] = GetFFPtrs();
    }


    if (zinfo->terminationConditionMet) {
        info("Caught termination condition on syscall exit, exiting");
        SimEnd();
    }
}

/* NOTE: We may screw up programs with frequent signals / SIG on syscall. If
 * you see this warning and simulations misbehave, it's time to do some testing
 * to figure out how to make syscall post-patching work in this case.
 */
VOID ContextChange(THREADID tid, CONTEXT_CHANGE_REASON reason, const CONTEXT* from, CONTEXT* to, INT32 info, VOID* v) {
    const char* reasonStr = "?";
    switch (reason) {
        case CONTEXT_CHANGE_REASON_FATALSIGNAL:
            reasonStr = "FATAL_SIGNAL";
            break;
        case CONTEXT_CHANGE_REASON_SIGNAL:
            reasonStr = "SIGNAL";
            break;
        case CONTEXT_CHANGE_REASON_SIGRETURN:
            reasonStr = "SIGRETURN";
            break;
        case CONTEXT_CHANGE_REASON_APC:
            reasonStr = "APC";
            break;
        case CONTEXT_CHANGE_REASON_EXCEPTION:
            reasonStr = "EXCEPTION";
            break;
        case CONTEXT_CHANGE_REASON_CALLBACK:
            reasonStr = "CALLBACK";
            break;
    }

    warn("[%d] ContextChange, reason %s, inSyscall %d", tid, reasonStr, inSyscall[tid]);
    if (inSyscall[tid]) {
        SyscallExit(tid, to, SYSCALL_STANDARD_IA32E_LINUX, nullptr);
    }

    if (reason == CONTEXT_CHANGE_REASON_FATALSIGNAL) {
        info("[%d] Fatal signal caught, finishing", tid);
        zinfo->sched->queueProcessCleanup(procIdx, getpid()); //the scheduler watchdog will remove all our state when we are really dead
        SimEnd();
    }

    //If this is an issue, we might need to call syscallexit on occasion. I very much doubt it
    //SyscallExit(tid, to, SYSCALL_STANDARD_IA32E_LINUX, nullptr); //NOTE: For now it is safe to do spurious syscall exits, but careful...
}

/* Fork and exec instrumentation */

//For funky macro stuff
#define QUOTED_(x) #x
#define QUOTED(x) QUOTED_(x)

// Pre-exec
BOOL FollowChild(CHILD_PROCESS childProcess, VOID * userData) {
    //Finish all threads in this process w.r.t. the global scheduler

    uint32_t activeCount = CountActiveThreads();
    if (activeCount > 1) warn("exec() of a multithreaded process! (%d live threads)", activeCount);

    // You can always run process0 = { command = "ls"; startPaused = True; startFastForwarded = True; }; to avoid this
    if (procIdx == 0) panic("process0 cannot exec(), it spawns globally needed internal threads (scheduler and contention); run a dummy process0 instead!");

    //Set up Pin command
    //NOTE: perProcessDir may be active, we don't care much... run in the same dir as parent process
    //NOTE: we recycle our own procIdx on an exec, but fork() changed it so we need to update Pin's command line
    g_vector<g_string> args = zinfo->pinCmd->getPinCmdArgs(procIdx);
    uint32_t numArgs = args.size();
    const char* pinArgs[numArgs];
    for (uint32_t i = 0; i < numArgs; i++) pinArgs[i] = args[i].c_str();
    CHILD_PROCESS_SetPinCommandLine(childProcess, numArgs, pinArgs);

    //As a convenience, print the command we are going to execute
    const char* const* cArgv;
    int cArgc;
    CHILD_PROCESS_GetCommandLine(childProcess, &cArgc, &cArgv);

    std::string childCmd = cArgv[0];
    for (int i = 1; i < cArgc; i++) {
        childCmd += " ";
        childCmd += cArgv[i];
    }

    info("Following exec(): %s", childCmd.c_str());

    return true; //always follow
}

static ProcessTreeNode* forkedChildNode = nullptr;

VOID BeforeFork(THREADID tid, const CONTEXT* ctxt, VOID * arg) {
    forkedChildNode = procTreeNode->getNextChild();
    info("Thread %d forking, child procIdx=%d", tid, forkedChildNode->getProcIdx());
}

VOID AfterForkInParent(THREADID tid, const CONTEXT* ctxt, VOID * arg) {
    forkedChildNode = nullptr;
}

VOID AfterForkInChild(THREADID tid, const CONTEXT* ctxt, VOID * arg) {
    assert(forkedChildNode);
    procTreeNode = forkedChildNode;
    procIdx = procTreeNode->getProcIdx();
    bool wasNotStarted = procTreeNode->notifyStart();
    assert(wasNotStarted); //it's a fork, should be new
    procMask = ((uint64_t)procIdx) << (64-lineBits);

    char header[64];
    snprintf(header, sizeof(header), "[S %dF] ", procIdx); //append an F to distinguish forked from fork/exec'd
    std::stringstream logfile_ss;
    logfile_ss << zinfo->outputDir << "/zsim.log." << procIdx;
    InitLog(header, KnobLogToFile.Value()? logfile_ss.str().c_str() : nullptr);

    info("Forked child (tid %d/%d), PID %d, parent PID %d", tid, PIN_ThreadId(), PIN_GetPid(), getppid());

    //Initialize process-local per-thread state, even if ThreadStart does so later
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        fPtrs[i] = joinPtrs;
        cids[i] = UNINITIALIZED_CID;
        activeThreads[i] = false;
        inSyscall[i] = false;
        cores[i] = nullptr;
    }

    //We need to launch another copy of the FF control thread
    PIN_SpawnInternalThread(FFThread, nullptr, 64*1024, nullptr);

    ThreadStart(tid, nullptr, 0, nullptr);
}

/** Finalization **/

VOID Fini(int code, VOID * v) {
    info("Finished, code %d", code);
    //NOTE: In fini, it appears that info() and writes to stdout in general won't work; warn() and stderr still work fine.
    SimEnd();
}

VOID SimEnd() {
    if (__sync_bool_compare_and_swap(&perProcessEndFlag, 0, 1) == false) { //failed, note DEPENDS ON STRONG CAS
        while (true) { //sleep until thread that won exits for us
            struct timespec tm;
            tm.tv_sec = 1;
            tm.tv_nsec = 0;
            nanosleep(&tm, nullptr);
        }
    }

    //at this point, we're in charge of exiting our whole process, but we still need to race for the stats

    //per-process
#ifdef BBL_PROFILING
    Decoder::dumpBblProfile();
#endif

    //global
    bool lastToFinish = procTreeNode->notifyEnd();
    (void) lastToFinish; //make gcc happy; not needed anymore, since proc 0 dumps stats

    if (procIdx == 0) {
        //Done to preserve the scheduler and contention simulation internal threads
        if (zinfo->globalActiveProcs) {
            info("Delaying termination until all other processes finish");
            while (zinfo->globalActiveProcs) usleep(100*1000);
            info("All other processes done, terminating");
        }

        info("Dumping termination stats");
        zinfo->trigger = 20000;
        for (StatsBackend* backend : *(zinfo->statsBackends)) backend->dump(false /*unbuffered, write out*/);
        for (AccessTraceWriter* t : *(zinfo->traceWriters)) t->dump(false);  // flushes trace writer

        if (zinfo->sched) zinfo->sched->notifyTermination();
    }

    //Uncomment when debugging termination races, which can be rare because they are triggered by threads of a dying process
    //sleep(5);

    exit(0);
}


// Magic ops interface
/* TODO: In the future, we might want to return values to the program.
 * This is definitely doable, but there is no use for it now.
 */
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)

VOID HandleMagicOp(THREADID tid, ADDRINT op) {
    switch (op) {
        case ZSIM_MAGIC_OP_ROI_BEGIN:
            if (!zinfo->ignoreHooks) {
                //TODO: Test whether this is thread-safe
                futex_lock(&zinfo->ffLock);
                if (procTreeNode->isInFastForward()) {
                    info("ROI_BEGIN, exiting fast-forward");
                    ExitFastForward();
                } else {
                    warn("Ignoring ROI_BEGIN magic op, not in fast-forward");
                }
                futex_unlock(&zinfo->ffLock);
            }
            return;
        case ZSIM_MAGIC_OP_ROI_END:
            if (!zinfo->ignoreHooks) {
                //TODO: Test whether this is thread-safe
                futex_lock(&zinfo->ffLock);
                if (procTreeNode->getSyncedFastForward()) {
                    warn("Ignoring ROI_END magic op on synced FF to avoid deadlock");
                } else if (!procTreeNode->isInFastForward()) {
                    info("ROI_END, entering fast-forward");
                    EnterFastForward();
                    //If we don't do this, we'll enter FF on the next phase. Which would be OK, except with synced FF
                    //we stay in the barrier forever. And deadlock. And the deadlock code does nothing, since we're in FF
                    //So, force immediate entry if we're sync-ffwding
                    if (procTreeNode->getSyncedFastForward()) {
                        info("Thread %d entering fast-forward (immediate)", tid);
                        uint32_t cid = getCid(tid);
                        assert(cid != INVALID_CID);
                        clearCid(tid);
                        zinfo->sched->leave(procIdx, tid, cid);
                        SimThreadFini(tid);
                        fPtrs[tid] = GetFFPtrs();
                    }
                } else {
                    warn("Ignoring ROI_END magic op, already in fast-forward");
                }
                futex_unlock(&zinfo->ffLock);
            }
            return;
        case ZSIM_MAGIC_OP_REGISTER_THREAD:
            if (!zinfo->registerThreads) {
                info("Thread %d: Treating REGISTER_THREAD magic op as NOP", tid);
            } else {
                if (fPtrs[tid].type == FPTR_NOP) {
                    SimThreadStart(tid);
                } else {
                    warn("Thread %d: Treating REGISTER_THREAD magic op as NOP, thread already registered", tid);
                }
            }
            return;
        case ZSIM_MAGIC_OP_HEARTBEAT:
            procTreeNode->heartbeat(); //heartbeats are per process for now
            return;

        // HACK: Ubik magic ops
        case 1029:
        case 1030:
        case 1031:
        case 1032:
        case 1033:
            return;
        default:
            panic("Thread %d issued unknown magic op %ld!", tid, op);
    }
}

//CPUIID faking
static uint32_t cpuidEax[MAX_THREADS];
static uint32_t cpuidEcx[MAX_THREADS];

VOID FakeCPUIDPre(THREADID tid, REG eax, REG ecx) {
    //info("%d precpuid", tid);
    cpuidEax[tid] = eax;
    cpuidEcx[tid] = ecx;
}

VOID FakeCPUIDPost(THREADID tid, ADDRINT* eax, ADDRINT* ebx, ADDRINT* ecx, ADDRINT* edx) {
    uint32_t eaxIn = cpuidEax[tid];
    uint32_t ecxIn = cpuidEcx[tid];

    // Point to record at same (eax,ecx) or immediately before
    CpuIdRecord val = {eaxIn, ecxIn, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1};
    CpuIdRecord* pos = std::lower_bound(cpuid_core2, cpuid_core2+(sizeof(cpuid_core2)/sizeof(CpuIdRecord)), val);
    if (pos->eaxIn > eaxIn) {
        assert(pos > cpuid_core2);
        pos--;
    }
    assert(pos->eaxIn <= eaxIn);
    assert(pos->ecxIn <= ecxIn);

    //info("%x %x : %x %x / %x %x %x %x", eaxIn, ecxIn, pos->eaxIn, pos->ecxIn, pos->eax, pos->ebx, pos->ecx, pos->edx);

    uint32_t eaxOut = pos->eax;
    uint32_t ebxOut = pos->ebx;

    // patch eax to give the number of cores
    if (eaxIn == 4) {
        uint32_t ncpus = cpuenumNumCpus(procIdx);
        uint32_t eax3126 = ncpus - 1;
        // Overflowing 6 bits?
        if (zinfo->numCores > 64) eax3126 = 63; //looked into swarm2.csail (4P Westmere-EX, 80 HTs), it sets this to 63
        eaxOut = (eaxOut & ((1<<26)-1)) | (eax3126<<26);
    }

    // HT siblings and APIC (core) ID (apparently used; seems Intel-specific)
    if (eaxIn == 0x1) {
        uint32_t cid = getCid(tid);
        uint32_t cpu = cpuenumCpu(procIdx, cid);
        uint32_t ncpus = cpuenumNumCpus(procIdx);
        uint32_t siblings = MIN(ncpus, (uint32_t)255);
        uint32_t apicId = (cpu < ncpus)? MIN(cpu, (uint32_t)255) : 0 /*not scheduled, ffwd?*/;
        ebxOut = (ebxOut & 0xffff) | (siblings << 16) | (apicId << 24);
    }

    //info("[%d] postcpuid, inEax 0x%x, pre 0x%lx 0x%lx 0x%lx 0x%lx", tid, eaxIn, *eax, *ebx, *ecx, *edx);
    //Preserve high bits
    *reinterpret_cast<uint32_t*>(eax) = eaxOut;
    *reinterpret_cast<uint32_t*>(ebx) = ebxOut;
    *reinterpret_cast<uint32_t*>(ecx) = pos->ecx;
    *reinterpret_cast<uint32_t*>(edx) = pos->edx;
    //info("[%d] postcpuid, inEax 0x%x, post 0x%lx 0x%lx 0x%lx 0x%lx", tid, eaxIn, *eax, *ebx, *ecx, *edx);
}


//RDTSC faking
VOID FakeRDTSCPost(THREADID tid, REG* eax, REG* edx) {
    if (fPtrs[tid].type == FPTR_NOP) return; //avoid virtualizing NOP threads.

    uint32_t cid = getCid(tid);
    uint64_t curCycle = VirtGetPhaseRDTSC();
    if (cid < zinfo->numCores) {
        curCycle += zinfo->cores[cid]->getPhaseCycles();
    }

    uint32_t lo = (uint32_t)curCycle;
    uint32_t hi = (uint32_t)(curCycle >> 32);

    assert((((uint64_t)hi) << 32) + lo == curCycle);

    //uint64_t origTSC = (((uint64_t)*edx) << 32) + (uint32_t)*eax;
    //info("[t%d/c%d] Virtualizing RDTSC, pre = %x %x (%ld), post = %x %x (%ld)", tid, cid, *edx, *eax, origTSC, hi, lo, curCycle);

    *eax = (REG)lo;
    *edx = (REG)hi;
}

/* Fast-forward control */

// Helper class, enabled the FFControl thread to sync with the phase end code
class SyncEvent: public Event {
    private:
        lock_t arrivalLock;
        lock_t leaveLock;

    public:
        SyncEvent() : Event(0 /*one-shot*/) {
            futex_init(&arrivalLock);
            futex_init(&leaveLock);

            futex_lock(&arrivalLock);
            futex_lock(&leaveLock);
        }

        // Blocks until callback()
        void wait() {
            futex_lock(&arrivalLock);
        }

        // Unblocks thread that called wait(), blocks until signal() called
        // Resilient against callback-wait races (wait does not block if it's
        // called afteer callback)
        void callback() {
            futex_unlock(&arrivalLock);
            futex_lock(&leaveLock);
        }

        // Unblocks thread waiting in callback()
        void signal() {
            futex_unlock(&leaveLock);
        }
};

VOID FFThread(VOID* arg) {
    futex_lock(&zinfo->ffToggleLocks[procIdx]); //initialize
    info("FF control Thread TID %ld", syscall(SYS_gettid));

    while (true) {
        //block ourselves until someone wakes us up with an unlock
        bool locked = futex_trylock_nospin_timeout(&zinfo->ffToggleLocks[procIdx], 5*BILLION /*5s timeout*/);

        if (!locked) { //timeout
            if (zinfo->terminationConditionMet) {
                info("Terminating FF control thread");
                SimEnd();
                panic("Should not be reached");
            }
            //info("FF control thread wakeup");
            continue;
        }

        futex_lock(&zinfo->ffLock);
        if (procTreeNode->isInFastForward()) {
            GetVmLock(); //like a callback. This disallows races on all syscall instrumentation, etc.
            info("Exiting fast forward");
            ExitFastForward();
            ReleaseVmLock();
        } else {
            SyncEvent* syncEv = new SyncEvent();
            zinfo->eventQueue->insert(syncEv); //will run on next phase
            info("Pending fast-forward entry, waiting for end of phase (%ld phases)", zinfo->numPhases);

            futex_unlock(&zinfo->ffLock);
            syncEv->wait();
            //At this point the thread thet triggered the end of phase is blocked inside of EndOfPhaseActions
            futex_lock(&zinfo->ffLock);
            if (!procTreeNode->isInFastForward()) {
                info("End of phase %ld, entering FF", zinfo->numPhases);
                EnterFastForward();
            } else {
                info("FF control thread called on end of phase, but someone else (program?) already entered ffwd");
            }
            syncEv->signal(); //unblock thread in EndOfPhaseActions
        }
        futex_unlock(&zinfo->ffLock);
    }
    panic("Should not be reached!");
}


/* Internal Exception Handler */
//When firing a debugger was an easy affair, this was not an issue. Now it's not so easy, so let's try to at least capture the backtrace and print it out

//Use unlocked output, who knows where this happens.
static EXCEPT_HANDLING_RESULT InternalExceptionHandler(THREADID tid, EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt, VOID *) {
    fprintf(stderr, "%s[%d] Internal exception detected:\n", logHeader, tid);
    fprintf(stderr, "%s[%d]  Code: %d\n", logHeader, tid, PIN_GetExceptionCode(pExceptInfo));
    fprintf(stderr, "%s[%d]  Address: 0x%lx\n", logHeader, tid, PIN_GetExceptionAddress(pExceptInfo));
    fprintf(stderr, "%s[%d]  Description: %s\n", logHeader, tid, PIN_ExceptionToString(pExceptInfo).c_str());

    ADDRINT faultyAccessAddr;
    if (PIN_GetFaultyAccessAddress(pExceptInfo, &faultyAccessAddr)) {
        const char* faultyAccessStr = "";
        FAULTY_ACCESS_TYPE fat = PIN_GetFaultyAccessType(pExceptInfo);
        if (fat == FAULTY_ACCESS_READ) faultyAccessStr = "READ ";
        else if (fat == FAULTY_ACCESS_WRITE) faultyAccessStr = "WRITE ";
        else if (fat == FAULTY_ACCESS_EXECUTE) faultyAccessStr = "EXECUTE ";

        fprintf(stderr, "%s[%d]  Caused by invalid %saccess to address 0x%lx\n", logHeader, tid, faultyAccessStr, faultyAccessAddr);
    }

    void* array[40];
    size_t size = backtrace(array, 40);
    char** strings = backtrace_symbols(array, size);
    fprintf(stderr, "%s[%d] Backtrace (%ld/%d max frames)\n", logHeader, tid, size, 40);
    for (uint32_t i = 0; i < size; i++) {
        //For libzsim.so addresses, call addr2line to get symbol info (can't use -rdynamic on libzsim.so because of Pin's linker script)
        //NOTE: May be system-dependent, may not handle malformed strings well. We're going to die anyway, so in for a penny, in for a pound...
        std::string s = strings[i];
        uint32_t lp = s.find_first_of("(");
        uint32_t cp = s.find_first_of(")");
        std::string fname = s.substr(0, lp);
        std::string faddr = s.substr(lp+1, cp-(lp+1));
        if (fname.find("libzsim.so") != std::string::npos) {
            std::string cmd = "addr2line -f -C -e " + fname + " " + faddr;
            FILE* f = popen(cmd.c_str(), "r");
            if (f) {
                char buf[1024];
                std::string func, loc;
                func = fgets(buf, 1024, f); //first line is function name
                loc = fgets(buf, 1024, f); //second is location
                //Remove line breaks
                func = func.substr(0, func.size()-1);
                loc = loc.substr(0, loc.size()-1);

                int status = pclose(f);
                if (status == 0) {
                    s = loc + " / " + func;
                }
            }
        }

        fprintf(stderr, "%s[%d]  %s\n", logHeader, tid, s.c_str());
    }
    fflush(stderr);

    return EHR_CONTINUE_SEARCH; //we never solve anything at all :P
}

/* ===================================================================== */

int main(int argc, char *argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) return Usage();

    //Register an internal exception handler (ASAP, to catch segfaults in init)
    PIN_AddInternalExceptionHandler(InternalExceptionHandler, nullptr);

    procIdx = KnobProcIdx.Value();
    char header[64];
    snprintf(header, sizeof(header), "[S %d] ", procIdx);
    std::stringstream logfile_ss;
    logfile_ss << KnobOutputDir.Value() << "/zsim.log." << procIdx;
    InitLog(header, KnobLogToFile.Value()? logfile_ss.str().c_str() : nullptr);

    //If parent dies, kill us
    //This avoids leaving strays running in any circumstances, but may be too heavy-handed with arbitrary process hierarchies.
    //If you ever need this disabled, sim.pinOptions = "-injection child" does the trick
    if (prctl(PR_SET_PDEATHSIG, 9 /*SIGKILL*/) != 0) {
        panic("prctl() failed");
    }

    info("Started instance");

    //Decrease priority to avoid starving system processes (e.g. gluster)
    //setpriority(PRIO_PROCESS, getpid(), 10);
    //info("setpriority, new prio %d", getpriority(PRIO_PROCESS, getpid()));

    gm_attach(KnobShmid.Value());

    bool masterProcess = false;
    if (procIdx == 0 && !gm_isready()) {  // process 0 can exec() without fork()ing first, so we must check gm_isready() to ensure we don't initialize twice
        masterProcess = true;
        SimInit(KnobConfigFile.Value().c_str(), KnobOutputDir.Value().c_str(), KnobShmid.Value());
    } else {
        while (!gm_isready()) usleep(1000);  // wait till proc idx 0 initializes everything
        zinfo = static_cast<GlobSimInfo*>(gm_get_glob_ptr());
    }

    //If assertion below fails, use this to print maps
#if 0
    futex_lock(&zinfo->ffLock); //whatever lock, just don't interleave
    std::ifstream infile("/proc/self/maps");
    std::string line;
    while (std::getline(infile, line)) info("  %s", line.c_str());
    futex_unlock(&zinfo->ffLock);
    usleep(100000);
#endif
    //LibzsimAddrs sanity check: Ensure that they match across processes
    struct LibInfo libzsimAddrs;
    getLibzsimAddrs(&libzsimAddrs);
    if (memcmp(&libzsimAddrs, &zinfo->libzsimAddrs, sizeof(libzsimAddrs)) != 0) {
        panic("libzsim.so address mismatch! text: %p != %p. Perform loader injection to homogenize offsets!", libzsimAddrs.textAddr, zinfo->libzsimAddrs.textAddr);
    }

    //Attach to debugger if needed (master process does so in SimInit, to be able to debug initialization)
    //NOTE: Pin fails to follow exec()'s when gdb is attached. The simplest way to avoid it is to kill the debugger manually before an exec(). If this is common, we could automate it
    if (!masterProcess && zinfo->attachDebugger) {
        notifyHarnessForDebugger(zinfo->harnessPid);
    }

    assert((uint32_t)procIdx < zinfo->numProcs);
    procTreeNode = zinfo->procArray[procIdx];
    if (!masterProcess) procTreeNode->notifyStart(); //masterProcess notifyStart is called in init() to avoid races
    assert(procTreeNode->getProcIdx() == (uint32_t)procIdx); //must be consistent

    trace(Process, "SHM'd global segment, starting");

    assert(zinfo->phaseLength > 0);
    assert(zinfo->maxPhases >= 0);
    assert(zinfo->statsPhaseInterval >= 0);

    perProcessEndFlag = 0;

    lineBits = ilog2(zinfo->lineSize);
    procMask = ((uint64_t)procIdx) << (64-lineBits);

    //Initialize process-local per-thread state, even if ThreadStart does so later
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        fPtrs[i] = joinPtrs;
        cids[i] = UNINITIALIZED_CID;
    }

    info("Started process, PID %d", getpid()); //NOTE: external scripts expect this line, please do not change without checking first

    //Unless things change substantially, keep this disabled; it causes higher imbalance and doesn't solve large system time with lots of processes.
    //Affinity testing code
    /*cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(procIdx % 8, &cpuset);
    int result = sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset);
    info("Affinity result %d", result);*/

    info("procMask: 0x%lx", procMask);

    if (zinfo->sched) zinfo->sched->processCleanup(procIdx);

    VirtCaptureClocks(false);
    FFIInit();

    VirtInit();

    //Register instrumentation
    TRACE_AddInstrumentFunction(Trace, 0);
    VdsoInit(); //initialized vDSO patching information (e.g., where all the possible vDSO entry points are)

    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);

    PIN_AddSyscallEntryFunction(SyscallEnter, 0);
    PIN_AddSyscallExitFunction(SyscallExit, 0);
    PIN_AddContextChangeFunction(ContextChange, 0);

    PIN_AddFiniFunction(Fini, 0);

    //Follow exec and fork
    PIN_AddFollowChildProcessFunction(FollowChild, 0);
    PIN_AddForkFunction(FPOINT_BEFORE, BeforeFork, 0);
    PIN_AddForkFunction(FPOINT_AFTER_IN_PARENT, AfterForkInParent, 0);
    PIN_AddForkFunction(FPOINT_AFTER_IN_CHILD, AfterForkInChild, 0);

    //FFwd control
    //OK, screw it. Launch this on a separate thread, and forget about signals... the caller will set a shared memory var. PIN is hopeless with signal instrumentation on multithreaded processes!
    PIN_SpawnInternalThread(FFThread, nullptr, 64*1024, nullptr);

    // Start trace-driven or exec-driven sim
    if (zinfo->traceDriven) {
        info("Running trace-driven simulation");
        while (!zinfo->terminationConditionMet && zinfo->traceDriver->executePhase()) {
            // info("Phase done");
            EndOfPhaseActions();
            zinfo->numPhases++;
            zinfo->globPhaseCycles += zinfo->phaseLength;
        }
        info("Finished trace-driven simulation");
        SimEnd();
    } else {
        // Never returns
        PIN_StartProgram();
    }
    return 0;
}

