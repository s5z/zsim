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


/* Simple program to sort a trace. It reads in the trace sequntially until it
 * has seen at least one access from every thread, then dumps the sorted trace
 * out. This may consume large amounts of memory if traces are largely
 * imbalanced. A simple way to fix this would be to dump N separate traces,
 * then join them together --- that's more I/O though.
 */

#include <deque>
#include <queue>
#include <stdio.h>

#include "access_tracing.h"
#include "galloc.h"
#include "trace_reader.h"
#include "trace_writer.h"

using namespace std;

void printProgress(uint64_t read, uint64_t written, uint64_t total) {
    printf("Read %3ld%% / Written %3ld%%\r", read*100/total, written*100/total);
    fflush(stdout);
}

int main(int argc, const char* argv[]) {
    InitLog(""); //no log header
    if (argc != 3) {
        info("Sorts an access trace");
        info("Usage: %s <input_trace> <output_trace>", argv[0]);
        exit(1);
    }

    gm_init(32<<20 /*32 MB --- only TraceWriter uses this, should be enough*/);

    AccessTraceReader* tr = new AccessTraceReader(argv[1]);
    uint32_t numChildren = tr->getNumChildren();
    AccessTraceWriter* tw = new AccessTraceWriter(argv[2], numChildren);

    deque<AccessRecord>* accs[numChildren]; //NULL if the child has no accesses
    for (uint32_t i = 0; i < numChildren; i++) accs[i] = NULL;
    priority_queue< pair<int64_t, uint32_t> > heads; //(negative cycle, child); we use negative cycles because priority_queue sorts from largest to smallest
    uint64_t readRecords  = 0;
    uint64_t writtenRecords  = 0;
    uint64_t totalRecords  = tr->getNumRecords();
    info("Sorting %ld records", totalRecords);

    while (!tr->empty() || heads.size() > 0) {
        if (!tr->empty() && heads.size() < numChildren) { //Read trace until all heads are filled
            for (uint32_t i = 0; i < 1024; i++) { //For performance, read at least a few accesses
                AccessRecord acc = tr->read();
                readRecords++;
                if ((readRecords % 1024) == 0) printProgress(readRecords, writtenRecords, totalRecords);
                if (!accs[acc.childId]) {
                    accs[acc.childId] = new deque<AccessRecord>();
                    heads.push(make_pair(-acc.reqCycle, acc.childId));
                }
                accs[acc.childId]->push_back(acc);
                if (tr->empty()) break;
            }
        }

        assert(heads.size() <= numChildren);

        while (heads.size() == numChildren || (tr->empty() && heads.size() > 0)) {
            pair<int64_t, uint32_t> next = heads.top();
            //info("pre %ld %d %ld", heads.size(), next.second, next.first);
            heads.pop();
            //info("post %ld %d %ld", heads.size(), heads.top().second, heads.top().first);
            uint32_t child = next.second;
            assert(!accs[child]->empty());
            AccessRecord acc = accs[child]->front();
            accs[child]->pop_front();
            tw->write(acc);
            writtenRecords++;
            if ((writtenRecords % 1024) == 0) printProgress(readRecords, writtenRecords, totalRecords);
            if (accs[child]->empty()) {
                delete accs[child];
                accs[child] = NULL;
            } else {
                AccessRecord acc = accs[child]->front();
                heads.push(make_pair(-acc.reqCycle, acc.childId));
            }
        }
    }
    printProgress(readRecords, writtenRecords, totalRecords);
    printf("\n");
    assert(readRecords == writtenRecords);
    assert(readRecords == totalRecords);

    delete tr;
    tw->dump(false); //flushes it
    delete tw;
    return 0;
}

