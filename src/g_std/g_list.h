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

#ifndef G_LIST_H_
#define G_LIST_H_

#include <list>
#include "g_std/stl_galloc.h"

// Supposedly, this will work in C++2011
//template <typename T> typedef std::vector<T, StlGlobAlloc<T> > g_vector;

// Until GCC is compliant with this, just inherit:
template <typename T> class g_list : public std::list<T, StlGlobAlloc<T> > {};

/* Some pointers on template typedefs:
 * http://www.gotw.ca/gotw/079.htm
 * http://drdobbs.com/cpp/184403850
 * http://gcc.gnu.org/ml/gcc-help/2007-04/msg00338.html
 */

#endif  // G_LIST_H_
