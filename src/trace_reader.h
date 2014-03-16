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

#ifndef __TRACE_READER_H__
#define __TRACE_READER_H__

#include <iostream>
#include <fstream>
#include "log.h"

/* Simple trace reader with some error checking. Note that this is process-local,
 * and intended to be used in trace-driven single-process simulations.
 */

class TraceReader {
    private:
        std::ifstream trace;
        std::string filename;
        uint64_t records;
        uint64_t nextRecord;
    public:
        TraceReader(std::string fname);
        ~TraceReader();

        inline uint64_t read() {
            assert(nextRecord < records);
            uint64_t res = 0;
            trace.read((char*)&res, sizeof(uint64_t));
            nextRecord++;
            return res;
        }

        inline uint64_t getNumRecords() const {
            return records;
        }

        inline bool empty() const {
            return (nextRecord >= records);
        }
};

#endif /*__TRACE_READER_H__*/
