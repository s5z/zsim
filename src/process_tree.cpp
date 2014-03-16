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

#include "process_tree.h"
#include <sstream>
#include <stdlib.h>
#include <string>
#include <vector>
#include "config.h"
#include "constants.h"
#include "event_queue.h"
#include "process_stats.h"
#include "stats.h"
#include "zsim.h"

using std::string;
using std::stringstream;

static string DefaultMaskStr() {
    stringstream ss;
    ss << "0:" << zinfo->numCores;
    return ss.str();
}

//Helper
static void DumpEventualStats(uint32_t procIdx, const char* reason) {
    uint32_t p = zinfo->procArray[procIdx]->getGroupIdx();
    info("Dumping eventual stats for process GROUP %d (%s)", p, reason);
    zinfo->trigger = p;
    zinfo->eventualStatsBackend->dump(true /*buffered*/);
    zinfo->procEventualDumps++;
    if (zinfo->procEventualDumps == zinfo->maxProcEventualDumps) {
        info("Terminating, maxProcEventualDumps (%ld) reached", zinfo->maxProcEventualDumps);
        zinfo->terminationConditionMet = true; //note this works because it always runs at the end of the phase
    }
}

//Returns true if this is an actual first start, false otherwise (e.g. an exec)
bool ProcessTreeNode::notifyStart() {
    if (!started) { //first start
        uint32_t oldActiveProcs = __sync_fetch_and_add(&zinfo->globalActiveProcs, 1);
        if (procIdx) {
            if (oldActiveProcs == 0) {
                panic("Race! All processes finished before this one started, so stats have already been dumped and sim state may be partially deleted. "
                    "You should serialize process creation and termination through the harness to avoid this.");
            }
        } else { //first start
            assert(oldActiveProcs == 0);
        }

        //Set FFWD counters -- NOTE we can't call enter FFWD
        if (inFastForward) {
            if (syncedFastForward) __sync_fetch_and_add(&zinfo->globalSyncedFFProcs, 1);
            __sync_fetch_and_add(&zinfo->globalFFProcs, 1);
        }

        started = true;
        return true;
    } else { //already started
        return false;
    }
}

bool ProcessTreeNode::notifyEnd() {
    if (inFastForward) exitFastForward();
    assert(zinfo->procExited[procIdx] == PROC_RUNNING);
    uint32_t remaining;
    if (restartsLeft && !zinfo->terminationConditionMet) {
        restartsLeft--;
        info("Marking procIdx %d for restart, %d restarts left", procIdx, restartsLeft);
        zinfo->procExited[procIdx] = PROC_RESTARTME;
        return false;
    } else {
        zinfo->procExited[procIdx] = PROC_EXITED;
        remaining = __sync_sub_and_fetch(&zinfo->globalActiveProcs, 1);
        return (remaining == 0);
    }
}

void ProcessTreeNode::enterFastForward() {
    assert(!inFastForward);
    inFastForward = true;
    if (syncedFastForward) __sync_fetch_and_add(&zinfo->globalSyncedFFProcs, 1);
    __sync_fetch_and_add(&zinfo->globalFFProcs, 1);
    __sync_synchronize();
}

void ProcessTreeNode::exitFastForward() {
    assert(inFastForward);
    inFastForward = false;
    if (syncedFastForward) __sync_fetch_and_sub(&zinfo->globalSyncedFFProcs, 1);
    __sync_fetch_and_sub(&zinfo->globalFFProcs, 1);
    __sync_synchronize();
}

void ProcessTreeNode::heartbeat() {
    uint64_t curBeats = __sync_add_and_fetch(&heartbeats, 1);
    zinfo->profHeartbeats->atomicInc(procIdx);
    //info("Heartbeat, total %ld", curBeats);

    //trigger stats if we've reached the limit
    class EventualStatsDumpEvent : public Event {
        private:
            uint32_t p;
        public:
            explicit EventualStatsDumpEvent(uint64_t _p) : Event(0 /*one-shot*/), p(_p) {}
            void callback() { DumpEventualStats(p, "heartbeats"); }
    };

    if (curBeats == dumpHeartbeats) { //never triggers if dumpHeartbeats == 0
        info("Heartbeat target %ld reached, marking stats dump", curBeats);
        zinfo->eventQueue->insert(new EventualStatsDumpEvent(procIdx));

        if (dumpsResetHeartbeats) {
            info("Resetting heartbeat count (for periodic dumps)");
            __sync_sub_and_fetch(&heartbeats, curBeats);
        }
    }
}

