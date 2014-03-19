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

// MersenneTwister.h
// Mersenne Twister random number generator -- a C++ class MTRand
// Based on code by Makoto Matsumoto, Takuji Nishimura, and Shawn Cokus
// Richard J. Wagner  v1.1  28 September 2009  wagnerr@umich.edu

// The Mersenne Twister is an algorithm for generating random numbers.  It
// was designed with consideration of the flaws in various other generators.
// The period, 2^19937-1, and the order of equidistribution, 623 dimensions,
// are far greater.  The generator is also fast; it avoids multiplication and
// division, and it benefits from caches and pipelines.  For more information
// see the inventors' web page at
// http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/emt.html

// Reference
// M. Matsumoto and T. Nishimura, "Mersenne Twister: A 623-Dimensionally
// Equidistributed Uniform Pseudo-Random Number Generator", ACM Transactions on
// Modeling and Computer Simulation, Vol. 8, No. 1, January 1998, pp 3-30.

// Copyright (C) 1997 - 2002, Makoto Matsumoto and Takuji Nishimura,
// Copyright (C) 2000 - 2009, Richard J. Wagner
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//   3. The names of its contributors may not be used to endorse or promote
//      products derived from this software without specific prior written
//      permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// The original code included the following notice:
//
//     When you use this, send an email to: m-mat@math.sci.hiroshima-u.ac.jp
//     with an appropriate reference to your work.
//
// It would be nice to CC: wagnerr@umich.edu and Cokus@math.washington.edu
// when you write.

#ifndef MTRAND_H_
#define MTRAND_H_

// Not thread safe (unless auto-initialization is avoided and each thread has
// its own MTRand object)

#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdint.h>
#include "galloc.h"

class MTRand : public GlobAlloc {
    // Data
    public:
        // typedef unsigned long uint32;  // unsigned integer type, at least 32 bits
        // dsm: WTF??? In x86-64, unsigned long is 64 bits! Using uint32_t broke
        // everything using this class, so I just turned all uint32 into uint64_t

        enum { N = 624 };       // length of state vector
        enum { SAVE = N + 1 };  // length of array for save()

    protected:
        enum { M = 397 };  // period parameter

        uint64_t state[N];   // internal state
        uint64_t *pNext;     // next value to get from state
        int left;          // number of values left before reload needed

        // Methods
    public:
        explicit MTRand(const uint64_t oneSeed);  // initialize with a simple uint64_t
        MTRand(uint64_t *const bigSeed, uint64_t const seedLength = N);  // or array
        MTRand();  // auto-initialize with /dev/urandom or time() and clock()
        explicit MTRand(const MTRand& o);  // copy

        // Do NOT use for CRYPTOGRAPHY without securely hashing several returned
        // values together, otherwise the generator state can be learned after
        // reading 624 consecutive values.

        // Access to 32-bit random numbers
        uint64_t randInt();                     // integer in [0,2^32-1]
        uint64_t randInt(const uint64_t n);     // integer in [0,n] for n < 2^32
        double rand();                        // real number in [0,1]
        double rand(const double n);        // real number in [0,n]
        double randExc();                     // real number in [0,1)
        double randExc(const double n);     // real number in [0,n)
        double randDblExc();                  // real number in (0,1)
        double randDblExc(const double n);  // real number in (0,n)
        double operator()();                  // same as rand()

        // Access to 53-bit random numbers (capacity of IEEE double precision)
        double rand53();  // real number in [0,1)

        // Access to nonuniform random number distributions
        double randNorm(const double mean = 0.0, const double stddev = 1.0);

        // Re-seeding functions with same behavior as initializers
        void seed(const uint64_t oneSeed);
        void seed(uint64_t *const bigSeed, const uint64_t seedLength = N);
        void seed();

