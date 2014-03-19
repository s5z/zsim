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

#ifndef __DETAILED_MEM_H__
#define __DETAILED_MEM_H__

#include "detailed_mem_params.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "timing_event.h"
#include <zlib.h>

/* Detailed memory model from Makoto/Kenta */

// FIXME(dsm): This enum should not be our here, esp with such generic names!
enum MemAccessType { READ, WRITE, NUM_ACCESS_TYPES};

// DRAM rank base class
class MemRankBase : public GlobAlloc {
    protected:
        uint32_t myId;
        uint32_t parentId;
        uint32_t bankCount;

        uint32_t lastBank;
        uint64_t lastAccessCycle;

        uint64_t lastRefreshCycle;
        uint32_t refreshNum;
        uint32_t accessInRefresh;
        uint32_t tFAWIndex;

        bool *bankinfo;
        uint32_t *lastRow;
        MemAccessType* lastType;
        uint64_t *lastActCycle;
        uint64_t *lastRdWrCycle;
        uint64_t *lastPreCycle;
        uint64_t *tFAWCycle;

        uint64_t activateCount;
        uint64_t prechargeCount;
        uint64_t readBurstCount;
        uint64_t writeBurstCount;

        uint64_t idlePowerDownCycle;
        uint64_t actvPowerDownCycle;
        uint64_t idleStandbyCycle;

        uint64_t prevIdlePowerDownCycle;
        uint64_t prevActvPowerDownCycle;
        uint64_t prevIdleStandbyCycle;

    public:
        MemRankBase(uint32_t _myId, uint32_t _parentId, uint32_t _bankCount);
        virtual ~MemRankBase();

        virtual void access(uint64_t accessCycle, uint64_t issuedCycle, uint32_t row, uint32_t col, uint32_t bank, MemAccessType type);
        virtual void refresh(uint64_t lastCycle);

        // FIXME(dsm): This huge amount of getters/setters is a telltale sign
        // of bad design (if an external class needs to access all these
        // fields, why is not the logic of that class here? and if the logic is
        // not here, why is this not a struct internal to that class?)
        uint32_t GetBankCount(void) {return bankCount; }
        uint32_t GetLastBank(void) { return lastBank; }
        uint32_t GetLastRow(uint32_t bank) { return lastRow[bank]; }
        MemAccessType GetLastType(uint32_t bank) { return lastType[bank]; }
        uint64_t GetLastRdWrCycle(uint32_t bank) { return lastRdWrCycle[bank]; }
        uint64_t GetLastRefreshCycle(void) { return lastRefreshCycle; }

        bool GetBankOpen(uint32_t bank) { return bankinfo[bank]; }
        void SetBankOpen(uint32_t bank)  { bankinfo[bank] = true; }
        void SetBankClose(uint32_t bank) { bankinfo[bank] = false; }
        uint32_t GetActiveBankCount(void);
        uint64_t GetLastAccessCycle(void) { return lastAccessCycle; }

        uint64_t GetActivateCount(void) { return activateCount; }
        void IncActivateCount(void) { activateCount++; }
        uint32_t GetPrechargeCount(void) { return prechargeCount; }
        void IncPrechargeCount(void) { prechargeCount++; }

        uint64_t GetReadBurstCount(void) { return readBurstCount; }
        void IncReadBurstCount(void) { readBurstCount++; }
        uint64_t GetWriteBurstCount(void) { return writeBurstCount; }
        void IncWriteBurstCount(void) { writeBurstCount++; }

        uint64_t GetIdlePowerDownCycle(void) { return idlePowerDownCycle; }
        uint64_t GetActvPowerDownCycle(void) { return actvPowerDownCycle; }
        uint64_t GetIdleStandbyCycle(void) { return idleStandbyCycle; }
        uint64_t GetPrevIdlePowerDownCycle(void) { return prevIdlePowerDownCycle; }
        uint64_t GetPrevActvPowerDownCycle(void) { return prevActvPowerDownCycle; }
        uint64_t GetPrevIdleStandbyCycle(void) { return prevIdleStandbyCycle; }

