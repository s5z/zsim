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

#ifndef MEMORY_HIERARCHY_H_
#define MEMORY_HIERARCHY_H_

/* Type and interface definitions of memory hierarchy objects */

#include <stdint.h>
#include "g_std/g_vector.h"
#include "galloc.h"
#include "locks.h"

/** TYPES **/

/* Addresses are plain 64-bit uints. This should be kept compatible with PIN addrints */
typedef uint64_t Address;

/* Types of Access. An Access is a request that proceeds from lower to upper
 * levels of the hierarchy (core->l1->l2, etc.)
 */
typedef enum {
    GETS, // get line, exclusive permission not needed (triggered by a processor load)
    GETX, // get line, exclusive permission needed (triggered by a processor store o atomic access)
    PUTS, // clean writeback (lower cache is evicting this line, line was not modified)
    PUTX  // dirty writeback (lower cache is evicting this line, line was modified)
} AccessType;

/* Types of Invalidation. An Invalidation is a request issued from upper to lower
 * levels of the hierarchy.
 */
typedef enum {
    INV,  // fully invalidate this line
    INVX, // invalidate exclusive access to this line (lower level can still keep a non-exclusive copy)
    FWD,  // don't invalidate, just send up the data (used by directories). Only valid on S lines.
} InvType;

/* Coherence states for the MESI protocol */
typedef enum {
    I, // invalid
    S, // shared (and clean)
    E, // exclusive and clean
    M  // exclusive and dirty
} MESIState;

//Convenience methods for clearer debug traces
const char* AccessTypeName(AccessType t);
const char* InvTypeName(InvType t);
const char* MESIStateName(MESIState s);

inline bool IsGet(AccessType t) { return t == GETS || t == GETX; }
inline bool IsPut(AccessType t) { return t == PUTS || t == PUTX; }


/* Memory request */
struct MemReq {
    Address lineAddr;
    AccessType type;
    uint32_t childId;
    MESIState* state;
    uint64_t cycle; //cycle where request arrives at component

    //Used for race detection/sync
    lock_t* childLock;
    MESIState initialState;

    //Requester id --- used for contention simulation
    uint32_t srcId;

    //Flags propagate across levels, though not to evictions
    //Some other things that can be indicated here: Demand vs prefetch accesses, TLB accesses, etc.
    enum Flag {
        IFETCH        = (1<<1), //For instruction fetches. Purely informative for now, does not imply NOEXCL (but ifetches should be marked NOEXCL)
        NOEXCL        = (1<<2), //Do not give back E on a GETS request (turns MESI protocol into MSI for this line). Used on e.g., ifetches and NUCA.
        NONINCLWB     = (1<<3), //This is a non-inclusive writeback. Do not assume that the line was in the lower level. Used on NUCA (BankDir).
        PUTX_KEEPEXCL = (1<<4), //Non-relinquishing PUTX. On a PUTX, maintain the requestor's E state instead of removing the sharer (i.e., this is a pure writeback)
        PREFETCH      = (1<<5), //Prefetch GETS access. Only set at level where prefetch is issued; handled early in MESICC
    };
    uint32_t flags;

    inline void set(Flag f) {flags |= f;}
    inline bool is (Flag f) const {return flags & f;}
};

/* Invalidation/downgrade request */
struct InvReq {
    Address lineAddr;
    InvType type;
    // NOTE: writeback should start false, children pull it up to true
    bool* writeback;
    uint64_t cycle;
    uint32_t srcId;
};

/** INTERFACES **/

class AggregateStat;
class Network;

/* Base class for all memory objects (caches and memories) */
class MemObject : public GlobAlloc {
    public:
        //Returns response cycle
        virtual uint64_t access(MemReq& req) = 0;
        virtual void initStats(AggregateStat* parentStat) {}
        virtual const char* getName() = 0;
};

/* Base class for all cache objects */
class BaseCache : public MemObject {
    public:
        virtual void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) = 0;
        virtual void setChildren(const g_vector<BaseCache*>& children, Network* network) = 0;
        virtual uint64_t invalidate(const InvReq& req) = 0;
};

#endif  // MEMORY_HIERARCHY_H_
