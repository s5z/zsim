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

#include "network.h"
#include <fstream>
#include <string>
#include "log.h"

using std::ifstream;
using std::string;

Network::Network(const char* filename) {
    ifstream inFile(filename);

    if (!inFile) {
        panic("Could not open network description file %s", filename);
    }

    while (inFile.good()) {
        string src, dst;
        uint32_t delay;
        inFile >> src;
        inFile >> dst;
        inFile >> delay;

        if (inFile.eof()) break;

        string s1 = src + " " + dst;
        string s2 = dst + " " + src;

        assert(delayMap.find(s1) == delayMap.end());
        assert(delayMap.find(s2) == delayMap.end());

        delayMap[s1] = delay;
        delayMap[s2] = delay;

        //info("Parsed %s %s %d", src.c_str(), dst.c_str(), delay);
    }

    inFile.close();
}

uint32_t Network::getRTT(const char* src, const char* dst) {
    string key(src);
    key += " ";
    key += dst;
/* dsm: Be sloppy, deadline deadline deadline
    assert_msg(delayMap.find(key) != delayMap.end(), "%s and %s cannot communicate, according to the network description file", src, dst);
    return 2*delayMap[key];
    */

    if (delayMap.find(key) != delayMap.end()) {
        return 2*delayMap[key];
    } else {
        warn("%s and %s have no entry in network description file, returning 0 latency", src, dst);
        return 0;
    }
}

