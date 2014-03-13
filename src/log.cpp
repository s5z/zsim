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

#include "log.h"
#include <stdlib.h>
#include <string.h>
#include "locks.h"

const char* logHeader = "";

const char* logTypeNames[] = {"Harness", "Config", "Process", "Cache", "Mem", "Sched", "FSVirt", "TimeVirt"};

FILE* logFdOut = stdout;
FILE* logFdErr = stderr;

static lock_t log_printLock;


void InitLog(const char* header, const char* file) {
    logHeader = strdup(header);
    futex_init(&log_printLock);

    if (file) {
        FILE* fd = fopen(file, "a");
        if (fd == NULL) {
            perror("fopen() failed");
            panic("Could not open logfile %s", file); //we can panic in InitLog (will dump to stderr)
        }
        logFdOut = fd;
        logFdErr = fd;
        //NOTE: We technically never close this fd, but always flush it
    }
}

void __log_lock() {futex_lock(&log_printLock);}
void __log_unlock() {futex_unlock(&log_printLock);}

