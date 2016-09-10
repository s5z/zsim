/** $glic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
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

#ifndef VIRT_TIME_CONV_H_
#define VIRT_TIME_CONV_H_

#include <unistd.h>
#include "zsim.h"

// Helper functions to translate between ns, timespec/timeval, and cycles

// ns per s :)
#define NSPS (1000*1000*1000L)

static inline uint64_t timevalToNs(struct timeval tv) {
    return tv.tv_sec*NSPS + tv.tv_usec*1000L;
}

static inline uint64_t timespecToNs(struct timespec ts) {
    return ts.tv_sec*NSPS + ts.tv_nsec;
}

static inline struct timeval nsToTimeval(uint64_t ns) {
    struct timeval res;
    res.tv_sec = ns/NSPS;
    res.tv_usec = (ns % NSPS)/1000;
    return res;
}

static inline struct timespec nsToTimespec(uint64_t ns) {
    struct timespec res;
    res.tv_sec = ns/NSPS;
    res.tv_nsec = (ns % NSPS);
    return res;
}

static inline uint64_t cyclesToNs(uint64_t cycles) {
    return cycles*1000/zinfo->freqMHz;
}

static inline uint64_t nsToCycles(uint64_t ns) {
    return ns*zinfo->freqMHz/1000;
}

#endif  // VIRT_TIME_CONV_H_
