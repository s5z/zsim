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

#ifndef STL_GALLOC_H_
#define STL_GALLOC_H_

#include <stddef.h>
#include "galloc.h"

/* Follows interface of STL allocator, allocates and frees from the global heap */

template <class T>
class StlGlobAlloc {
    public:
        typedef size_t size_type;
        typedef ptrdiff_t difference_type;
        typedef T* pointer;
        typedef const T* const_pointer;
        typedef T& reference;
        typedef const T& const_reference;
        typedef T value_type;

        StlGlobAlloc() {}
        StlGlobAlloc(const StlGlobAlloc&) {}

        pointer allocate(size_type n, const void * = 0) {
            T* t = gm_calloc<T>(n);
            return t;
        }

        void deallocate(void* p, size_type) {
            if (p) gm_free(p);
        }

        pointer address(reference x) const { return &x; }
        const_pointer address(const_reference x) const { return &x; }
        StlGlobAlloc<T>& operator=(const StlGlobAlloc&) { return *this; }


        // Construct/destroy
        // gcc keeps changing these interfaces. See /usr/include/c++/4.8/ext/new_allocator.h
#if __cplusplus >= 201103L // >= 4.8
        template<typename _Up, typename... _Args>
        void construct(_Up* __p, _Args&&... __args) { ::new((void *)__p) _Up(std::forward<_Args>(__args)...); }

        template<typename _Up> void destroy(_Up* __p) { __p->~_Up(); }
#else // < 4.8
        void construct(pointer p, const T& val) { new (static_cast<T*>(p)) T(val); }
        void construct(pointer p) { construct(p, value_type()); } //required by gcc 4.6
        void destroy(pointer p) { p->~T(); }
#endif

        size_type max_size() const { return size_t(-1); }

        template <class U> struct rebind { typedef StlGlobAlloc<U> other; };

        template <class U> StlGlobAlloc(const StlGlobAlloc<U>&) {}

        template <class U> StlGlobAlloc& operator=(const StlGlobAlloc<U>&) { return *this; }


        /* dsm: The == (and !=) operator in an allocator must be defined and,
         * per http://download.oracle.com/docs/cd/E19422-01/819-3703/15_3.htm :
         *
         *   Returns true if allocators b and a can be safely interchanged. Safely
         *   interchanged means that b could be used to deallocate storage obtained
         *   through a, and vice versa.
         *
         * We can ALWAYS do this, as deallocate just calls gm_free()
         */
        template <class U> bool operator==(const StlGlobAlloc<U>&) const { return true; }

        template <class U> bool operator!=(const StlGlobAlloc<U>&) const { return false; }
};

#endif  // STL_GALLOC_H_
