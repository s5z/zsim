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

#ifndef STATS_FILTER_H_
#define STATS_FILTER_H_

#include "stats.h"

/* Produces a filtered stats tree, where only the base stats whose names match the regex are retained.
 * Base stats are NOT copied, they are either kept or ommitted. Aggregate stats are created as needed.
 * The returned tree can be passed to any backend to produce filtered dumps. Returns NULL if nothing
 * matches the regex.
 */
AggregateStat* FilterStats(const AggregateStat* srcStat, const char* regex);

#endif  // STATS_FILTER_H_
