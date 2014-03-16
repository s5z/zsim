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

#ifndef __TRACE_WRITER_H__
#define __TRACE_WRITER_H__

#include "log.h"
#include "galloc.h"
#include "g_std/g_string.h"

/* A simple, 64b heavily buffered writer to dump traces out */

// Buffer size in 64-bit words (note it's gm-allocated)
#define TRACEWRITER_BUFSZ (2*1024*1024) // 2M 64b words --> 16MB

class TraceWriter : public GlobAlloc {
    private:
        uint64_t buf[TRACEWRITER_BUFSZ];
        uint64_t elems;
        g_string filename;
    public:
        TraceWriter(g_string& file);
        ~TraceWriter();

        inline void write(uint64_t w) {
            buf[elems++] = w;
            if (unlikely(elems == TRACEWRITER_BUFSZ)) flush();
        }

        void flush();
};

#endif /*__TRACER_H__*/
