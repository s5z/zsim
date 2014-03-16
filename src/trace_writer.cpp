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

#include <iostream>
#include <fstream>

#include "constants.h" ///for TRACEFILE_MAGICNUMBER
#include "g_std/g_vector.h"
#include "trace_writer.h"

using namespace std;

TraceWriter::TraceWriter(g_string& file) : elems(0), filename(file) {
    info("XXXX");
    ofstream out(filename.c_str(), ofstream::binary);
    if (!out.is_open()) panic("Could not open %s for trace init", filename.c_str());
    volatile uint64_t hdr = TRACEFILE_MAGICNUMBER;
    out.write((const char*)&hdr, sizeof(uint64_t));
    out.close();
}

void TraceWriter::flush() {
    ofstream out(filename.c_str(), ofstream::binary | ofstream::app); //append
    if (!out.is_open()) panic("Could not open %s for trace flush", filename.c_str());
    out.write((const char*)buf, sizeof(uint64_t)*elems);
    elems = 0;
    out.close();
}

TraceWriter::~TraceWriter() {
    ofstream out(filename.c_str(), ofstream::binary | ofstream::app); //append
    if (!out.is_open()) panic("Could not open %s for trace finish", filename.c_str());
    out.write((const char*)buf, sizeof(uint64_t)*elems);
    elems = 0;
    volatile uint64_t tail = TRACEFILE_MAGICNUMBER;
    out.write((const char*)&tail, sizeof(uint64_t));
    out.close();
}