        void SetIdlePowerDownCycle(uint64_t cycle) { assert(idlePowerDownCycle <= cycle); idlePowerDownCycle = cycle; }
        void SetActvPowerDownCycle(uint64_t cycle) { assert(actvPowerDownCycle <= cycle); actvPowerDownCycle = cycle; }
        void SetIdleStandbyCycle(uint64_t cycle) { assert(idleStandbyCycle <= cycle); idleStandbyCycle = cycle; }
        void SaveBackgroundCycles(void);

        void SetRefreshNum(uint32_t num) { assert(refreshNum <= num); refreshNum = num;}
        uint32_t GetRefreshNum(void) { return refreshNum; }
        void SetAccessInRefresh(uint32_t num) { accessInRefresh = num; }
        uint32_t GetAccessInRefresh(void) { return accessInRefresh; }

        uint64_t GetLastActCycle(uint32_t bank) { return lastActCycle[bank]; }
        void SetLastActCycle(uint32_t bank, uint64_t cycle) { assert(lastActCycle[bank] <= cycle); lastActCycle[bank] = cycle; }
        uint64_t GetLastPreCycle(uint32_t bank) { return lastPreCycle[bank]; }
        void SetLastPreCycle(uint32_t bank, uint64_t cycle) { assert(lastPreCycle[bank] <= cycle); lastPreCycle[bank] = cycle; }
        uint64_t GetFAWCycle(void) { return tFAWCycle[tFAWIndex]; }
        void SetFAWCycle(uint32_t bank, uint64_t cycle) { assert(tFAWCycle[bank] <= cycle); tFAWCycle[bank] = cycle; }
        uint64_t GetFAWCycle(uint32_t bank) { return tFAWCycle[bank]; }
        void SetFAWCycle(uint64_t cycle) { assert(tFAWCycle[tFAWIndex] <= cycle); tFAWCycle[tFAWIndex++] = cycle; tFAWIndex %= 4; }
};

// DRAM channel base class
class MemChannelBase : public GlobAlloc {
    protected:
        uint32_t myId;
        MemParam *mParam;

        g_vector <MemRankBase*> ranks;
        std::vector<std::pair<uint64_t, uint64_t> > accessLog;

        virtual uint32_t UpdateRefreshNum(uint32_t rank, uint64_t arrivalCycle);
        virtual uint64_t UpdateLastRefreshCycle(uint32_t rank, uint64_t arrivalCycle, uint32_t refreshNum);
        virtual void UpdatePowerDownCycle(uint32_t rank, uint64_t arrivalCycle, uint64_t lastPhaseCycle, uint32_t refreshNum);
        virtual void UpdateDataBusCycle(uint64_t start, uint64_t end);

        virtual void IssueActivate(uint32_t rank, uint32_t bank, uint64_t issuedCycle);
        virtual void IssuePrecharge(uint32_t rank, uint32_t bank, uint64_t issuedCycle, bool continuous = false);

        virtual uint64_t CalcIntraIssueCycle(bool rowHit, uint32_t rank, MemAccessType type, uint64_t arrivalCycle, uint32_t refreshNum);
        virtual uint64_t CalcInterIssueCycle(MemAccessType type, uint64_t arrivalCycle);
        virtual uint64_t CalcActConst(uint32_t rank, uint32_t bank, uint64_t issuableCycle);
        virtual uint64_t CalcPreConst(uint32_t rank, uint32_t bank, MemAccessType type, uint64_t issuableCycle);
        virtual uint64_t CalcRdWrConst(uint32_t rank, MemAccessType type, uint64_t issuableCycle);

        virtual uint32_t GetPowerDownPenalty(uint32_t rank, uint64_t arrivalCycle);
        virtual bool     CheckContinuousAccess(uint64_t arrivalCycle, uint32_t rank, uint32_t bank, uint32_t row);

