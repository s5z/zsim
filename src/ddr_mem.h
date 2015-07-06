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

#ifndef DDR_MEM_H_
#define DDR_MEM_H_

#include <deque>

#include "g_std/g_string.h"
#include "intrusive_list.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "stats.h"


/* Helper data structures */

/* Efficiently track the activation window: A circular buffer that stores the
 * next allowed cycle we're allowed to issue an activation
 */
class ActWindow {
    private:
        g_vector<uint64_t> buf;
        uint32_t idx;

    public:
        void init(uint32_t size) {
            buf.resize(size);
            for (uint32_t i = 0; i < size; i++) buf[i] = 0;
            idx = 0;
        }

        inline uint64_t minActCycle() const {
            return buf[idx];
        }

        inline void addActivation(uint64_t actCycle) {
            assert(buf[idx] <= actCycle); // o/w we have violated tTAW/tFAW and more...

            // We need to reorder rank ACT commands, which may happen somewhat out of order
            // Typically, acts come in order or nearly in order, so doing this is pretty efficient
            // (vs e.g. scanning all last few acts to figure out the minumum constraint)
            uint32_t cur = idx;
            while (buf[dec(cur)] > actCycle) {
                buf[cur] = buf[dec(cur)];
                cur = dec(cur);
                if (cur == idx) break;  // we're the oldest in the window
            }
            buf[cur] = actCycle;

            idx = inc(idx);
        }

    private:
        inline uint32_t inc(uint32_t i) const { return (i < buf.size()-1)? i+1 : 0; }
        inline uint32_t dec(uint32_t i) const { return i? i-1 : buf.size()-1; }
};

// Read or write queues, ordered/inserted by arrival time, out-of-order finish
template <typename T>
class RequestQueue {
    private:
        struct Node : InListNode<Node> {
            T elem;
        };
        InList<Node> reqList;  // FIFO
        InList<Node> freeList; // LIFO (higher locality)

    public:
        void init(size_t size) {
            assert(reqList.empty() && freeList.empty());
            Node* buf = gm_calloc<Node>(size);
            for (uint32_t i = 0; i < size; i++) {
                new (&buf[i]) Node();
                freeList.push_back(&buf[i]);
            }
        }

        inline bool empty() const { return reqList.empty(); }
        inline bool full() const { return freeList.empty(); }
        inline size_t size() const { return reqList.size(); }

        inline T* alloc() {
            assert(!full());
            Node* n = freeList.back();
            freeList.pop_back();
            reqList.push_back(n);
            return &n->elem;
        }

        struct iterator {
            Node* n;
            explicit inline iterator(Node* _n) : n(_n) {}
            inline void inc() {n = n->next;}  // overloading prefix/postfix too messy
            inline T* operator*() const { return &(n->elem); }
            inline bool operator==(const iterator& it) const { return it.n == n; }
            inline bool operator!=(const iterator& it) const { return it.n != n; }
        };

        inline iterator begin() const {return iterator(reqList.front());}
        inline iterator end() const {return iterator(nullptr);}

        inline void remove(iterator i) {
            assert(i.n);
            reqList.remove(i.n);
            freeList.push_back(i.n);
        }
};

class DDRMemoryAccEvent;
class SchedEvent;

// Single-channel controller. For multiple channels, use multiple controllers.
class DDRMemory : public MemObject {
    private:

        struct AddrLoc {
            uint64_t row;
            uint32_t bank;
            uint32_t rank;
            uint32_t col;
        };

        struct Request : InListNode<Request> {
            Address addr;
            AddrLoc loc;
            bool write;

            uint64_t rowHitSeq; // sequence number used to throttle max # row hits

            // Cycle accounting
            uint64_t arrivalCycle;  // in memCycles
            uint64_t startSysCycle;  // in sysCycles

            // Corresponding event to send a response to
            // Writes get a response immediately, so this is nullptr for them
            DDRMemoryAccEvent* ev;
        };

        struct Bank {
            uint64_t openRow;
            bool open;  // false indicates a PRE has been issued

            // Timing constraints
            uint64_t minPreCycle;   // if !open, time of last PRE; if open, min cycle PRE can be issued
            uint64_t lastActCycle;  // cycle of last ACT command
            uint64_t lastCmdCycle;  // RD/WR command, used for refreshes only

            uint64_t curRowHits;    // row hits on the currently opened row

            InList<Request> rdReqs;
            InList<Request> wrReqs;
        };

        // Global timing constraints
        /* We wake up at minSchedCycle, issue one or more requests, and
         * reschedule ourselves at the new minSchedCycle if any requests remain
         * unserved.
         */
        uint64_t minSchedCycle; // TODO: delayed commands still not implemented
        // Minimum cycle at which the next response may arrive
        // Equivalent to first cycle that the data bus can be used
        uint64_t minRespCycle;
        bool lastCmdWasWrite;

