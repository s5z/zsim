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

#ifndef CPUENUM_H_
#define CPUENUM_H_

/* Small routines for core enumeration */

#include "process_tree.h"
#include "zsim.h"

inline uint32_t cpuenumNumCpus(uint32_t pid) {
    if (zinfo->perProcessCpuEnum) {
        const g_vector<bool>& mask = zinfo->procArray[pid]->getMask();
        uint32_t count = 0;
        for (bool x : mask) count += x;
        assert(count);
        return count;
    } else {
        return zinfo->numCores;
    }
}

inline std::vector<bool> cpuenumMask(uint32_t pid) {
    std::vector<bool> res;
    if (zinfo->perProcessCpuEnum) {
        res.resize(cpuenumNumCpus(pid));
        for (uint32_t i = 0; i < res.size(); i++) res[i] = true;
    } else { 
        const g_vector<bool>& mask = zinfo->procArray[pid]->getMask();
        res.resize(mask.size());
        for (uint32_t i = 0; i < res.size(); i++) res[i] = mask[i];
    }
    return res;
}

// Returns the cpu that this cid is scheduled on, taking care of per-process cpuenum
// Can be called when app is fast-forwarding (cid == -1), it will return the first cpu
// that can run a thread from the specified pid
inline uint32_t cpuenumCpu(uint32_t pid, uint32_t cid) {
    if (zinfo->perProcessCpuEnum) {
        if (cid > zinfo->numCores) return 0;  // not scheduled; with perProcessCpuEnum, first cpu is always 0
        const g_vector<bool>& mask = zinfo->procArray[pid]->getMask();
        uint32_t count = 0;
        for (uint32_t i = 0; i < mask.size(); i++) {
            if (i == cid) return count;
            if (mask[i]) count++;
        }
        panic("Something went horribly wrong with the process masks... are they dynamic now?");
        return -1;
    } else {
        if (cid > zinfo->numCores) {  // not scheduled
            const g_vector<bool>& mask = zinfo->procArray[pid]->getMask();
            for (uint32_t i = 0; i < mask.size(); i++) {
                if (mask[i]) return i;  // first core that can run this pid
            }
            panic("Empty mask for pid %d?", pid);
            return -1;
        } else {
            return cid;
        }
    }
}

#endif  // CPUENUM_H_
