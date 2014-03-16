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

#ifndef __ACCESS_TRACING_H__
#define __ACCESS_TRACING_H__

#include "memory_hierarchy.h"
#include "trace_reader.h"
#include "trace_writer.h"

/* Thin wrappers around Tracer and TraceReader to read and write address traces in a consistent format */

struct AccessRecord {
    Address lineAddr;
    uint64_t reqCycle;
    uint32_t latency;
    uint32_t childId;
    AccessType type;
};

class AccessTraceReader {
    private:
        TraceReader tr;
        uint32_t numChildren; //i.e., how many parallel streams does this file contain?
    public:
        AccessTraceReader(std::string fname) : tr(fname) {
            assert(!tr.empty());
            numChildren = (uint32_t)tr.read();
        }

        bool empty() const {return tr.empty();}
        uint32_t getNumChildren() const {return numChildren;}
        uint64_t getNumRecords() const {return (tr.getNumRecords()-1)/3;}

        inline AccessRecord read() {
            AccessRecord res;
            res.lineAddr = tr.read();
            res.reqCycle = tr.read();
            uint64_t typeChildLat = tr.read();
            res.latency = (uint32_t)  (0x00000000FFFFFFFFL & typeChildLat);
            res.childId = (uint32_t)  (0x0FFFFL & (typeChildLat >> 32));
            res.type = (AccessType) (typeChildLat >> 48);
            //info("%ld:%d 0x%lx %d %d", );
            return res;
        }
};

class AccessTraceWriter : public GlobAlloc {
    private:
        TraceWriter tw;
    public:
        AccessTraceWriter(g_string fname, uint32_t numChildren) : tw(fname) {
            tw.write(numChildren);
        }
        
        inline void write(AccessRecord& acc) {
            uint64_t typeChildLat = (((uint64_t)acc.type)<<48) | (((uint64_t)acc.childId)<<32) | acc.latency;
            tw.write(acc.lineAddr);
            tw.write(acc.reqCycle);
            tw.write(typeChildLat);
        }

        inline TraceWriter* getTraceWriter() {
            return &tw;
        }
};

#endif /*__ACCESS_TRACING_H__*/