        static const uint32_t JEDEC_BUS_WIDTH = 64;
        const uint32_t lineSize, ranksPerChannel, banksPerRank;
        const uint32_t controllerSysLatency;  // in sysCycles
        const uint32_t queueDepth;
        const uint32_t rowHitLimit; // row hits not prioritized in FR-FCFS beyond this point
        const bool deferredWrites;
        const bool closedPage;
        const uint32_t domain;

        // DRAM timing parameters -- initialized in initTech()
        // All parameters are in memory clocks (multiples of tCK)
        uint32_t tBL;    // burst length (== tTrans)
        uint32_t tCL;    // CAS latency
        uint32_t tRCD;   // ACT to CAS
        uint32_t tRTP;   // RD to PRE
        uint32_t tRP;    // PRE to ACT
        uint32_t tRRD;   // ACT to ACT
        uint32_t tRAS;   // ACT to PRE
        uint32_t tFAW;   // No more than 4 ACTs per rank in this window
        uint32_t tWTR;   // end of WR burst to RD command
        uint32_t tWR;    // end of WR burst to PRE
        uint32_t tRFC;   // Refresh to ACT (refresh leaves rows closed)
        uint32_t tREFI;  // Refresh interval

        // Address mapping information
        uint32_t colShift, colMask;
        uint32_t rankShift, rankMask;
        uint32_t bankShift, bankMask;
        uint64_t rowShift;  // row's always top

        uint32_t minRdLatency;
        uint32_t minWrLatency;
        uint32_t preDelay, postDelayRd, postDelayWr;

        RequestQueue<Request> rdQueue, wrQueue;
        std::deque<Request> overflowQueue;

        g_vector< g_vector<Bank> > banks; // indexed by rank, bank
        g_vector<ActWindow> rankActWindows;

        // Event scheduling
        SchedEvent* nextSchedEvent;
        uint64_t nextSchedCycle;
        SchedEvent* eventFreelist;

        const g_string name;

        // R/W stats
        PAD();
        Counter profReads, profWrites;
        Counter profTotalRdLat, profTotalWrLat;
        Counter profReadHits, profWriteHits;  // row buffer hits
        VectorCounter latencyHist;
        static const uint32_t BINSIZE = 10, NUMBINS = 100;
        PAD();

        //In KHz, though it does not matter so long as they are consistent and fine-grain enough (not Hz because we multiply
        //uint64_t cycles by this; as it is, KHzs are 20 bits, so we can simulate ~40+ bits (a few trillion system cycles, around an hour))
        uint64_t sysFreqKHz, memFreqKHz;

        // sys<->mem cycle xlat functions. We get and must return system cycles, but all internal logic is in memory cycles
        // will do the right thing so long as you multiply first
        inline uint64_t sysToMemCycle(uint64_t sysCycle) { return sysCycle*memFreqKHz/sysFreqKHz+1; }
        inline uint64_t memToSysCycle(uint64_t memCycle) { return (memCycle+1)*sysFreqKHz/memFreqKHz; }

        // Produces a sysCycle that, when translated back using sysToMemCycle, will produce the same memCycle
        // Requires memFreq < sysFreq/2
        inline uint64_t matchingMemToSysCycle(uint64_t memCycle) {
            // The -sysFreqKHz/memFreqKHz/2 cancels the +1 in sysToMemCycle in integer arithmetic --- you can prove this with inequalities
            return (2*memCycle-1)*sysFreqKHz/memFreqKHz/2;
        }

    public:
        DDRMemory(uint32_t _lineSize, uint32_t _colSize, uint32_t _ranksPerChannel, uint32_t _banksPerRank,
            uint32_t _sysFreqMHz, const char* tech, const char* addrMapping, uint32_t _controllerSysLatency,
            uint32_t _queueDepth, uint32_t _rowHitLimit, bool _deferredWrites, bool _closedPage,
            uint32_t _domain, g_string& _name);

        void initStats(AggregateStat* parentStat);
        const char* getName() {return name.c_str();}

        // Bound phase interface
        uint64_t access(MemReq& req);

        // Weave phase interface
        void enqueue(DDRMemoryAccEvent* ev, uint64_t cycle);
        void refresh(uint64_t sysCycle);

        // Scheduling event interface
        uint64_t tick(uint64_t sysCycle);
        void recycleEvent(SchedEvent* ev);

    private:
        AddrLoc mapLineAddr(Address lineAddr);

        void queue(Request* req, uint64_t memCycle);

        inline uint64_t trySchedule(uint64_t curCycle, uint64_t sysCycle);
        uint64_t findMinCmdCycle(const Request& r) const;

        void initTech(const char* tech);
};


#endif  // DDR_MEM_H_
