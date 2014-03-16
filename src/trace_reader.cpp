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

#include "constants.h" //for TRACEFILE_MAGICNUMBER
#include "trace_reader.h"

TraceReader::TraceReader(std::string fname) : filename(fname), nextRecord(0) {
    trace.open(filename, std::ifstream::binary);
    if (!trace.is_open()) panic("Could not open trace file %s", filename.c_str());
    uint64_t hdr = 0;
    trace.read((char*)&hdr, sizeof(uint64_t));
    if (hdr != TRACEFILE_MAGICNUMBER) panic("File %s does not begin with magic number, not a trace!", filename.c_str());

    bool hasTrailer = true;
    trace.seekg(-sizeof(uint64_t), std::ios::end);
    trace.read((char*)&hdr, sizeof(uint64_t));
    if (hdr != TRACEFILE_MAGICNUMBER) {
        warn("File %s does not end with magic number, it's an unfinished trace", filename.c_str());
        hasTrailer = false;
    }

    uint64_t fsize = trace.tellg();
    if (fsize % sizeof(uint64_t)) panic("File %s is size %ld, not an integer of record size! (%ld)", filename.c_str(), fsize, sizeof(uint64_t));

    if (fsize < 2*sizeof(uint64_t)) {
        warn("File %s is an unfinished and empty trace", filename.c_str());
        hasTrailer = false;
    }

    records = fsize/sizeof(uint64_t) - 1;
    if (hasTrailer) {
        assert(records > 0);
        records--;
    }

    trace.seekg(sizeof(uint64_t), std::ios::beg);
    assert(trace.good());
}

TraceReader::~TraceReader() {
    trace.close();
}

