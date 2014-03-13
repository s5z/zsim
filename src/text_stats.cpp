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

#include <fstream>
#include <iostream>
#include "galloc.h"
#include "log.h"
#include "stats.h"
#include "zsim.h"

using std::endl;

class TextBackendImpl : public GlobAlloc {
    private:
        const char* filename;
        AggregateStat* rootStat;

        void dumpStat(Stat* s, uint32_t level, std::ofstream* out) {
            for (uint32_t i = 0; i < level; i++) *out << " ";
            *out << s->name() << ": ";
            if (AggregateStat* as = dynamic_cast<AggregateStat*>(s)) {
                *out << "# " << as->desc() << endl;
                for (uint32_t i = 0; i < as->size(); i++) {
                    dumpStat(as->get(i), level+1, out);
                }
            } else if (Counter* cs = dynamic_cast<Counter*>(s)) {
                *out << cs->count() << " # " << cs->desc() << endl;
            } else if (ScalarStat* ss = dynamic_cast<ScalarStat*>(s)) {
                *out << ss->get() << " # " << ss->desc() << endl;
            } else if (VectorStat* vs = dynamic_cast<VectorStat*>(s)) {
                *out << "# " << vs->desc() << endl;
                for (uint32_t i = 0; i < vs->size(); i++) {
                    for (uint32_t j = 0; j < level+1; j++) *out << " ";
                    if (vs->hasCounterNames()) {
                        *out << vs->counterName(i) << ": " << vs->count(i) << endl;
                    } else {
                        *out << i << ": " << vs->count(i) << endl;
                    }
                }
            } else if (ProxyStat* ps = dynamic_cast<ProxyStat*>(s)) {
                *out << ps->stat() << " # " << ps->desc() << endl;
            } else if (ProxyFuncStat* pfs = dynamic_cast<ProxyFuncStat*>(s)) {
                *out << pfs->stat() << " # " << pfs->desc() << endl;
            } else {
                panic("Unrecognized stat type");
            }
        }

    public:
        TextBackendImpl(const char* _filename, AggregateStat* _rootStat) :
            filename(_filename), rootStat(_rootStat)
        {
            std::ofstream out(filename, std::ios_base::out);
            out << "# zsim stats" << endl;
            out << "===" << endl;
        }

        void dump(bool buffered) {
            std::ofstream out(filename, std::ios_base::app);
            dumpStat(rootStat, 0, &out);
            out << "===" << endl;
        }
};

TextBackend::TextBackend(const char* filename, AggregateStat* rootStat) {
    backend = new TextBackendImpl(filename, rootStat);
}

void TextBackend::dump(bool buffered) {
    backend->dump(buffered);
}

