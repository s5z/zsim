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

#ifndef DEBUG_HARNESS_H_
#define DEBUG_HARNESS_H_

#include "debug.h"

/* Launch gdb automatically in a separate xterm window to debug the current process.
 * I'm doing this because I'm sick to death of debugging manually (wait 20 secs, attach
 * to PID, copy the libzsim.so symbol file command, etc etc).
 * Returns PID of children. Must be called from harness, since we can't fork from a pintool.
 */
int launchXtermDebugger(int targetPid, LibInfo* libzsimAddrs);

#endif  // DEBUG_HARNESS_H_
