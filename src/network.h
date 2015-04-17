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

#ifndef NETWORK_H_
#define NETWORK_H_

/* Very simple fixed-delay network model. Parses a list of delays between
 * entities, then accepts queries for roundtrip times between these entities.
 * There is no contention modeling or even support for serialization latency.
 * This is a basic model that should be extended as appropriate.
 */

#include <string>
#include <unordered_map>

class Network {
    private:
        std::unordered_map<std::string, uint32_t> delayMap;

    public:
        explicit Network(const char* filename);
        uint32_t getRTT(const char* src, const char* dst);
};

#endif  // NETWORK_H_