        // Saving and loading generator state
        void save(uint64_t* saveArray) const;  // to array of size SAVE
        void load(uint64_t *const loadArray);  // from such array
        friend std::ostream& operator<<(std::ostream& os, const MTRand& mtrand);
        friend std::istream& operator>>(std::istream& is, MTRand& mtrand);
        MTRand& operator=(const MTRand& o);

    protected:
        void initialize(const uint64_t oneSeed);
        void reload();
        uint64_t hiBit(const uint64_t u) const { return u & 0x80000000UL; }
        uint64_t loBit(const uint64_t u) const { return u & 0x00000001UL; }
        uint64_t loBits(const uint64_t u) const { return u & 0x7fffffffUL; }
        uint64_t mixBits(const uint64_t u, const uint64_t v) const { return hiBit(u) | loBits(v); }
        uint64_t magic(const uint64_t u) const { return loBit(u) ? 0x9908b0dfUL : 0x0UL; }
        uint64_t twist(const uint64_t m, const uint64_t s0, const uint64_t s1) const {
            return m ^ (mixBits(s0, s1)>>1) ^ magic(s1);
        }
        static uint64_t hash(time_t t, clock_t c);
};

// Functions are defined in order of usage to assist inlining

inline uint64_t MTRand::hash(time_t t, clock_t c) {
    // Get a uint64_t from t and c
    // Better than uint64_t(x) in case x is floating point in [0,1]
    // Based on code by Lawrence Kirby (fred@genesis.demon.co.uk)

    static uint64_t differ = 0;  // guarantee time-based seeds will change

    uint64_t h1 = 0;
    unsigned char *p = (unsigned char *) &t;
    for (size_t i = 0; i < sizeof(t); ++i) {
        h1 *= UCHAR_MAX + 2U;
        h1 += p[i];
    }
    uint64_t h2 = 0;
    p = (unsigned char *) &c;
    for (size_t j = 0; j < sizeof(c); ++j) {
        h2 *= UCHAR_MAX + 2U;
        h2 += p[j];
    }
    return (h1 + differ++) ^ h2;
}

inline void MTRand::initialize(const uint64_t seed) {
    // Initialize generator state with seed
    // See Knuth TAOCP Vol 2, 3rd Ed, p.106 for multiplier.
    // In previous versions, most significant bits (MSBs) of the seed affect
    // only MSBs of the state array.  Modified 9 Jan 2002 by Makoto Matsumoto.
    register uint64_t *s = state;
    register uint64_t *r = state;
    register int i = 1;
    *s++ = seed & 0xffffffffUL;
    for (; i < N; ++i) {
        *s++ = (1812433253UL * (*r ^ (*r >> 30)) + i) & 0xffffffffUL;
        r++;
    }
}

inline void MTRand::reload() {
    // Generate N new values in state
    // Made clearer and faster by Matthew Bellew (matthew.bellew@home.com)
    static const int MmN = int(M) - int(N);  // in case enums are unsigned
    register uint64_t *p = state;
    register int i;
    for (i = N - M; i--; ++p)
        *p = twist(p[M], p[0], p[1]);
    for (i = M; --i; ++p)
        *p = twist(p[MmN], p[0], p[1]);
    *p = twist(p[MmN], p[0], state[0]);

    left = N, pNext = state;
}

inline void MTRand::seed(const uint64_t oneSeed) {
    // Seed the generator with a simple uint64_t
    initialize(oneSeed);
    reload();
}