    public:
        MemChannelBase(uint32_t _myId, MemParam *_mParam);
        virtual ~MemChannelBase();

        virtual uint64_t LatencySimulate(Address lineAddr, uint64_t arrivalCycle, uint64_t lastPhaseCycle, MemAccessType type);
        virtual void AddressMap(Address addr, uint32_t& row, uint32_t& col, uint32_t& rank, uint32_t& bank);
        bool IsRowBufferHit(uint32_t row, uint32_t rank, uint32_t bank);

        virtual uint64_t GetActivateCount(void);
        virtual uint64_t GetPrechargeCount(void);
        virtual uint64_t GetRefreshCount(void);

        virtual uint64_t GetBurstEnergy(void);
        virtual uint64_t GetActPreEnergy(void);
        virtual uint64_t GetRefreshEnergy(void);
        virtual uint64_t GetBackGroundEnergy(uint64_t memCycle, uint64_t lastMemCycle, bool bInstant = false);

        virtual void PeriodicUpdatePower(uint64_t phaseCycle, uint64_t lastPhaseCycle);
};

class MemAccessEventBase;

// DRAM scheduler base class
class MemSchedulerBase : public GlobAlloc {
    protected:
        // HK: Umm...MemAccessEventBase already has a member named addr. How is the
        // Address in MemSchedQueueElem different from this?
        typedef std::pair<MemAccessEventBase*, Address> MemSchedQueueElem;

        uint32_t id;
        MemParam* mParam;
        MemChannelBase* mChnl;

    public:

        MemSchedulerBase(uint32_t id, MemParam* mParam, MemChannelBase* mChnl)
            : id(id), mParam(mParam), mChnl(mChnl) {}

        virtual ~MemSchedulerBase() {}

        virtual bool CheckSetEvent(MemAccessEventBase* ev) = 0;

        // HK: I hope there's a good reason to be using a reference to a pointer here
        // Don't know the code enough at the moment to be able to tell.
        //
        // Hmm...so upon further investigation it looks like all of these arguments are 
        // written by the function. I am not a big fan of passing WRITE arguments by 
        // reference. Even more distrubingly, MemSchedQueueElem uses its MemAccessEventBase
        // member in a weird way, with the member var being nullptr signifying something (I don't
        // know what yet). Will look into this further
        //
        // FIXME(dsm): refpointer? pointeref? Hmmm...
        virtual bool GetEvent(MemAccessEventBase*& ev, Address& addr, MemAccessType& type) = 0;
};

class MemSchedulerDefault : public MemSchedulerBase {
    private:
        MemAccessType prioritizedAccessType;
        uint32_t wrQueueSize;
        uint32_t wrQueueHighWatermark;
        uint32_t wrQueueLowWatermark;

        g_vector <MemSchedQueueElem> rdQueue;
        g_vector <MemSchedQueueElem> wrQueue;
        g_vector <MemSchedQueueElem> wrDoneQueue;

        bool FindBestRequest(g_vector <MemSchedQueueElem> *queue, uint32_t& idx);

    public:
        MemSchedulerDefault(uint32_t id, MemParam* mParam, MemChannelBase* mChnl);
        ~MemSchedulerDefault();
        bool CheckSetEvent(MemAccessEventBase* ev);
        bool GetEvent(MemAccessEventBase*& ev, Address& addr, MemAccessType& type);
};

// DRAM controller base class
class MemControllerBase : public MemObject {
    protected:
        g_string name;
        uint32_t domain;
        uint32_t cacheLineSize;

        MemParam* mParam;
        g_vector <MemChannelBase*> chnls;
        g_vector <MemSchedulerBase*> sches;
        lock_t updateLock;

        uint64_t sysFreqKHz;
        uint64_t memFreqKHz;

        uint64_t lastPhaseCycle;
        uint64_t lastAccessedCycle;
        uint64_t nextSysTick;
        uint64_t reportPeriodCycle;

