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

#include "config.h"
#include <sstream>
#include <string.h>
#include <string>
#include <typeinfo>
#include <vector>
#include "libconfig.h++"
#include "log.h"

// We need minor specializations to work with older versions of libconfig
#if defined(LIBCONFIGXX_VER_MAJOR) && defined(LIBCONFIGXX_VER_MINOR) && defined(LIBCONFIGXX_VER_REVISION)
#define LIBCONFIG_VERSION (LIBCONFIGXX_VER_MAJOR*10000 +  LIBCONFIGXX_VER_MINOR*100 + LIBCONFIGXX_VER_REVISION)
#else
#define LIBCONFIG_VERSION 0
#endif

using std::string;
using std::stringstream;
using std::vector;

// Restrict use of long long, which libconfig uses as its int64
typedef long long lc_int64;  // NOLINT(runtime/int)

Config::Config(const char* inFile) {
    inCfg = new libconfig::Config();
    outCfg = new libconfig::Config();
    try {
        inCfg->readFile(inFile);
    } catch (libconfig::FileIOException fioe) {
        panic("Input config file %s could not be read", inFile);
    } catch (libconfig::ParseException pe) {
#if LIBCONFIG_VERSION >= 10408 // 1.4.8
        const char* peFile = pe.getFile();
#else
        // Old versions of libconfig don't have libconfig::ParseException::getFile()
        // Using inFile is typically OK, but won't be accurate with multi-file configs (includes)
        const char* peFile = inFile;
#endif
        panic("Input config file %s could not be parsed, line %d, error: %s", peFile, pe.getLine(), pe.getError());
    }
}

Config::~Config() {
    delete inCfg;
    delete outCfg;
}

// Helper function: Add "*"-prefixed vars, which are used by our scripts but not zsim, to outCfg
// Returns number of copied vars
static uint32_t copyNonSimVars(libconfig::Setting& s1, libconfig::Setting& s2, std::string prefix) {
    uint32_t copied = 0;
    for (uint32_t i = 0; i < (uint32_t)s1.getLength(); i++) {
        const char* name = s1[i].getName();
        if (name[0] == '*') {
            if (s2.exists(name)) panic("Setting %s was read, should be private", (prefix + name).c_str());
            // This could be as simple as:
            //s2.add(s1[i].getType()) = s1[i];
            // However, because Setting kinda sucks, we need to go type by type:
            libconfig::Setting& ns = s2.add(name, s1[i].getType());
            if      (libconfig::Setting::Type::TypeInt     == s1[i].getType()) ns = (int) s1[i];
            else if (libconfig::Setting::Type::TypeInt64   == s1[i].getType()) ns = (lc_int64) s1[i];
            else if (libconfig::Setting::Type::TypeBoolean == s1[i].getType()) ns = (bool) s1[i];
            else if (libconfig::Setting::Type::TypeString  == s1[i].getType()) ns = (const char*) s1[i];
            else panic("Unknown type for priv setting %s, cannot copy", (prefix + name).c_str());
            copied++;
        }

        if (s1[i].isGroup() && s2.exists(name)) {
            copied += copyNonSimVars(s1[i], s2[name], prefix + name + ".");
        }
    }
    return copied;
}

// Helper function: Compares two settings recursively, checking for inclusion
// Returns number of settings without inclusion (given but unused)
static uint32_t checkIncluded(libconfig::Setting& s1, libconfig::Setting& s2, std::string prefix) {
    uint32_t unused = 0;
    for (uint32_t i = 0; i < (uint32_t)s1.getLength(); i++) {
        const char* name = s1[i].getName();
        if (!s2.exists(name)) {
            warn("Setting %s not used during configuration", (prefix + name).c_str());
            unused++;
        } else if (s1[i].isGroup()) {
            unused += checkIncluded(s1[i], s2[name], prefix + name + ".");
        }
    }
    return unused;
}



//Called when initialization ends. Writes output config, and emits warnings for unused input settings
void Config::writeAndClose(const char* outFile, bool strictCheck) {
    uint32_t nonSimVars = copyNonSimVars(inCfg->getRoot(), outCfg->getRoot(), std::string(""));
    uint32_t unused = checkIncluded(inCfg->getRoot(), outCfg->getRoot(), std::string(""));

    if (nonSimVars) info("Copied %d non-sim var%s to output config", nonSimVars, (nonSimVars > 1)? "s" : "");
    if (unused) {
        if (strictCheck) {
            panic("%d setting%s not used during configuration", unused, (unused > 1)? "s" : "");
        } else {
            warn("%d setting%s not used during configuration", unused, (unused > 1)? "s" : "");
        }
    }

    try {
        outCfg->writeFile(outFile);
    } catch (libconfig::FileIOException fioe) {
        panic("Output config file %s could not be written", outFile);
    }
}


