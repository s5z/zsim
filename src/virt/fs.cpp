/** $glic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
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

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/types.h>
#include "config.h"  // for Tokenize
#include "process_tree.h"
#include "str.h"
#include "virt/common.h"


using std::string;
using std::vector;

/* Helper functions to perform robust, incremental name resolution
 * See http://man7.org/linux/man-pages/man7/path_resolution.7.html
 * Tested against several corner cases
 */

static string getcwd() {
    char buf[PATH_MAX+1];
    char* res = getcwd(buf, PATH_MAX);
    assert(res);
    return string(res);
}

static string abspath(const string& path, const string& basepath) {
    if (path.length() == 0) return path;
    if (path[0] == '/') return path;
    return basepath + "/" + path;
}

static string dirnamepath(const string& path) {
    char* buf = strdup(path.c_str());
    string res = dirname(buf);
    free(buf);
    return res;
}

// Resolves at most one symlink, returns an absolute path
// Works fine if file does not exist --- it will return the same path
string resolvepath(const string& path) {
    string ap = abspath(path, getcwd());
    if (ap.length() == 0) return ap;

    vector<string> comps;
    Tokenize(ap, comps, "/");

    // Remove empty comps
    for (int32_t i = comps.size() - 1; i >= 0; i--) {
        if (comps[i].length() == 0) comps.erase(comps.begin() + i);
    }
    if (comps.size() == 0) return "/";

    std::string cur = "/";
    for (uint32_t i = 0; i < comps.size(); i++) {
        if (comps[i] == "..") {
            cur = dirnamepath(cur); // reaching / is safe, (/.. returns /)
            if ((i+1) < comps.size()) cur += "/";
            continue;
        }
        string p = cur + comps[i];

        char buf[PATH_MAX+1];
        int res = readlink(p.c_str(), buf, PATH_MAX);
        if (res < 0) {
            // not a symlink, keep going
            cur = p;
            if ((i+1) < comps.size()) cur += "/";
        } else {
            // NULL-terminate the string (readlink doesn't)
            assert(res <= PATH_MAX);
            buf[res] = '\0';

            // Reconstruct rest of the path
            string link = buf;
            string newpath = abspath(link, cur);
            for (uint32_t j = i+1; j < comps.size(); j++) {
                newpath += "/" + comps[j];
            }
            cur = newpath;
            break;
        }
    }
    return cur;
}

/* Path generation from patchRoot */

vector<string> listdir(string dir) {
    vector<string> files;

    DIR* d = opendir(dir.c_str());
    if (!d) panic("Invalid dir %s", dir.c_str());

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        string s = de->d_name;
        if (s == ".") continue;
        if (s == "..") continue;
        files.push_back(s);
    }

    closedir(d);
    return files;
}

vector<string>* getFakedPaths(const char* patchRoot) {
    vector<string> rootFiles = listdir(patchRoot);
    auto pi = std::find(rootFiles.begin(), rootFiles.end(), "proc");

    // HACK: We soft-patch on /proc (only patch files that exist)
    if (pi != rootFiles.end()) {
        rootFiles.erase(pi);
        vector<string> procFiles = listdir(patchRoot + string("/proc"));
        for (auto pf : procFiles) {
            rootFiles.push_back("proc/" + pf);
        }
    }

    vector<string>* res = new vector<string>();
    for (auto f : rootFiles) {
        res->push_back("/" + f);
    }
    info("PatchRoot %s, faking paths %s", patchRoot, Str(*res).c_str());
    return res;
}

static const vector<string>* fakedPaths = nullptr; //{"/proc/cputinfo", "/proc/stat", "/sys", "/lib", "/usr"};
static uint32_t numInfos = 0;
static const uint32_t MAX_INFOS = 100;

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
            char* rp = realpath(buf, nullptr);
            if (rp) {
                fileName = string(buf) + "/" + fileName;
                free(rp);
            } else {
                panic("Not a valid path, but readlink() succeeded! %s fd %d res %d", buf, dirfd, res);
            }
        }
    }

    // Try to match the path with out path matches, and resolve symlinks in path one at a time.
    // This ensures we always catch any symlink that gets us to one of the paths we intercept.
    vector<string> bases;
    string curPath = abspath(fileName, getcwd());
    uint32_t numSymlinks = 0;

    while (numSymlinks < 1024 /*avoid symlink loops*/) {
        bool match = false;
        if (!fakedPaths) fakedPaths = getFakedPaths(patchRoot);
        for (uint32_t i = 0; i < fakedPaths->size(); i++) {
            uint32_t diff = strncmp(curPath.c_str(), fakedPaths->at(i).c_str(), fakedPaths->at(i).length());
            if (!diff) {
                match = true;
                break;
            }
        }

        if (match) {
            std::string patchPath = patchRoot;
            patchPath += curPath;

            bool patch = true;
            //Try to open the patched file to see if it exists
            //NOTE: We now rely on always patching; uncomment to do selectively, but this leaks info
            //FILE * patchedFd = fopen(patchPath.c_str(), "r");
            //if (patchedFd) fclose(patchedFd); else patch = false;
            if (patch) {
                char* patchPathMem = strdup(patchPath.c_str());  // in heap
                if (numInfos <= MAX_INFOS) {
                    info("Patched SYS_open, original %s, patched %s", fileName.c_str(), patchPathMem);
                    if (numInfos == MAX_INFOS) {
                        info("(Omitting future SYS_open path messages...)");
                    }
                    numInfos++;
                }
                PIN_SetSyscallArgument(ctxt, std, pathReg, (ADDRINT) patchPathMem);

                // Restore old path on syscall exit
                return [pathReg, pathArg, patchPathMem](PostPatchArgs args) {
                    PIN_SetSyscallArgument(args.ctxt, args.std, pathReg, pathArg);
                    free(patchPathMem);
                    return PPA_NOTHING;
                };
            } else {
                info("Patched SYS_open to match %s, left unpatched (no patch)", fileName.c_str());
                return NullPostPatch;
            }
        } else {
            string newPath = resolvepath(curPath);
            if (newPath == curPath) {
                break;  // we've already resolved all the symlinks
            } else {
                numSymlinks++;
                curPath = newPath;
            }
        }
    }
    // info("Leaving SYS_open unpatched, %s", fileName.c_str());
    return NullPostPatch;
}