static void PopulateLevel(Config& config, const std::string& prefix, std::vector<ProcessTreeNode*>& globProcVector, ProcessTreeNode* parent, uint32_t& procIdx, uint32_t& groupIdx) {
    uint32_t idx = 0;
    std::vector<ProcessTreeNode*> children;
    while (true) {
        std::stringstream p_ss;
        p_ss << prefix << "process" << idx;

        if (!config.exists(p_ss.str().c_str())) {
            break;
        }

        //Get patch root fs
        std::string patchRoot = config.get<const char*>(p_ss.str() +  ".patchRoot", "");

        const char* gpr = NULL;
        if (patchRoot != "") {
            //In case this is a relpath, convert it to absolute
            char* pathBuf = realpath(patchRoot.c_str(), NULL); //mallocs the buffer
            assert(pathBuf);
            gpr = gm_strdup(pathBuf);
            free(pathBuf);
        }

        bool groupWithPrevious = config.get<bool>(p_ss.str() +  ".groupWithPrevious", false);
        if (groupWithPrevious) {
            if (procIdx == 0) panic("Can't group process0 with the previous one, there is not previous process");
            assert(groupIdx > 0);
            groupIdx--;
        }


        bool startFastForwarded = config.get<bool>(p_ss.str() +  ".startFastForwarded", false);
        bool syncedFastForward = config.get<bool>(p_ss.str() +  ".syncedFastForward", true);
        bool startPaused = config.get<bool>(p_ss.str() +  ".startPaused", false);
        uint32_t clockDomain = config.get<uint32_t>(p_ss.str() +  ".clockDomain", 0);
        uint32_t portDomain = config.get<uint32_t>(p_ss.str() +  ".portDomain", 0);
        uint64_t dumpHeartbeats = config.get<uint64_t>(p_ss.str() +  ".dumpHeartbeats", 0);
        bool dumpsResetHeartbeats = config.get<bool>(p_ss.str() +  ".dumpsResetHeartbeats", false);
        uint64_t dumpInstrs = config.get<uint64_t>(p_ss.str() +  ".dumpInstrs", 0);
        uint32_t restarts = config.get<uint32_t>(p_ss.str() +  ".restarts", 0);
        g_string syscallBlacklistRegex = config.get<const char*>(p_ss.str() +  ".syscallBlacklistRegex", ".*");
        g_vector<bool> mask;
        if (!zinfo->traceDriven) {
            mask = ParseMask(config.get<const char*>(p_ss.str() +  ".mask", DefaultMaskStr().c_str()), zinfo->numCores);
        }  //  else leave mask empty, no cores
        g_vector<uint64_t> ffiPoints(ParseList<uint64_t>(config.get<const char*>(p_ss.str() +  ".ffiPoints", "")));

        if (dumpInstrs) {
            if (dumpHeartbeats) warn("Dumping eventual stats on both heartbeats AND instructions; you won't be able to distinguish both!");
            auto getInstrs = [procIdx]() { return zinfo->processStats->getProcessInstrs(procIdx); };
            auto dumpStats = [procIdx]() { DumpEventualStats(procIdx, "instructions"); };
            zinfo->eventQueue->insert(makeAdaptiveEvent(getInstrs, dumpStats, 0, dumpInstrs, MAX_IPC*zinfo->phaseLength*zinfo->numCores /*all cores can be on*/));
        } //NOTE: trivial to do the same with cycles

        if (clockDomain >= MAX_CLOCK_DOMAINS) panic("Invalid clock domain %d", clockDomain);
        if (portDomain >= MAX_PORT_DOMAINS) panic("Invalid port domain %d", portDomain);

        ProcessTreeNode* ptn = new ProcessTreeNode(procIdx, groupIdx, startFastForwarded, startPaused, syncedFastForward, clockDomain, portDomain, dumpHeartbeats, dumpsResetHeartbeats, restarts, mask, ffiPoints, syscallBlacklistRegex, gpr);
        //info("Created ProcessTreeNode, procIdx %d", procIdx);
        parent->addChild(ptn);
        children.push_back(ptn);

        assert(procIdx == globProcVector.size());
        globProcVector.push_back(ptn);

        procIdx++;
        groupIdx++;
        idx++;
    }

    for (uint32_t i = 0;  i < children.size(); i++) {
        std::stringstream p_ss;
        p_ss << prefix << "process" << i << ".";
        std::string childPrefix = p_ss.str();
        PopulateLevel(config, childPrefix, globProcVector, children[i], procIdx, groupIdx);
    }
}

void CreateProcessTree(Config& config) {
    ProcessTreeNode* rootNode = new ProcessTreeNode(-1, -1, false, false, false, 0, 0, 0, false, 0, g_vector<bool> {},  g_vector<uint64_t> {}, g_string {}, NULL);
    uint32_t procIdx = 0;
    uint32_t groupIdx = 0;
    std::vector<ProcessTreeNode*> globProcVector;

    PopulateLevel(config, std::string(""), globProcVector, rootNode, procIdx, groupIdx);

    if (procIdx > (uint32_t)zinfo->lineSize) panic("Cannot simulate more than sys.lineSize=%d processes (address spaces will get aliased), %d specified", zinfo->lineSize, procIdx);

    zinfo->procTree = rootNode;
    zinfo->numProcs = procIdx;
    zinfo->numProcGroups = groupIdx;

    zinfo->procArray = gm_calloc<ProcessTreeNode*>(zinfo->lineSize /*max procs*/); //note we can add processes later, so we size it to the maximum
    for (uint32_t i = 0; i < procIdx; i++) zinfo->procArray[i] = globProcVector[i];

    zinfo->procExited = gm_calloc<ProcExitStatus>(zinfo->lineSize /*max procs*/);
}

