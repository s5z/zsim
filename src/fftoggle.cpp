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

/* Small utility to control ff toggling. */

#include <sched.h>
#include <stdlib.h>
#include <string>
#include "galloc.h"
#include "locks.h"
#include "log.h"
#include "zsim.h"

int main(int argc, char *argv[]) {
    InitLog("[T] ");
    if (argc < 3 || argc > 4) {
        info("Usage: %s <ff|pause|globpause|term> <shmid> [<procIdx>]", argv[0]);
        exit(1);
    }

    const char* cmd = argv[1];
    int shmid = atoi(argv[2]);
    int procIdx = (argc == 4)? atoi(argv[3]) : -1;

    gm_attach(shmid);
    while (!gm_isready()) sched_yield(); //wait till proc idx 0 initializes everything; sched_yield to avoid livelock with lots of processes
    GlobSimInfo* zinfo = static_cast<GlobSimInfo*>(gm_get_glob_ptr());

    if (strcmp(cmd, "ff") == 0) {
        if (procIdx < 0) panic("ff needs procIdx");
        futex_unlock(&zinfo->ffToggleLocks[procIdx]);
        info("Toggled fast-forward on process %d", procIdx);
    } else if (strcmp(argv[1], "pause") == 0) {
        if (procIdx < 0) panic("pause needs procIdx");
        futex_unlock(&zinfo->pauseLocks[procIdx]);
        info("Unpaused process %d", procIdx);
    } else if (strcmp(argv[1], "globpause") == 0) {
        if (procIdx >= 0) warn("globpause pauses the whole simulation, you specified a procIdx");
        zinfo->globalPauseFlag = !zinfo->globalPauseFlag; //you will not be stupid enough to run multiple fftoggles at the same time.
        __sync_synchronize();
    } else if (strcmp(argv[1], "term") == 0) {
        if (procIdx >= 0) warn("term terminates the whole simulation, you specified a procIdx");
        zinfo->externalTermPending = true;
        __sync_synchronize();
        info("Marked simulation for termination");
    } else {
        panic("Invalid command: %s", cmd);
    }
    exit(0);
}

