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

/* Statistics facilities
 * Author: Daniel Sanchez
 * Date: Aug 2010
 *
 * There are four basic types of stats:
 * - Counter: A plain single counter.
 * - VectorCounter: A fixed-size vector of logically related counters. Each
 *   vector element may be unnamed or named (useful when enum-indexed vectors).
 * - Histogram: A GEMS-style histogram, intended to profile a distribution.
 *   It has a fixed amount of buckets, and buckets are resized as samples
 *   are added, making profiling increasingly coarser but keeping storage
 *   space constant. Unlike GEMS-style stats, though, at some configurable
 *   point part of the array starts growing logarithmically, to capture
 *   outliers without hurting accuracy of most samples.
 * - ProxyStat takes a function pointer uint64_t(*)(void) at initialization,
 *   and calls it to get its value. It is used for cases where a stat can't
 *   be stored as a counter (e.g. aggregates, RDTSC, performance counters,...)
 *   or where we have values we want to output just as stats, but would not
 *   like to treat as raw counters because e.g. they have a different type,
 *   or for efficiency reasons (e.g. the per-thread phase cycles count is
 *   updated on every BBL, and may be an uint32_t)
 *
 * Groups of stats are contained in aggregates (AggregateStat), representing
 * a collection of stats. At initialization time, all stats are registered
 * with an aggregate, forming a tree of stats. After all stats are
 * initialized, the tree of stats is made immutable; no new stats can be
 * created and output at runtime.
 *
 * These facilities are created with three goals in mind:
 * 1) Allow stats to be independent of stats output: Simulator code is only
 *    concerned with creating, naming, describing and updating a hierarchy of
 *    stats. We can then use a variety of *stats backends* to traverse and
 *    output the stats, either periodically or at specific events.
 * 2) High-performance stats: Updating counters should be as fast as updating raw
 *    integers. Counters are objects though, so they entail some space overhead.
 * 3) Allow fixed-size stats output: The stat types supported are all fixed-size,
 *    and stats cannot be created after initialization. This allows fixed-size records,
 *    making periodic stats much easier to parse and **iterate over** (e.g. we can
 *    parse 1% of the samples for a high-level graph without bringing the whole stats
 *    file from disk, then zoom in on a specific portion, etc.).
 *
 * This design was definitely influenced by the M5 stats facilities, however,
 * it is significantly simpler, doesn't use templates or has formula support,
 * and has an emphasis on fixed-size records for periodic stats.
 */

#ifndef STATS_H_
#define STATS_H_

/* TODO: I want these to be POD types, but polymorphism (needed by dynamic_cast) probably disables it. Dang. */

#include <stdint.h>
#include <string>
#include "g_std/g_vector.h"
#include "log.h"

class Stat : public GlobAlloc {
    protected:
        const char* _name;
        const char* _desc;

    public:
        Stat() : _name(nullptr), _desc(nullptr) {}

        virtual ~Stat() {}

        const char* name() const {
            assert(_name);
            return _name;
        }

        const char* desc() const {
            assert(_desc);
            return _desc;
        }

    protected:
        virtual void initStat(const char* name, const char* desc) {
            assert(name);
            assert(desc);
            assert(!_name);
            assert(!_desc);
            _name = name;
            _desc = desc;
        }
};

class AggregateStat : public Stat {
    private:
        g_vector<Stat*> _children;
        bool _isMutable;
        bool _isRegular;

    public:
        /* An aggregate stat is regular if all its children are 1) aggregate and 2) of the same type (e.g. all the threads).
         * This lets us express all the subtypes of instances of a common datatype, and this collection as an array. It is
         * useful with HDF5, where we would otherwise be forced to have huge compund datatypes, which HDF5 can't do after some
         * point.
         */
        explicit AggregateStat(bool isRegular = false) : Stat(), _isMutable(true), _isRegular(isRegular) {}

        void init(const char* name, const char* desc) {
            assert(_isMutable);
            initStat(name, desc);
        }

        //Returns true if it is a non-empty type, false otherwise. Empty types are culled by the parent.
        bool makeImmutable() {
            assert(_isMutable);
            assert(_name != nullptr); //Should have been initialized
            _isMutable = false;
            g_vector<Stat*>::iterator it;
            g_vector<Stat*> newChildren;
            for (it = _children.begin(); it != _children.end(); it++) {
                Stat* s = *it;
                AggregateStat* as = dynamic_cast<AggregateStat*>(s);
                if (as) {
                    bool emptyChild = as->makeImmutable();
                    if (!emptyChild) newChildren.push_back(s);
                } else {
                    newChildren.push_back(s);
                }
            }
            _children = newChildren;
            return _children.size() == 0;
        }

        void append(Stat* child) {
            assert(_isMutable);
            _children.push_back(child);
        }

        uint32_t size() const {
            assert(!_isMutable);
            return _children.size();
        }

        bool isRegular() const {
            return _isRegular;
        }

        Stat* get(uint32_t idx) const {
            return _children[idx];
        }

        // Access-while-mutable interface
        uint32_t curSize() const {
            return _children.size();
        }

};

/*  General scalar & vector classes */

class ScalarStat : public Stat {
    public:
        ScalarStat() : Stat() {}

        virtual void init(const char* name, const char* desc) {
            initStat(name, desc);
        }

        virtual uint64_t get() const = 0;
};

class VectorStat : public Stat {
    protected:
        const char** _counterNames;

    public:
        VectorStat() : _counterNames(nullptr) {}

