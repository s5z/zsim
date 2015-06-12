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

#ifndef ACCESS_TRACING_H_
#define ACCESS_TRACING_H_

#include "g_std/g_string.h"
#include "memory_hierarchy.h"

/* HDF5-based classes read and write address traces in a consistent format */

struct AccessRecord {
    Address lineAddr;
    uint64_t reqCycle;
    uint32_t latency;
    uint32_t childId;
    AccessType type;
};

struct PackedAccessRecord {
    uint64_t lineAddr;
    uint64_t reqCycle;
    uint32_t latency;
    uint16_t childId;
    uint16_t type;  // could be uint8_t, but causes corruption in HDF5? (wtf...)
} /*__attribute__((packed))*/;  // 24 bytes --> no packing needed


class AccessTraceReader {
    private:
        PackedAccessRecord* buf;
        uint32_t cur;
        uint32_t max;
        g_string fname;

        uint64_t curFrameRecord;
        uint64_t numRecords;
        uint32_t numChildren; //i.e., how many parallel streams does this file contain?

    public:
        AccessTraceReader(std::string fname);

        inline bool empty() const {return (cur == max);}
        uint32_t getNumChildren() const {return numChildren;}
        uint64_t getNumRecords() const {return numRecords;}

        inline AccessRecord read() {
            assert(cur < max);
            PackedAccessRecord& pr = buf[cur++];
            AccessRecord rec = {pr.lineAddr, pr.reqCycle, pr.latency, pr.childId, (AccessType) pr.type};
            if (unlikely(cur == max)) nextChunk();
            return rec;
        }

    private:
        void nextChunk();
};

class AccessTraceWriter : public GlobAlloc {
    private:
        PackedAccessRecord* buf;
        uint32_t cur;
        uint32_t max;
        g_string fname;

    public:
        AccessTraceWriter(g_string fname, uint32_t numChildren);

        inline void write(AccessRecord& acc) {
            buf[cur++] = {acc.lineAddr, acc.reqCycle, acc.latency, (uint16_t) acc.childId, (uint8_t) acc.type};
            if (unlikely(cur == max)) {
                dump(true);
                assert(cur < max);
            }
        }

        void dump(bool cont);
};

#endif  // _ACCESS_TRACING_H
