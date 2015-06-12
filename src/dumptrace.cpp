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

/* Simple program to dump a trace */

#include <queue>
#include <stdio.h>

#include "access_tracing.h"
#include "galloc.h"
#include "memory_hierarchy.h"  // to translate access type to strings

int main(int argc, const char* argv[]) {
    InitLog(""); //no log header
    if (argc != 2) {
        info("Prints an access trace");
        info("Usage: %s <trace>", argv[0]);
        exit(1);
    }

    gm_init(32<<20 /*32 MB, should be enough*/);
    AccessTraceReader tr(argv[1]);

    info("%12s %6s %6s %20s %10s", "Cycle", "Src", "Type", "LineAddr", "Latency");
    while(!tr.empty()) {
        AccessRecord acc = tr.read();
        info("%12ld %6d   %s %20p %10d", acc.reqCycle, acc.childId, AccessTypeName(acc.type), (uint64_t*)acc.lineAddr, acc.latency);
    }

    return 0;
}
