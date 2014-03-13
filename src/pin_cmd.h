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

#ifndef PIN_CMD_H_
#define PIN_CMD_H_

/* Interface to get pin command line */

#include <stdint.h>
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "galloc.h"

class Config;

class PinCmd : public GlobAlloc {
    private:
        g_vector<g_string> args;

        struct ProcCmdInfo {
            g_string cmd;
            g_string input;
            g_string loader;
            g_string env;
        };

        g_vector<ProcCmdInfo> procInfo; //one entry for each process that the harness launches (not for child procs)

    public:
        PinCmd(Config* conf, const char* configFile, const char* outputDir, uint64_t shmid);
        g_vector<g_string> getPinCmdArgs(uint32_t procIdx);
        g_vector<g_string> getFullCmdArgs(uint32_t procIdx, const char** inputFile);
        void setEnvVars(uint32_t procIdx);

        uint32_t getNumCmdProcs() {return procInfo.size();}
};

#endif  // PIN_CMD_H_
