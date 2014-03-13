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

#ifndef DEBUG_ZSIM_H_
#define DEBUG_ZSIM_H_

#include "debug.h"

/* Gather libzsim addresses and initialize a libinfo structure.
 * This is needed to essentially replicate the line that PIN prints when
 * called with pause_tool. It uses libelf, but PIN is linked to it already
 * (I bet that PIN does pretty much the same thing).
 */
void getLibzsimAddrs(LibInfo* libzsimAddrs);

/* Signal the harness process that we're ready to be debugged */
void notifyHarnessForDebugger(int harnessPid);

#endif  // DEBUG_ZSIM_H_