inline void MTRand::seed(uint64_t *const bigSeed, const uint64_t seedLength) {
    // Seed the generator with an array of uint64_t's
    // There are 2^19937-1 possible initial states.  This function allows
    // all of those to be accessed by providing at least 19937 bits (with a
    // default seed length of N = 624 uint64_t's).  Any bits above the lower 32
    // in each element are discarded.
    // Just call seed() if you want to get array from /dev/urandom
    initialize(19650218UL);
    register int i = 1;
    register uint64_t j = 0;
    register int k = (N > seedLength ? N : seedLength);
    for (; k; --k) {
        state[i] =
            state[i] ^ ((state[i-1] ^ (state[i-1] >> 30)) * 1664525UL);
        state[i] += (bigSeed[j] & 0xffffffffUL) + j;
        state[i] &= 0xffffffffUL;
        ++i;  ++j;
        if (i >= N) { state[0] = state[N-1];  i = 1; }
        if (j >= seedLength) j = 0;
    }
    for (k = N - 1; k; --k) {
        state[i] =
            state[i] ^ ((state[i-1] ^ (state[i-1] >> 30)) * 1566083941UL);
        state[i] -= i;
        state[i] &= 0xffffffffUL;
        ++i;
        if (i >= N) { state[0] = state[N-1];  i = 1; }
    }
    state[0] = 0x80000000UL;  // MSB is 1, assuring non-zero initial array
    reload();
}

inline void MTRand::seed() {
    // Seed the generator with an array from /dev/urandom if available
    // Otherwise use a hash of time() and clock() values

    // First try getting an array from /dev/urandom
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        uint64_t bigSeed[N];
        register uint64_t *s = bigSeed;
        register int i = N;
        register bool success = true;
        while (success && i--)
            success = fread(s++, sizeof(uint64_t), 1, urandom);
        fclose(urandom);
        if (success) { seed(bigSeed, N); return; }
    }

    // Was not successful, so use time() and clock() instead
    seed(hash(time(nullptr), clock()));
}

inline MTRand::MTRand(const uint64_t oneSeed) { seed(oneSeed); }

inline MTRand::MTRand(uint64_t *const bigSeed, const uint64_t seedLength) {
    seed(bigSeed, seedLength);
}

inline MTRand::MTRand() { seed(); }

inline MTRand::MTRand(const MTRand& o) {
    register const uint64_t *t = o.state;
    register uint64_t *s = state;
    register int i = N;
    for (; i--; *s++ = *t++) {}
    left = o.left;
    pNext = &state[N-left];
}

inline uint64_t MTRand::randInt() {
    // Pull a 32-bit integer from the generator state
    // Every other access function simply transforms the numbers extracted here

    if (left == 0) reload();
    --left;

    register uint64_t s1;
    s1 = *pNext++;
    s1 ^= (s1 >> 11);
    s1 ^= (s1 <<  7) & 0x9d2c5680UL;
    s1 ^= (s1 << 15) & 0xefc60000UL;
    return (s1 ^ (s1 >> 18));
}

inline uint64_t MTRand::randInt(const uint64_t n) {
    // Find which bits are used in n
    // Optimized by Magnus Jonsson (magnus@smartelectronix.com)
    uint64_t used = n;
    used |= used >> 1;
    used |= used >> 2;
    used |= used >> 4;
    used |= used >> 8;
    used |= used >> 16;

    // Draw numbers until one is found in [0,n]
    uint64_t i;
    do {
        i = randInt() & used;  // toss unused bits to shorten search
    } while (i > n);
    return i;
}

inline double MTRand::rand() { return double(randInt()) * (1.0/4294967295.0); }

inline double MTRand::rand(const double n) { return rand() * n; }

inline double MTRand::randExc() { return double(randInt()) * (1.0/4294967296.0); }

inline double MTRand::randExc(const double n) { return randExc() * n; }

inline double MTRand::randDblExc() { return (double(randInt()) + 0.5) * (1.0/4294967296.0); }

inline double MTRand::randDblExc(const double n) { return randDblExc() * n; }

inline double MTRand::rand53() {
    uint64_t a = randInt() >> 5, b = randInt() >> 6;
    return (a * 67108864.0 + b) * (1.0/9007199254740992.0);  // by Isaku Wada
}

inline double MTRand::randNorm(const double mean, const double stddev) {
    // Return a real number from a normal (Gaussian) distribution with given
    // mean and standard deviation by polar form of Box-Muller transformation
    double x, y, r;
    do {
        x = 2.0 * rand() - 1.0;
        y = 2.0 * rand() - 1.0;
        r = x * x + y * y;
    } while (r >= 1.0 || r == 0.0);
    double s = sqrt(-2.0 * log(r) / r);
    return mean + x * s * stddev;
}