bool Config::exists(const char* key) {
    return inCfg->exists(key);
}

//Helper functions
template<typename T> static const char* getTypeName();
template<> const char* getTypeName<int>() {return "uint32";}
template<> const char* getTypeName<lc_int64>() {return "uint64";}
template<> const char* getTypeName<bool>() {return "bool";}
template<> const char* getTypeName<const char*>() {return "string";}
template<> const char* getTypeName<double>() {return "double";}

typedef libconfig::Setting::Type SType;
template<typename T> static SType getSType();
template<> SType getSType<int>() {return SType::TypeInt;}
template<> SType getSType<lc_int64>() {return SType::TypeInt64;}
template<> SType getSType<bool>() {return SType::TypeBoolean;}
template<> SType getSType<const char*>() {return SType::TypeString;}
template<> SType getSType<double>() {return SType::TypeFloat;}

template<typename T> static bool getEq(T v1, T v2);
template<> bool getEq<int>(int v1, int v2) {return v1 == v2;}
template<> bool getEq<lc_int64>(lc_int64 v1, lc_int64 v2) {return v1 == v2;}
template<> bool getEq<bool>(bool v1, bool v2) {return v1 == v2;}
template<> bool getEq<const char*>(const char* v1, const char* v2) {return strcmp(v1, v2) == 0;}
template<> bool getEq<double>(double v1, double v2) {return v1 == v2;}

template<typename T> static void writeVar(libconfig::Setting& setting, const char* key, T val) {
    //info("writeVal %s", key);
    const char* sep = strchr(key, '.');
    if (sep) {
        assert(*sep == '.');
        uint32_t plen = (size_t)(sep-key);
        char prefix[plen+1];
        strncpy(prefix, key, plen);
        prefix[plen] = 0;
        // libconfig strdups all passed strings, so it's fine that prefix is local.
        if (!setting.exists(prefix)) {
            try {
                setting.add((const char*)prefix, SType::TypeGroup);
            } catch (libconfig::SettingNameException sne) {
                panic("libconfig error adding group setting %s", prefix);
            }
        }
        libconfig::Setting& child = setting[(const char*)prefix];
        writeVar(child, sep+1, val);
    } else {
        if (!setting.exists(key)) {
            try {
                setting.add(key, getSType<T>()) = val;
            } catch (libconfig::SettingNameException sne) {
                panic("libconfig error adding leaf setting %s", key);
            }
        } else {
            //If this panics, what the hell are you doing in the code? Multiple reads and different defaults??
            T origVal = setting[key];
            if (!getEq(val, origVal)) panic("Duplicate writes to out config key %s with different values!", key);
        }
    }
}

template<typename T> static void writeVar(libconfig::Config* cfg, const char* key, T val) {
    libconfig::Setting& setting = cfg->getRoot();
    writeVar(setting, key, val);
}


template<typename T>
T Config::genericGet(const char* key, T def) {
    T val;
    if (inCfg->exists(key)) {
        if (!inCfg->lookupValue(key, val)) {
            panic("Type error on optional setting %s, expected type %s", key, getTypeName<T>());
        }
    } else {
        val = def;
    }
    writeVar(outCfg, key, val);
    return val;
}

template<typename T>
T Config::genericGet(const char* key) {
    T val;
    if (inCfg->exists(key)) {
        if (!inCfg->lookupValue(key, val)) {
            panic("Type error on mandatory setting %s, expected type %s", key, getTypeName<T>());
        }
    } else {
        panic("Mandatory setting %s (%s) not found", key, getTypeName<T>())
    }
    writeVar(outCfg, key, val);
    return val;
}

//Template specializations for access interface
template<> uint32_t Config::get<uint32_t>(const char* key) {return (uint32_t) genericGet<int>(key);}
template<> uint64_t Config::get<uint64_t>(const char* key) {return (uint64_t) genericGet<lc_int64>(key);}
template<> bool Config::get<bool>(const char* key) {return genericGet<bool>(key);}
template<> const char* Config::get<const char*>(const char* key) {return genericGet<const char*>(key);}
template<> double Config::get<double>(const char* key) {return (double) genericGet<double>(key);}

