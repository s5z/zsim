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

#ifndef CONFIG_H_
#define CONFIG_H_

/* Thin wrapper around libconfig to:
 * - Reduce and simplify init code (tailored interface, not type BS, ...)
 * - Strict config: type errors, warnings on unused variables, panic on different defaults
 * - Produce a full configuration file with all the variables, including defaults (for config parsing, comparison, etc.)
 */

#include <stdint.h>
#include <string>
#include <vector>
#include "log.h"

namespace libconfig {
    class Config;
    class Setting;
};


class Config {
    private:
        libconfig::Config* inCfg;
        libconfig::Config* outCfg;

    public:
        explicit Config(const char* inFile);
        ~Config();

        //Called when initialization ends. Writes output config, and emits warnings for unused input settings
        void writeAndClose(const char* outFile, bool strictCheck);

        bool exists(const char* key);
        bool exists(const std::string& key) {return exists(key.c_str());}

        //Access interface
        //T can be uint32_t, uint64_t, bool, or const char*. Instantiations are in the cpp file

        // Mandatory values (no default, panics if setting does not exist)
        template<typename T> T get(const char* key);
        template<typename T> T get(const std::string& key) {return get<T>(key.c_str());}

        // Optional values (default)
        template<typename T> T get(const char* key, T def);
        template<typename T> T get(const std::string& key, T def) {return get<T>(key.c_str(), def);}

        //Get subgroups in a specific key
        void subgroups(const char* key, std::vector<const char*>& grps);
        void subgroups(const std::string& key, std::vector<const char*>& grps) {subgroups(key.c_str(), grps);}

    private:
        template<typename T> T genericGet(const char* key);
        template<typename T> T genericGet(const char* key, T def);
};


/* Parsing functions used for configuration */

std::vector<bool> ParseMask(const std::string& maskStr, uint32_t maskSize);

/* Parses a delimiter-separated list of T's (typically ints, see/add specializtions in .cpp)
 * 0-elem lists are OK
 * panics on parsing and size-violation errors
 */
template <typename T> std::vector<T> ParseList(const std::string& listStr, const char* delimiters);

template <typename T> std::vector<T> ParseList(const std::string& listStr) { return ParseList<T>(listStr, " "); }

// fills remaining elems till maxSize with fillValue
template <typename T> std::vector<T> ParseList(const std::string& listStr, uint32_t maxSize, uint32_t fillValue) {
    std::vector<T> res = ParseList<T>(listStr);
    if (res.size() > maxSize) panic("ParseList: Too many elements, max %d, got %ld", maxSize, res.size());
    while (res.size() < maxSize) res.push_back(fillValue);
    return res;
}

void Tokenize(const std::string& str, std::vector<std::string>& tokens, const std::string& delimiters);

#endif  // CONFIG_H_
