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

#include "stats_filter.h"
#include <regex>
#include <string>
#include <vector>

using std::regex; using std::regex_match; using std::string; using std::vector;

// FilterStats operates recursively, building up a filtered hierarchy of aggregates

AggregateStat* FilterStatsLevel(const AggregateStat* src, const regex& filter, const char* prefix) {
    string base = prefix? (string(prefix) + src->name() + ".") : ""; //if nullptr prefix, omit our name (we're root)
    vector<Stat*> children;
    for (uint32_t i = 0; i < src->curSize(); i++) {
        Stat* child = src->get(i);
        if (AggregateStat* as = dynamic_cast<AggregateStat*>(child)) {
            AggregateStat* fs = FilterStatsLevel(as, filter, base.c_str());
            if (fs) children.push_back(fs);
        } else {
            string name = base + child->name();
            if (regex_match(name, filter)) children.push_back(child);
        }
    }

    if (children.size()) {
        AggregateStat* res = new AggregateStat(src->isRegular());
        res->init(src->name(), src->desc());
        for (Stat* c : children) res->append(c);
        return res;
    } else {
        return nullptr;
    }
}

AggregateStat* FilterStats(const AggregateStat* rootStat, const char* regexStr) {
    regex filter(regexStr);
    AggregateStat* res = FilterStatsLevel(rootStat, filter, nullptr /*root*/);
    if (res) res->makeImmutable();
    return res;
}