        virtual uint64_t count(uint32_t idx) const = 0;
        virtual uint32_t size() const = 0;

        inline bool hasCounterNames() {
            return (_counterNames != nullptr);
        }

        inline const char* counterName(uint32_t idx) const {
            return (_counterNames == nullptr)? nullptr : _counterNames[idx];
        }

        virtual void init(const char* name, const char* desc) {
            initStat(name, desc);
        }
};


class Counter : public ScalarStat {
    private:
        uint64_t _count;

    public:
        Counter() : ScalarStat(), _count(0) {}

        void init(const char* name, const char* desc) {
            initStat(name, desc);
            _count = 0;
        }

        inline void inc(uint64_t delta) {
            _count += delta;
        }

        inline void inc() {
            _count++;
        }

        inline void atomicInc(uint64_t delta) {
            __sync_fetch_and_add(&_count, delta);
        }

        inline void atomicInc() {
            __sync_fetch_and_add(&_count, 1);
        }

        uint64_t get() const {
            return _count;
        }

        inline void set(uint64_t data) {
            _count = data;
        }
};

class VectorCounter : public VectorStat {
    private:
        g_vector<uint64_t> _counters;

    public:
        VectorCounter() : VectorStat() {}

        /* Without counter names */
        virtual void init(const char* name, const char* desc, uint32_t size) {
            initStat(name, desc);
            assert(size > 0);
            _counters.resize(size);
            for (uint32_t i = 0; i < size; i++) _counters[i] = 0;
            _counterNames = nullptr;
        }

        /* With counter names */
        virtual void init(const char* name, const char* desc, uint32_t size, const char** counterNames) {
            init(name, desc, size);
            assert(counterNames);
            _counterNames = gm_dup<const char*>(counterNames, size);
        }

        inline void inc(uint32_t idx, uint64_t value) {
            _counters[idx] += value;
        }

        inline void inc(uint32_t idx) {
             _counters[idx]++;
        }

        inline void atomicInc(uint32_t idx, uint64_t delta) {
            __sync_fetch_and_add(&_counters[idx], delta);
        }

        inline void atomicInc(uint32_t idx) {
            __sync_fetch_and_add(&_counters[idx], 1);
        }

        inline virtual uint64_t count(uint32_t idx) const {
            return _counters[idx];
        }

        inline uint32_t size() const {
            return _counters.size();
        }
};

/*
class Histogram : public Stat {
    //TBD
};
*/

class ProxyStat : public ScalarStat {
    private:
        uint64_t* _statPtr;

    public:
        ProxyStat() : ScalarStat(), _statPtr(nullptr) {}

        void init(const char* name, const char* desc, uint64_t* ptr) {
            initStat(name, desc);
            _statPtr = ptr;
        }

        uint64_t get() const {
            assert(_statPtr);  // TODO: we may want to make this work only with volatiles...
            return *_statPtr;
        }
};


class ProxyFuncStat : public ScalarStat {
    private:
        uint64_t (*_func)();

    public:
        ProxyFuncStat() : ScalarStat(), _func(nullptr) {}

        void init(const char* name, const char* desc, uint64_t (*func)()) {
            initStat(name, desc);
            _func = func;
        }

        uint64_t get() const {
            assert(_func);
            return _func();
        }
};

/*
 * Generic lambda stats
 * If your stat depends on a formula, this lets you encode it compactly using C++11 lambdas
 *
 * Usage example:
 *  auto x = [this]() { return curCycle - haltedCycles; }; //declare the lambda function that computes the stat; note this is captured because these values are class members
 *  LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x); //instantiate the templated stat. Each lambda has a unique type, which you get with decltype
 *  cyclesStat->init("cycles", "Simulated cycles"); //etc. Use as an usual stat!
 */
template <typename F>
class LambdaStat : public ScalarStat {
    private:
        F f;

    public:
        explicit LambdaStat(F _f) : f(_f) {} //copy the lambda
        uint64_t get() const {return f();}
};

template<typename F>
class LambdaVectorStat : public VectorStat {
    private:
        F f;
        uint32_t s;

    public:
        LambdaVectorStat(F _f, uint32_t _s) : VectorStat(), f(_f), s(_s) {}
        uint32_t size() const { return s; }
        uint64_t count(uint32_t idx) const { //dsm: Interestingly, this compiles even if f() is not const. gcc may catch this eventually...
            assert(idx < s);
            return f(idx);
        }
};

// Convenience creation functions
template <typename F>
LambdaStat<F>* makeLambdaStat(F f) { return new LambdaStat<F>(f); }

template<typename F>
LambdaVectorStat<F>* makeLambdaVectorStat(F f, uint32_t size) { return new LambdaVectorStat<F>(f, size); }

//Stat Backends declarations.

class StatsBackend : public GlobAlloc {
    public:
        StatsBackend() {}
        virtual ~StatsBackend() {}
        virtual void dump(bool buffered)=0;
};


class TextBackendImpl;

class TextBackend : public StatsBackend {
    private:
        TextBackendImpl* backend;

    public:
        TextBackend(const char* filename, AggregateStat* rootStat);
        virtual void dump(bool buffered);
};


class HDF5BackendImpl;

class HDF5Backend : public StatsBackend {
    private:
        HDF5BackendImpl* backend;

    public:
        HDF5Backend(const char* filename, AggregateStat* rootStat, size_t bytesPerWrite, bool skipVectors, bool sumRegularAggregates);
        virtual void dump(bool buffered);
};

#endif  // STATS_H_