inline double MTRand::operator()() {
    return rand();
}

inline void MTRand::save(uint64_t* saveArray) const {
    register const uint64_t *s = state;
    register uint64_t *sa = saveArray;
    register int i = N;
    for (; i--; *sa++ = *s++) {}
    *sa = left;
}

inline void MTRand::load(uint64_t *const loadArray) {
    register uint64_t *s = state;
    register uint64_t *la = loadArray;
    register int i = N;
    for (; i--; *s++ = *la++) {}
    left = *la;
    pNext = &state[N-left];
}

inline std::ostream& operator<<(std::ostream& os, const MTRand& mtrand) {
    register const uint64_t *s = mtrand.state;
    register int i = mtrand.N;
    for (; i--; os << *s++ << "\t") {}
    return os << mtrand.left;
}

inline std::istream& operator>>(std::istream& is, MTRand& mtrand) {
    register uint64_t *s = mtrand.state;
    register int i = mtrand.N;
    for (; i--; is >> *s++) {}
    is >> mtrand.left;
    mtrand.pNext = &mtrand.state[mtrand.N-mtrand.left];
    return is;
}

inline MTRand& MTRand::operator=(const MTRand& o) {
    if (this == &o) return (*this);
    register const uint64_t *t = o.state;
    register uint64_t *s = state;
    register int i = N;
    for (; i--; *s++ = *t++) {}
    left = o.left;
    pNext = &state[N-left];
    return (*this);
}

#endif  // MTRAND_H_

// Change log:
//
// v0.1 - First release on 15 May 2000
//      - Based on code by Makoto Matsumoto, Takuji Nishimura, and Shawn Cokus
//      - Translated from C to C++
//      - Made completely ANSI compliant
//      - Designed convenient interface for initialization, seeding, and
//        obtaining numbers in default or user-defined ranges
//      - Added automatic seeding from /dev/urandom or time() and clock()
//      - Provided functions for saving and loading generator state
//
// v0.2 - Fixed bug which reloaded generator one step too late
//
// v0.3 - Switched to clearer, faster reload() code from Matthew Bellew
//
// v0.4 - Removed trailing newline in saved generator format to be consistent
//        with output format of built-in types
//
// v0.5 - Improved portability by replacing static const int's with enum's and
//        clarifying return values in seed(); suggested by Eric Heimburg
//      - Removed MAXINT constant; use 0xffffffffUL instead
//
// v0.6 - Eliminated seed overflow when uint32 is larger than 32 bits
//      - Changed integer [0,n] generator to give better uniformity
//
// v0.7 - Fixed operator precedence ambiguity in reload()
//      - Added access for real numbers in (0,1) and (0,n)
//
// v0.8 - Included time.h header to properly support time_t and clock_t
//
// v1.0 - Revised seeding to match 26 Jan 2002 update of Nishimura and Matsumoto
//      - Allowed for seeding with arrays of any length
//      - Added access for real numbers in [0,1) with 53-bit resolution
//      - Added access for real numbers from normal (Gaussian) distributions
//      - Increased overall speed by optimizing twist()
//      - Doubled speed of integer [0,n] generation
//      - Fixed out-of-range number generation on 64-bit machines
//      - Improved portability by substituting literal constants for long enum's
//      - Changed license from GNU LGPL to BSD
//
// v1.1 - Corrected parameter label in randNorm from "variance" to "stddev"
//      - Changed randNorm algorithm from basic to polar form for efficiency
//      - Updated includes from deprecated <xxxx.h> to standard <cxxxx> forms
//      - Cleaned declarations and definitions to please Intel compiler
//      - Revised twist() operator to work on ones'-complement machines
//      - Fixed reload() function to work when N and M are unsigned
//      - Added copy constructor and copy operator from Salvador Espana
