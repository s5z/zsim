/** $glic$
 * Copyright (C) 2012-2014 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
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

#include <fcntl.h>
#include <libgen.h>
#include "process_tree.h"
#include "str.h"
#include "virt/common.h"

static const char* fakedPaths[] = {"/proc/cpuinfo", "/proc/stat", "/sys"};

// SYS_open and SYS_openat; these are ALWAYS patched
PostPatchFn PatchOpen(PrePatchArgs args) {
    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;
    const char* patchRoot = args.patchRoot;

    uint32_t syscall = PIN_GetSyscallNumber(ctxt, std);
    assert(syscall == SYS_open || syscall == SYS_openat);

    if (!patchRoot) return NullPostPatch;  // process does not want patched system...
        
    string fileName;
    int pathReg = (syscall == SYS_open)? 0 : 1;
    ADDRINT pathArg = PIN_GetSyscallArgument(ctxt, std, pathReg);
    if (pathArg) fileName = (const char*) pathArg;  // TODO(dsm): SafeCopy
    if (syscall == SYS_openat) {
        // Get path relative to dirfd's path; if AT_CWDFD, readlink() should fail
        int dirfd = PIN_GetSyscallArgument(ctxt, std, 0);
        char buf[PATH_MAX+1];
        string fd = "/proc/self/fd/" + Str(dirfd);
        int res = readlink(fd.c_str(), buf, PATH_MAX);
        if (res > 0) {
            buf[res] = '\0';  // argh... readlink does not null-terminate strings!
            // Double-check deref'd symlink is valid
            char* rp = realpath(buf, NULL);
            if (rp) {
                fileName = string(buf) + "/" + fileName;
                free(rp);
            } else {
                panic("Not a valid path, but readlink() succeeded! %s fd %d res %d", buf, dirfd, res);
            }
        }
    }

    // Canonicalize as much as you can, even if the file does not exist
    vector<string> bases;
    string cur = fileName;
    string absPath;

    while (true) {
        char* rp = realpath(cur.c_str(), NULL);
        if (rp) {
            absPath = rp;  // copies
            free(rp);
            while (bases.size()) {
                absPath += "/" + bases.back();
                bases.pop_back();
            }
            break;  // success
        } else {
            if (!cur.size()) break;  // failed
            char* dirc = strdup(cur.c_str());
            char* basec = strdup(cur.c_str());
            char* dname = dirname(dirc);
            char* bname = basename(basec);
            bases.push_back(bname);
            cur = dname;  // copies
            free(dirc);
            free(basec);
        }
    }

    //info("Canonicalized %s -> %s", fileName.c_str(), absPath.c_str());

    if (absPath.size()) {
        bool match = false;
        for (uint32_t i = 0; i < sizeof(fakedPaths)/sizeof(const char*); i++) {
            uint32_t diff = strncmp(absPath.c_str(), fakedPaths[i], strlen(fakedPaths[i]));
            if (!diff) {
                match = true;
                break;
            }
        }

        if (match) {
            std::string patchPath = patchRoot;
            patchPath += absPath;

            bool patch = true;
            //Try to open the patched file to see if it exists
            //NOTE: We now rely on always patching; uncomment to do selectively, but this leaks info
            //FILE * patchedFd = fopen(patchPath.c_str(), "r");
            //if (patchedFd) fclose(patchedFd); else patch = false;
            if (patch) {
                char* patchPathMem = strdup(patchPath.c_str());  // in heap
                info("Patched SYS_open, original %s, patched %s", fileName.c_str(), patchPathMem);
                PIN_SetSyscallArgument(ctxt, std, pathReg, (ADDRINT) patchPathMem);

                // Restore old path on syscall exit
                return [pathReg, pathArg, patchPathMem](PostPatchArgs args) {
                    PIN_SetSyscallArgument(args.ctxt, args.std, pathReg, pathArg);
                    free(patchPathMem);
                    return PPA_NOTHING;
                };
            } else {
                info("Patched SYS_open to match %s, left unpatched (no patch)", fileName.c_str());
            }
        } else {
            //info("Non-matching SYS_open/at, path %s (canonical %s)", fileName.c_str(), absPath.c_str());
        }
    } else {
        //info("Non-realpath file %s (%s)", fileName.c_str(), pathArg);
    }

    return NullPostPatch;
}

