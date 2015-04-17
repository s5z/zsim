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

#ifndef GALLOC_H_
#define GALLOC_H_

#include <stdlib.h>
#include <string.h>

int gm_init(size_t segmentSize);

void gm_attach(int shmid);

// C-style interface
void* gm_malloc(size_t size);
void* __gm_calloc(size_t num, size_t size);  //deprecated, only used internally
void* __gm_memalign(size_t blocksize, size_t bytes);  // deprecated, only used internally
char* gm_strdup(const char* str);
void gm_free(void* ptr);

// C++-style alloc interface (preferred)
template <typename T> T* gm_malloc() {return static_cast<T*>(gm_malloc(sizeof(T)));}
template <typename T> T* gm_malloc(size_t objs) {return static_cast<T*>(gm_malloc(sizeof(T)*objs));}
template <typename T> T* gm_calloc() {return static_cast<T*>(__gm_calloc(1, sizeof(T)));}
template <typename T> T* gm_calloc(size_t objs) {return static_cast<T*>(__gm_calloc(objs, sizeof(T)));}
template <typename T> T* gm_memalign(size_t blocksize) {return static_cast<T*>(__gm_memalign(blocksize, sizeof(T)));}
template <typename T> T* gm_memalign(size_t blocksize, size_t objs) {return static_cast<T*>(__gm_memalign(blocksize, sizeof(T)*objs));}
template <typename T> T* gm_dup(T* src, size_t objs) {
    T* dst = gm_malloc<T>(objs);
    memcpy(dst, src, sizeof(T)*objs);
    return dst;
}

void gm_set_glob_ptr(void* ptr);
void* gm_get_glob_ptr();

void gm_set_secondary_ptr(void* ptr);
void* gm_get_secondary_ptr();

void gm_stats();

bool gm_isready();
void gm_detach();


class GlobAlloc {
    public:
        virtual ~GlobAlloc() {}

        inline void* operator new (size_t sz) {
            return gm_malloc(sz);
        }

        //Placement new
        inline void* operator new (size_t sz, void* ptr) {
            return ptr;
        }

        inline void operator delete(void *p, size_t sz) {
            gm_free(p);
        }

        //Placement delete... make ICC happy. This would only fire on an exception
        void operator delete (void* p, void* ptr) {}
};

#endif  // GALLOC_H_
