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

#include "pin_cmd.h"
#include <algorithm>
#include <iostream>
#include <linux/version.h>
#include <sstream>
#include <string>
#include <wordexp.h> //for posix-shell command expansion
#include "config.h"
#include "pin.H"

//Funky macro expansion stuff
#define QUOTED_(x) #x
#define QUOTED(x) QUOTED_(x)

PinCmd::PinCmd(Config* conf, const char* configFile, const char* outputDir, uint64_t shmid) {
    //Figure the program paths
    const char* zsimEnvPath = getenv("ZSIM_PATH");
    g_string pinPath, zsimPath;
    if (zsimEnvPath) {
        info("Using env path %s", zsimEnvPath);
        pinPath = zsimEnvPath;
        pinPath += "/pinbin";
        zsimPath = zsimEnvPath;
        zsimPath += "/libzsim.so";
    } else {
        pinPath = QUOTED(PIN_PATH);
        zsimPath = QUOTED(ZSIM_PATH);
    }

    args.push_back(pinPath);

    //Global pin options
    args.push_back("-follow_execv"); //instrument child processes
    args.push_back("-tool_exit_timeout"); //don't wait much of internal threads
    args.push_back("1");

    //Additional options (e.g., -smc_strict for Java), parsed from config
    const char* pinOptions = conf->get<const char*>("sim.pinOptions", "");
    wordexp_t p;
    wordexp(pinOptions, &p, 0);
    for (uint32_t i = 0; i < p.we_wordc; i++) {
        args.push_back(g_string(p.we_wordv[i]));
    }
    wordfree(&p);

    if (PIN_PRODUCT_VERSION_MAJOR <= 2 && LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
            && std::find(args.begin(), args.end(), "-injection") == args.end()) {
        // FIXME(mgao): hack to bypass kernel version check in Pin 2.x.
        // Parent injection.
        args.push_back("-injection");
        args.push_back("parent");
    }

    //Load tool
    args.push_back("-t");
    args.push_back(zsimPath);

    //Tool options
    if (configFile) {
        //Check configFile is an absolute path
        //NOTE: We check rather than canonicalizing it ourselves because by the time we're created, we might be in another directory
        char* absPath = realpath(configFile, nullptr);
        if (std::string(configFile) != std::string(absPath)) {
            panic("Internal zsim bug, configFile should be absolute");
        }
        free(absPath);

        args.push_back("-config");
        args.push_back(configFile);
    }

    args.push_back("-outputDir");
    args.push_back(outputDir);

    std::stringstream shmid_ss;
    shmid_ss << shmid;

    args.push_back("-shmid");
    args.push_back(shmid_ss.str().c_str());

    if (conf->get<bool>("sim.logToFile", false)) {
        args.push_back("-logToFile");
    }

    //Read the per-process params of the processes run directly by the harness
    while (true) {
        std::stringstream p_ss;
        p_ss << "process" << procInfo.size();

        if (!conf->exists(p_ss.str().c_str())) break;

        const char* cmd = conf->get<const char*>(p_ss.str() +  ".command");
        const char* input = conf->get<const char*>(p_ss.str() +  ".input", "");
        const char* loader = conf->get<const char*>(p_ss.str() +  ".loader", "");
        const char* env = conf->get<const char*>(p_ss.str() +  ".env", "");

        ProcCmdInfo pi = {g_string(cmd), g_string(input), g_string(loader), g_string(env)};
        procInfo.push_back(pi);
    }
}

g_vector<g_string> PinCmd::getPinCmdArgs(uint32_t procIdx) {
    g_vector<g_string> res = args;

    std::stringstream procIdx_ss;
    procIdx_ss << procIdx;
    res.push_back("-procIdx");
    res.push_back(procIdx_ss.str().c_str());
    res.push_back("--");
    return res;
}

g_vector<g_string> PinCmd::getFullCmdArgs(uint32_t procIdx, const char** inputFile) {
    assert(procIdx < procInfo.size()); //must be one of the topmost processes
    g_vector<g_string> res = getPinCmdArgs(procIdx);

    g_string cmd = procInfo[procIdx].cmd;

    /* Loader injection: Turns out that Pin mingles with the simulated binary, which decides the loader used,
     * even when PIN_VM_LIBRARY_PATH is used. This kill the invariance on libzsim.so's loaded address, because
     * loaders in different children have different sizes. So, if specified, we prefix the program with the
     * given loader. This is optional because it won't work with statically linked binaries.
     *
     * BTW, thinking of running pin under a specific loaderto fix this instead? Nope, it gets into an infinite loop.
     */
    if (procInfo[procIdx].loader != "") {
        cmd = procInfo[procIdx].loader + " " + cmd;
        info("Injected loader on process%d, command line: %s", procIdx, cmd.c_str());
        warn("Loader injection makes Pin unaware of symbol routines, so things like routine patching"
             "will not work! You can homogeneize the loaders instead by editing the .interp ELF section");
    }

    //Parse command -- use glibc's wordexp to parse things like quotes, handle argument expansion, etc correctly
    wordexp_t p;
    wordexp(cmd.c_str(), &p, 0);
    for (uint32_t i = 0; i < p.we_wordc; i++) {
        res.push_back(g_string(p.we_wordv[i]));
    }
    wordfree(&p);

    //Input redirect
    *inputFile = (procInfo[procIdx].input == "")? nullptr : procInfo[procIdx].input.c_str();
    return res;
}

void PinCmd::setEnvVars(uint32_t procIdx) {
    assert(procIdx < procInfo.size()); //must be one of the topmost processes
    if (procInfo[procIdx].env != "") {
        wordexp_t p;
        wordexp(procInfo[procIdx].env.c_str(), &p, 0);
        for (uint32_t i = 0; i < p.we_wordc; i++) {
            char* var = strdup(p.we_wordv[i]); //putenv() does not make copies, and takes non-const char* in
            if (putenv(var) != 0) {
                panic("putenv(%s) failed", var);
            }
        }
        wordfree(&p);
    }
}

