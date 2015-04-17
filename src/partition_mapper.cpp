/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
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

#include "partition_mapper.h"
#include "log.h"
#include "process_tree.h"
#include "zsim.h"

uint32_t CorePartMapper::getPartition(const MemReq& req) {
    return req.srcId;
}

uint32_t InstrDataPartMapper::getPartition(const MemReq& req) {
    return req.flags & MemReq::IFETCH;
}

uint32_t InstrDataCorePartMapper::getPartition(const MemReq& req) {
    bool instr = req.flags & MemReq::IFETCH;
    return req.srcId + (instr ? numCores : 0); //all instruction partitions come after data partitions
}

uint32_t ProcessPartMapper::getPartition(const MemReq& req) {
    assert(procIdx < numProcs);
    return procIdx;
}

uint32_t InstrDataProcessPartMapper::getPartition(const MemReq& req) {
    assert(procIdx < numProcs);
    bool instr = req.flags & MemReq::IFETCH;
    return procIdx + (instr ? numProcs : 0);
}

uint32_t ProcessGroupPartMapper::getNumPartitions() {
    return zinfo->numProcGroups;
}

uint32_t ProcessGroupPartMapper::getPartition(const MemReq& req) {
    uint32_t groupIdx = zinfo->procArray[procIdx]->getGroupIdx();
    assert(groupIdx < zinfo->numProcGroups);
    return groupIdx;
}