        // latency
        uint32_t minLatency[NUM_ACCESS_TYPES];
        uint32_t preDelay[NUM_ACCESS_TYPES];
        uint32_t postDelay[NUM_ACCESS_TYPES];
        uint32_t memMinLatency[NUM_ACCESS_TYPES];

        virtual uint64_t ReturnChannel(Address addr);
        virtual uint64_t LatencySimulate(Address lineAddr, uint64_t sysCycle, MemAccessType type);
        virtual void UpdateCmdCounters(void);
        virtual void EstimatePowers(uint64_t sysCycle, bool finish = false);
        virtual void EstimateBandwidth(uint64_t realTime, uint64_t lastTime, bool finish = false);
        virtual uint64_t CalcDQTermCur(uint64_t acc_dq, uint64_t last_dq, uint64_t instCycle, uint64_t memCycle, uint64_t lastMemCycle);
        virtual uint64_t CalcDQTermAcc(uint64_t acc_dq, uint64_t memCycle, uint64_t lastMemCycle);
        virtual void TickScheduler(uint64_t sysCycle);

        inline uint64_t sysToMemCycle(uint64_t sysCycle) { return sysCycle*memFreqKHz/sysFreqKHz; }
        inline uint64_t sysToMicroSec(uint64_t sysCycle) { return sysCycle*1000/sysFreqKHz; }
        inline uint64_t usecToSysCycle(uint64_t usec)    { return usec*sysFreqKHz/1000; }
        inline uint64_t memToSysCycle(uint64_t memCycle) { return memCycle*sysFreqKHz/memFreqKHz; }
        inline uint64_t memToMicroSec(uint64_t memCycle) { return memCycle*1000/memFreqKHz; }

        // profiles
        Counter profReads;
        Counter profWrites;
        Counter profTotalRdLat;
        Counter profTotalWrLat;
        VectorCounter latencyHist;
        uint32_t lhBinSize;
        uint32_t lhNumBins;

        Counter profActivate;
        Counter profPrecharge;
        Counter profRefresh;

        static const uint32_t pwCounterNum = 7;
        Counter profAccAvgPower[pwCounterNum];
        Counter profCurAvgPower[pwCounterNum];
        static const uint32_t bwCounterNum = 4;
        Counter profBandwidth[bwCounterNum];

        uint64_t lastAccesses;
        uint64_t maxBandwidth;
        uint64_t minBandwidth;

        gzFile addrTraceLog;

        // Power
        uint64_t lastMemCycle;
        struct powerValue {
            uint64_t total;
            uint64_t actPre;
            uint64_t burst;
            uint64_t refresh;
            uint64_t background;
            uint64_t dq;
            uint64_t terminate;
        };
        powerValue lastPower;


    public:
        MemControllerBase(g_string _memCfg, uint32_t _cacheLineSize, uint32_t _sysFreqMHz, uint32_t _domain, g_string& _name);
        virtual ~MemControllerBase();

        const char* getName() { return name.c_str(); }
        void enqueue(MemAccessEventBase* ev, uint64_t cycle);
        uint64_t access(MemReq& req);
        uint32_t tick(uint64_t sysCycle);
        void initStats(AggregateStat* parentStat);
        void updateStats(void);
        void finish(void);
};

// DRAM access event base class
class MemAccessEventBase : public TimingEvent {
    private:
        MemControllerBase* dram;
        MemAccessType type;
        Address addr;

    public:
        MemAccessEventBase(MemControllerBase* _dram, MemAccessType _type, Address _addr, int32_t domain, uint32_t preDelay, uint32_t postDelay)
            : TimingEvent(preDelay, postDelay, domain), dram(_dram), type(_type), addr(_addr) {}

        void simulate(uint64_t startCycle) { dram->enqueue(this, startCycle); }
        MemAccessType getType() const { return type; }
        Address getAddr() const { return addr; }
};

#endif