template<> uint32_t Config::get<uint32_t>(const char* key, uint32_t def) {return (uint32_t) genericGet<int>(key, (int)def);}
template<> uint64_t Config::get<uint64_t>(const char* key, uint64_t def) {return (uint64_t) genericGet<lc_int64>(key, (lc_int64)def);}
template<> bool Config::get<bool>(const char* key, bool def) {return genericGet<bool>(key, def);}
template<> const char* Config::get<const char*>(const char* key, const char* def) {return genericGet<const char*>(key, def);}
template<> double Config::get<double>(const char* key, double def) {return (double) genericGet<double>(key, (double)def);}

//Get subgroups in a specific key
void Config::subgroups(const char* key, std::vector<const char*>& grps) {
    if (inCfg->exists(key)) {
        libconfig::Setting& s = inCfg->lookup(key);
        uint32_t n = s.getLength(); //0 if not a group or list
        for (uint32_t i = 0; i < n; i++) {
            if (s[i].isGroup()) grps.push_back(s[i].getName());
        }
    }
}


/* Config value parsing functions */

//Range parsing, for process masks

//Helper, from http://oopweb.com/CPP/Documents/CPPHOWTO/Volume/C++Programming-HOWTO-7.html
void Tokenize(const string& str, vector<string>& tokens, const string& delimiters) {
    // Skip delimiters at beginning.
    string::size_type lastPos = 0; //dsm: DON'T //str.find_first_not_of(delimiters, 0);
    // Find first "non-delimiter".
    string::size_type pos = str.find_first_of(delimiters, lastPos);

    while (string::npos != pos || string::npos != lastPos) {
        // Found a token, add it to the vector.
        tokens.push_back(str.substr(lastPos, pos - lastPos));
        // Skip delimiters.  Note the "not_of"
        lastPos = str.find_first_not_of(delimiters, pos);
        // Find next "non-delimiter"
        pos = str.find_first_of(delimiters, lastPos);
    }
}

struct Range {
    int32_t min;
    int32_t sup;
    int32_t step;

    explicit Range(string r)  {
        vector<string> t;
        Tokenize(r, t, ":");
        vector<uint32_t> n;
        for (auto s : t) {
            stringstream ss(s);
            uint32_t x = 0;
            ss >> x;
            if (ss.fail()) panic("%s in range %s is not a valid number", s.c_str(), r.c_str());
            n.push_back(x);
        }
        switch (n.size()) {
            case 1:
                min = n[0];
                sup = min + 1;
                step = 1;
                break;
            case 2:
                min = n[0];
                sup = n[1];
                step = 1;
                break;
            case 3:
                min = n[0];
                sup = n[1];
                step = n[2];
                break;
            default:
                panic("Range '%s' can only have 1-3 numbers delimited by ':', %ld parsed", r.c_str(), n.size());
        }

        //Final error-checking
        if (min < 0 || step < 0 || sup < 0) panic("Range %s has negative numbers", r.c_str());
        if (step == 0) panic("Range %s has 0 step!", r.c_str());
        if (min >= sup) panic("Range %s has min >= sup!", r.c_str());
    }

    void fill(vector<bool>& mask) {
        for (int32_t i = min; i < sup; i += step) {
            if (i >= (int32_t)mask.size() || i < 0) panic("Range %d:%d:%d includes out-of-bounds %d (mask limit %ld)", min, step, sup, i, mask.size()-1);
            mask[i] = true;
        }
    }
};

std::vector<bool> ParseMask(const std::string& maskStr, uint32_t maskSize) {
    vector<bool> mask;
    mask.resize(maskSize);

    vector<string> ranges;
    Tokenize(maskStr, ranges, " ");
    for (auto r : ranges) {
        if (r.length() == 0) continue;
        Range range(r);
        range.fill(mask);
    }
    return mask;
}

//List parsing
template <typename T>
std::vector<T> ParseList(const std::string& listStr) {
    vector<string> nums;
    Tokenize(listStr, nums, " ");

    vector<T> res;
    for (auto n : nums) {
        if (n.length() == 0) continue;
        stringstream ss(n);
        T x;
        ss >> x;
        if (ss.fail()) panic("%s in list [%s] could not be parsed", n.c_str(), listStr.c_str());
        res.push_back(x);
    }
    return res;
}

//Instantiations
template std::vector<uint32_t> ParseList<uint32_t>(const std::string& listStr);
template std::vector<uint64_t> ParseList<uint64_t>(const std::string& listStr);
template std::vector<std::string> ParseList(const std::string& listStr);
