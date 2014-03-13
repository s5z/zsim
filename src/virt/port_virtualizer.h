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

#ifndef VIRT_PORT_VIRTUALIZER_H_
#define VIRT_PORT_VIRTUALIZER_H_

/* Simple class to keep tabs on virtualized ports */

#include "g_std/g_unordered_map.h"
#include "galloc.h"
#include "locks.h"

class PortVirtualizer : public GlobAlloc {
    private:
        g_unordered_map<int, int> realToVirt;
        g_unordered_map<int, int> virtToReal;

        lock_t pvLock;

    public:
        PortVirtualizer() {
            futex_init(&pvLock);
        }

        //Must always lock before any operation, and unlock after!
        //lock() unlock() are external because bind() spans multiple methods
        void lock() { futex_lock(&pvLock); }
        void unlock() { futex_unlock(&pvLock); }

        //Note there's no error checking for a bind that binds on a previous one.
        //If someone previous bound to that port, the virtualization code should just go ahead with that mapping and
        //either let bind() fail (if the previous bind is stil active) or succeed (if the previous bind ended)
        void registerBind(int virt, int real) {
            realToVirt[real] = virt;
            virtToReal[virt] = real;
        }

        //Returns -1 if not in map. For connect() and bind()
        int lookupReal(int virt) {
            g_unordered_map<int, int>::iterator it = virtToReal.find(virt);
            return (it == virtToReal.end())? -1 : it->second;
        }

        //Returns -1 if not in map. For getsockname(), where the OS returns real and we need virt
        int lookupVirt(int real) {
            g_unordered_map<int, int>::iterator it = realToVirt.find(real);
            return (it == realToVirt.end())? -1 : it->second;
        }
};

#endif  // VIRT_PORT_VIRTUALIZER_H_
