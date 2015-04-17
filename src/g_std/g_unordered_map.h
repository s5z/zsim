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

#ifndef G_UNORDERED_MAP_H_
#define G_UNORDERED_MAP_H_

#include <functional>
#include <unordered_map>
#include "g_std/stl_galloc.h"

//template <typename K, typename V> class g_unordered_map : public std::unordered_map<K, V, StlGlobAlloc<std::pair<K const, V> > > {}; //this seems to work for TR1, not for final
template <typename K, typename V> class g_unordered_map : public std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, StlGlobAlloc<std::pair<const K, V> > > {};

#endif  // G_UNORDERED_MAP_H_
