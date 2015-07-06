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

#include "detailed_mem.h"
#include "zsim.h"
#include "tick_event.h"
#include <algorithm>

MemRankBase::MemRankBase(uint32_t _myId, uint32_t _parentId, uint32_t _bankCount) {
    myId = _myId;
    parentId = _parentId;
    bankCount = _bankCount;

    bankinfo = gm_calloc<bool>(bankCount);
    lastType = gm_calloc<MemAccessType>(bankCount);
    lastRow = gm_calloc<uint32_t>(bankCount);
    lastActCycle = gm_calloc<uint64_t>(bankCount);
    lastRdWrCycle = gm_calloc<uint64_t>(bankCount);
    lastPreCycle = gm_calloc<uint64_t>(bankCount);
    tFAWCycle = gm_calloc<uint64_t>(bankCount);

    lastBank = 0;
    lastAccessCycle = 0;
    lastRefreshCycle = 0;
    refreshNum = 0;
    accessInRefresh = 0;
    tFAWIndex = 0;

    activateCount = 0;
    prechargeCount = 0;
    readBurstCount = 0;
    writeBurstCount = 0;

    idlePowerDownCycle = 0;
    actvPowerDownCycle = 0;
    idleStandbyCycle = 0;

    prevIdlePowerDownCycle = 0;
    prevActvPowerDownCycle = 0;
    prevIdleStandbyCycle = 0;
}

MemRankBase::~MemRankBase() {
    gm_free(bankinfo);
    gm_free(lastRow);
    gm_free(lastType);
    gm_free(lastActCycle);
    gm_free(lastRdWrCycle);
    gm_free(lastPreCycle);
    gm_free(tFAWCycle);
}

void MemRankBase::access(uint64_t accessCycle, uint64_t issuedCycle, uint32_t row, uint32_t col, uint32_t bank, MemAccessType type) {
    // If the difference between read latency and write latency is large,
    // a latter access may overtake the prior one by the scheduling in intraIssueCycleble.
    // assert(lastAccessCycle < accessCycle);
    lastAccessCycle = std::max(lastAccessCycle, accessCycle);
    assert(lastRdWrCycle[bank] < issuedCycle);
    lastRdWrCycle[bank] = issuedCycle;
    lastRow[bank] = row;
    lastType[bank] = type;
    lastBank = bank;

    if (type == READ) {
        IncReadBurstCount();
    } else {
        IncWriteBurstCount();
    }
}

void MemRankBase::refresh(uint64_t lastCycle) {
    for (uint32_t i = 0; i < bankCount; i++) {
        bankinfo[i] = false;
    }
    assert(lastRefreshCycle < lastCycle);
    lastRefreshCycle = lastCycle;
}

uint32_t MemRankBase::GetActiveBankCount(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < bankCount; i++) {
        count += (bankinfo[i] == true)? 1 : 0;
    }
    return count;
}

void MemRankBase::SaveBackgroundCycles(void) {
    prevIdlePowerDownCycle = idlePowerDownCycle;
    prevActvPowerDownCycle = actvPowerDownCycle;
    prevIdleStandbyCycle = idleStandbyCycle;
}


MemChannelBase::MemChannelBase(uint32_t _myId, MemParam *_mParam) {
    myId = _myId;
    mParam = _mParam;
    accessLog.reserve(mParam->accessLogDepth);

    uint32_t rankCount = mParam->rankCount;
    ranks.resize(rankCount);
    for(uint32_t i = 0; i< rankCount; i++) {
        ranks[i] = new MemRankBase(i, myId, mParam->bankCount);
    }
}

MemChannelBase::~MemChannelBase(void) {
    for(uint32_t i = 0; i< mParam->rankCount; i++) {
        delete ranks[i];
    }
}

bool MemChannelBase::IsRowBufferHit(uint32_t row, uint32_t rank, uint32_t bank) {
    return ((ranks[rank]->GetBankOpen(bank) == true) && (ranks[rank]->GetLastRow(bank) == row));
}


uint32_t MemChannelBase::UpdateRefreshNum(uint32_t rank, uint64_t arrivalCycle) {
    //////////////////////////////////////////////////////////////////////
    // Auto Refresh Final Version ////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////
    //
    // ## tRPab = 0 in Close Policy
    //
    //           < - - - - - -   tREFI   - - - - -  >
    //        |  tRPab tRFC                           tRPab tRFC
    //-----------|---|-------|------------------------|---|-------|-----
    // *      *     (*)  (*)->*~~~~*           *
    // |      |~~~>* |    |   |    |           |
    // A1     A2   | B1   B2  B1   B2          C
    //             |
    //             accessCycle
    //           <->
    //            diff (refOverlap)
    //
    //
    // A:  Access (A2) comes before refresh and the last access (A1) is
    //     in the same refresh period
    // => No refresh penalty for A1 (normal access)
    // => if A2 latency overlaps on refresh period (tRPab+tRFC),
    //          all the access are shifted to the end of refresh (Case B)
    // => refOverlap is added to the head of B1 as additional constraint
    //          to get pseudo refresh shift by A2 latency
    // B:  2 or more access (B1, B2) come in refresh period
    // => B1 is shifted to the end of Refresh and B2 is shifted
    //          to the end of B1 access. Even if B2 is after refresh,
    //          B2 is shifted to the end of B1 access.
    // C:  Beyond tREFI from previous access
    // => Count refnum and multiply the time & power
    //
    //////////////////////////////////////////////////////////////////////
    uint64_t lastRefreshCycle = ranks[rank]->GetLastRefreshCycle();
    uint32_t refreshNum = 0;
    if (arrivalCycle >= lastRefreshCycle) {
        refreshNum = (arrivalCycle - lastRefreshCycle)/mParam->tREFI;
    }
    uint32_t totalNum = ranks[rank]->GetRefreshNum() + refreshNum;
    ranks[rank]->SetRefreshNum(totalNum);
    return refreshNum;
}

uint64_t MemChannelBase::UpdateLastRefreshCycle(uint32_t rank, uint64_t arrivalCycle, uint32_t refreshNum) {
    uint64_t lastRefreshCycle = ranks[rank]->GetLastRefreshCycle();
    if (refreshNum > 0) {
        // Updating Activate & Precharge count / rank from each bank
        for (uint32_t j = 0; j < mParam->bankCount; j++) {
            if (ranks[rank]->GetBankOpen(j) == true)
                ranks[rank]->IncPrechargeCount();
        }
        lastRefreshCycle += mParam->tREFI * refreshNum;
        ranks[rank]->refresh(lastRefreshCycle);// banks are closed
    }
    return lastRefreshCycle;
}

void MemChannelBase::UpdateDataBusCycle(uint64_t start, uint64_t end) {
    std::pair<uint64_t, uint64_t> tmpPair = std::make_pair(start, end);
    accessLog.push_back(tmpPair);
    sort(accessLog.begin(), accessLog.end());
    if (accessLog.size() > mParam->accessLogDepth) {
        accessLog.erase(accessLog.begin());
        accessLog[0].first = 0;
    }
}

uint64_t MemChannelBase::CalcIntraIssueCycle(bool rowHit, uint32_t rank, MemAccessType type, uint64_t arrivalCycle, uint32_t refreshNum) {
    uint32_t lastBank = ranks[rank]->GetLastBank();
    uint32_t lastType = ranks[rank]->GetLastType(lastBank);

    // Check last access Cycle is overlapped in ref period (tRPab + tRFC)
    uint32_t refCycle = mParam->GetRefreshCycle();
    uint64_t lastAccessCycle = ranks[rank]->GetLastAccessCycle();
    uint64_t lastRefreshCycle = ranks[rank]->GetLastRefreshCycle();
    uint64_t refOverlap = lastAccessCycle - lastRefreshCycle;
    if (refreshNum == 0) {
        // This is not the first access after refresh.
        ranks[rank]->SetAccessInRefresh(0);
    }
    if ((lastRefreshCycle != 0) &&
       (refCycle >= refOverlap) && (lastAccessCycle >= lastRefreshCycle) ) {
        //2nd access is during refresh
        uint32_t accessInRefresh = ranks[rank]->GetAccessInRefresh();
        ranks[rank]->SetAccessInRefresh(accessInRefresh+1);
    } else {
        ranks[rank]->SetAccessInRefresh(0);
    }

    // When Access comes during refresh period
    uint32_t accessInRefresh = ranks[rank]->GetAccessInRefresh();
    if (accessInRefresh != 0) {
        uint64_t issuableCycle = lastRefreshCycle + refCycle + refOverlap;
        issuableCycle = std::max(issuableCycle, arrivalCycle);
        if (accessInRefresh >= 2) { //2nd access in refresh
            issuableCycle += mParam->GetRdWrDelay(type, lastType);
        }
        return issuableCycle;
    }

    // Get constraint for same Rank and different Rank access
    uint64_t intraIssuableCycle = arrivalCycle;
    uint64_t lastIssuedCycle = ranks[rank]->GetLastRdWrCycle(lastBank);
    if (lastIssuedCycle != 0) {
        intraIssuableCycle = lastIssuedCycle;
        if (rowHit == true)
            intraIssuableCycle += mParam->GetRdWrDelay(type, lastType);
        else
            intraIssuableCycle += 1;// for command bus conflict
        intraIssuableCycle = std::max(intraIssuableCycle, arrivalCycle);
    }
    return intraIssuableCycle;
}

uint64_t MemChannelBase::CalcInterIssueCycle(MemAccessType type, uint64_t arrivalCycle) {
    // find out the slot
    uint32_t tWait = mParam->GetDataLatency(type);
    uint32_t tSlot = mParam->GetDataSlot(type) + mParam->tRTRS;
    uint64_t tStart = arrivalCycle + tWait;
    uint64_t tEnd = tStart + tSlot;
    for(uint32_t i = 0; i < accessLog.size(); i++) {
        uint64_t busStart = accessLog[i].first;
        uint64_t busEnd = accessLog[i].second + mParam->tRTRS;
        if (((busStart < tEnd) && (tEnd <= busEnd)) || ((busStart <= tStart) && (tStart < busEnd))) {
            tStart = busEnd;
            tEnd = tStart + tSlot;
        } else if (busStart > tEnd) {
            break;
        }
    }
    return tStart - tWait;
}

uint64_t MemChannelBase::CalcActConst(uint32_t rank, uint32_t bank, uint64_t issuableCycle) {
    uint64_t updateCycle = issuableCycle;
    if (ranks[rank]->GetLastActCycle(bank) == 0)
        return updateCycle;

    // tRC Constraint Check
    uint64_t currentBankActCycle = ranks[rank]->GetLastActCycle(bank);
    uint64_t tRC_const = currentBankActCycle + mParam->tRC;
    updateCycle = std::max(updateCycle, tRC_const);

    // tRP Constraint Check
    uint64_t lastPreCycle = ranks[rank]->GetLastPreCycle(bank);
    if (lastPreCycle != 0) {
        uint64_t tRP_const = lastPreCycle + mParam->tRP;
        updateCycle = std::max(updateCycle, tRP_const);
    }

    // tRRD Constraint Check
    uint64_t latestActCycle = 0;
    for(uint32_t i = 0; i < mParam->bankCount; i++) {
        uint64_t bankActCycle = ranks[rank]->GetLastActCycle(i);
        latestActCycle = std::max(latestActCycle, bankActCycle);
    }
    uint64_t tRRD_const = latestActCycle + mParam->tRRD;
    updateCycle = std::max(updateCycle, tRRD_const);

    // tFAW Constraint Check
    uint64_t tFAW_const = ranks[rank]->GetFAWCycle() + mParam->tFAW;
    updateCycle = std::max(updateCycle, tFAW_const);

    return updateCycle;
}

uint64_t MemChannelBase::CalcPreConst(uint32_t rank, uint32_t bank, MemAccessType type, uint64_t issuableCycle) {
    uint64_t updateCycle = issuableCycle;

    // read/write to precharge Constraint Check
    uint64_t lastRdWrCycle = ranks[rank]->GetLastRdWrCycle(bank);
    uint64_t tRW_const = lastRdWrCycle + mParam->GetPreDelay(type);
    updateCycle = std::max(updateCycle, tRW_const);

    // tRAS Constraint Check
    uint64_t lastActCycle = ranks[rank]->GetLastActCycle(bank);
    uint64_t tRAS_const = lastActCycle + mParam->tRAS;
    updateCycle = std::max(updateCycle, tRAS_const);

    return updateCycle;
}

uint64_t MemChannelBase::CalcRdWrConst(uint32_t rank, MemAccessType type, uint64_t issuableCycle) {
    uint64_t updateCycle = issuableCycle;
    uint32_t lastBank = ranks[rank]->GetLastBank();
    uint32_t lastType = ranks[rank]->GetLastType(lastBank);

    // read/write to read/write Constraint Check
    uint64_t lastIssuedCycle = ranks[rank]->GetLastRdWrCycle(lastBank);
    uint64_t rdwr_const = lastIssuedCycle + mParam->GetRdWrDelay(type, lastType);
    updateCycle = std::max(updateCycle, rdwr_const);

    return updateCycle;
}

void MemChannelBase::IssueActivate(uint32_t rank, uint32_t bank, uint64_t issuedCycle) {
    ranks[rank]->SetFAWCycle(issuedCycle);
    ranks[rank]->SetLastActCycle(bank, issuedCycle);
    ranks[rank]->SetBankOpen(bank);
    ranks[rank]->IncActivateCount();
}

void MemChannelBase::IssuePrecharge(uint32_t rank, uint32_t bank, uint64_t issuedCycle, bool continuous) {
    ranks[rank]->SetLastPreCycle(bank, issuedCycle);
    ranks[rank]->SetBankClose(bank);
    if (continuous == false)
        ranks[rank]->IncPrechargeCount();
}

uint64_t MemChannelBase::LatencySimulate(Address lineAddr, uint64_t arrivalCycle, uint64_t lastPhaseCycle, MemAccessType type) {
    uint32_t row, col, rank, bank;
    AddressMap(lineAddr, row, col, rank, bank);

    uint32_t refreshNum = UpdateRefreshNum(rank, arrivalCycle);
    // Require to call here. Between RefreshNum is updated, but LastRefreshNum is not Updated.
    uint32_t pd_penalty = GetPowerDownPenalty(rank, arrivalCycle);
    UpdatePowerDownCycle(rank, arrivalCycle, lastPhaseCycle, refreshNum);
    UpdateLastRefreshCycle(rank, arrivalCycle, refreshNum);

    // save rowBufferHit at this point
    bool rowHit = IsRowBufferHit(row, rank, bank);

    uint64_t preIssueCycle = (uint64_t)-1;
    uint64_t actIssueCycle = (uint64_t)-1;
    bool continuous = false;
    if (mParam->IsOpenRowBufPolicy()) {
        // rowbuffer hit -> intra constraint for read write command
        if (rowHit == false) {
            uint64_t issueCycle = CalcIntraIssueCycle(rowHit, rank, type,
                                                      arrivalCycle, refreshNum);
            if (ranks[rank]->GetBankOpen(bank) == true) {
                MemAccessType lastType = ranks[rank]->GetLastType(bank);
                preIssueCycle = CalcPreConst(rank, bank, lastType, issueCycle);
                assert(preIssueCycle >= issueCycle);
                IssuePrecharge(rank, bank, preIssueCycle);
                actIssueCycle = preIssueCycle + mParam->tRP;
            } else {
                // Issue only Activate after refresh
                actIssueCycle = issueCycle;
            }
        }
    } else { // Closed-row Policy
        assert(rowHit == false);
        continuous = CheckContinuousAccess(arrivalCycle, rank, bank, row);
        if (continuous == false) {
            actIssueCycle = CalcIntraIssueCycle(rowHit, rank, type,
                                                arrivalCycle, refreshNum);
        }
    }
    if (actIssueCycle != (uint64_t)-1) {
        actIssueCycle = CalcActConst(rank, bank, actIssueCycle);
        IssueActivate(rank, bank, actIssueCycle);
        assert(actIssueCycle >= arrivalCycle);
    }

    // Find Read Write command issue slot
    uint64_t rdwrStart = arrivalCycle;
    if (actIssueCycle == (uint64_t)-1) {
        // read/write to read/write constraint check
        if (continuous == true) {
            rdwrStart = CalcRdWrConst(rank, type, arrivalCycle);
        } else {
            // open page only
            assert(rowHit == true);
            assert(mParam->IsOpenRowBufPolicy());
            rdwrStart = CalcIntraIssueCycle(rowHit, rank, type,
                                            arrivalCycle, refreshNum);
        }
    } else {
        rdwrStart = actIssueCycle + mParam->tRCD;
        rdwrStart = CalcRdWrConst(rank, type, rdwrStart);
    }
    assert(rdwrStart >= arrivalCycle);
    uint64_t rdwrIssueCycle = CalcInterIssueCycle(type, rdwrStart);
    assert(rdwrIssueCycle >= arrivalCycle);
    uint64_t issueDelay = rdwrIssueCycle - arrivalCycle;
    uint32_t dataDelay = mParam->GetDataDelay(type);

    // total delay from the request arrival from CPU
    uint64_t latency = issueDelay + dataDelay + pd_penalty;
    uint64_t latency_mem = latency + (mParam->tTrans - mParam->tTransCrit);

    // Update Current Read/Write Command Information
    uint64_t accessCycle = arrivalCycle + latency_mem;
    ranks[rank]->access(accessCycle, rdwrIssueCycle, row, col, bank, type);

    // last, issue precharge in close policy
    if (mParam->IsCloseRowBufPolicy()) {
        // In close policy Precharge is issued in each access
        preIssueCycle = CalcPreConst(rank, bank, type, rdwrIssueCycle);
        assert(preIssueCycle >= rdwrIssueCycle);
        IssuePrecharge(rank, bank, preIssueCycle, continuous);
    }

    // save access cycle for inter constraint
    uint64_t busEndCycle = arrivalCycle + latency_mem;
    uint64_t busStartCycle = busEndCycle - mParam->GetDataSlot(type);
    UpdateDataBusCycle(busStartCycle, busEndCycle);

    return latency;
}

uint32_t MemChannelBase::GetPowerDownPenalty(uint32_t rank, uint64_t arrivalCycle) {
    uint32_t penalty = 0;
    if (mParam->powerDownCycle != 0) {
        uint64_t lastAccessCycle = ranks[rank]->GetLastAccessCycle();
        uint64_t lastPowerDownCycle = lastAccessCycle + mParam->powerDownCycle;
        if (arrivalCycle > lastPowerDownCycle) { // check if arrival cycle needs to be issuedCycle
            penalty = mParam->tXP;
        }
    }
    return penalty;
}

void MemChannelBase::UpdatePowerDownCycle(uint32_t rank, uint64_t arrivalCycle, uint64_t lastPhaseCycle, uint32_t refreshNum) {
    uint32_t powerDownCycle = mParam->powerDownCycle;
    if (powerDownCycle == 0)
        return;

    uint64_t lastAccessCycle = ranks[rank]->GetLastAccessCycle();
    uint64_t lastPowerDownCycle = lastAccessCycle + powerDownCycle;
    if (lastPowerDownCycle < lastPhaseCycle) {
        lastPowerDownCycle = lastPhaseCycle;
        powerDownCycle = 0;
    }

    uint32_t bankCount = mParam->bankCount;
    uint32_t actbanknum = ranks[rank]->GetActiveBankCount();
    uint32_t idlbanknum = bankCount - actbanknum;

    uint64_t idle_pd_cycle = ranks[rank]->GetIdlePowerDownCycle();
    uint64_t actv_pd_cycle = ranks[rank]->GetActvPowerDownCycle();
    uint64_t idle_sb_cycle = ranks[rank]->GetIdleStandbyCycle();

    if (lastAccessCycle == 0 && lastPhaseCycle == 0) {// This is a first Access for the rank
        idle_pd_cycle += arrivalCycle;
    } else if (arrivalCycle <= lastAccessCycle) {
        // add to actv_sb_cycle, so nothing to do here.
    } else if (arrivalCycle > lastAccessCycle && arrivalCycle <= lastPowerDownCycle) {
        uint64_t diffPowerDownCycle = arrivalCycle - lastAccessCycle;
        if (mParam->IsCloseRowBufPolicy()) {
            idle_sb_cycle += diffPowerDownCycle;
        } else {// Open Page Policy
            idle_sb_cycle += idlbanknum * diffPowerDownCycle / bankCount;
        }
    } else  {
        uint64_t powerDownDuration = arrivalCycle - lastPowerDownCycle;
        if (mParam->IsCloseRowBufPolicy()) {
            idle_pd_cycle += powerDownDuration;
            actv_pd_cycle += 0;
            idle_sb_cycle += powerDownCycle;

        } else {// Open Page Policy
            if (refreshNum == 0) {
                idle_pd_cycle += idlbanknum * powerDownDuration / bankCount;
                actv_pd_cycle += actbanknum * powerDownDuration / bankCount;
                idle_sb_cycle += idlbanknum * powerDownCycle    / bankCount;
            } else {
                uint32_t tREFI = mParam->tREFI;
                uint64_t lastRefreshCycle = ranks[rank]->GetLastRefreshCycle();
                uint64_t refreshEndCycle1 = (lastRefreshCycle + tREFI);
                uint64_t refreshEndCycle2 = (lastRefreshCycle + tREFI * refreshNum);

                assert_msg(arrivalCycle >= refreshEndCycle2,
                           "arrivalCycle(%ld) must be greater or equal than refreshEndCycle2(%ld)",
                           arrivalCycle, refreshEndCycle2);

                uint64_t diffArrivalCycle = arrivalCycle - refreshEndCycle2;
                uint64_t diffRefreshCycle = 0;
                if (refreshEndCycle1 > lastPowerDownCycle) {
                    diffRefreshCycle = refreshEndCycle1 - lastPowerDownCycle;
                }
                idle_pd_cycle += ((idlbanknum * diffRefreshCycle)
                                  + (((refreshNum - 1) * tREFI ) + diffArrivalCycle)) / bankCount;
                actv_pd_cycle += (actbanknum * diffRefreshCycle) / bankCount;
                idle_sb_cycle += idlbanknum * powerDownCycle / bankCount;
            }
        }
    }
    assert_msg(arrivalCycle >= (idle_pd_cycle + actv_pd_cycle + idle_sb_cycle),
               "PowerDown calc Error. arrival=%ld, idle_pd=%ld, actv_pd=%ld, idle_sb=%ld",
               arrivalCycle, idle_pd_cycle, actv_pd_cycle, idle_sb_cycle);
    ranks[rank]->SetIdlePowerDownCycle(idle_pd_cycle);
    ranks[rank]->SetActvPowerDownCycle(actv_pd_cycle);
    ranks[rank]->SetIdleStandbyCycle(idle_sb_cycle);
}

void MemChannelBase::PeriodicUpdatePower(uint64_t phaseCycle, uint64_t lastPhaseCycle) {
    for(uint32_t i = 0; i < mParam->rankCount; i++) {
        if (ranks[i]->GetLastAccessCycle() < phaseCycle) {
            uint32_t refreshNum = UpdateRefreshNum(i, phaseCycle);
            UpdatePowerDownCycle(i, phaseCycle, lastPhaseCycle, refreshNum);
            UpdateLastRefreshCycle(i, phaseCycle, refreshNum);
        }
    }
}

bool MemChannelBase::CheckContinuousAccess(uint64_t arrivalCycle, uint32_t rank, uint32_t bank, uint32_t row) {
     //////////////////////////////////////////////////////////////////////
     // Continuous Case in Close Policy ///////////////////////////////////
     //////////////////////////////////////////////////////////////////////
     //  # If next access comes before PRE, MEMC will not issue PRE and deal
     //  # it as continuous (Limited Open Policy = w/o Precharge) access
     //
     // 1.last access is Write
     //
     //        ACT   WRT                 PRE
     // last  --|-----|-------------------|--
     //          tRCD  tCWD  tTrans   tWR |
     //          <---> <--> ******** <--->|
     //                        < - - - - >|  continuousLatency
     //                      WRT           <---->PRE
     // current ------*-------|-------------------|--
     // (write)       |        tCWD  tTrans   tWR
     //               |        <--> ******** <--->
     //               |
     //              arrivalCycle
     //                             continuousLatency
     //                W->R const  <-------->
     //                 - - - - >RD         PRE
     // current ------*----------|-----------|---
     // (read)        |           tCAS  tTrans
     //               |           <--> ********
     //               |
     //              arrivalCycle
     //
     // 2.last access is Read
     //
     //        ACT   RD<-------->PRE
     // last  --|-----|----------|----------
     //          tRCD  tCAS  tTrans
     //          <---> <--> ********
     //
     //                R->W          continuousLatency
     //               - - - >WRT    <------->    PRE
     // current ------*-------|-------------------|--
     // (write)       |         tCWD  tTrans   tWR
     //               |        <---> ******** <--->
     //               |
     //              arrivalCycle
     //
     //                       RD        PRE
     // current ------*-------|----------|--
     // (read)        |        tCAS  tTrans
     //               |        <--> ********
     //               |             <------>
     //              arrivalCycle    continuousLatency
     //////////////////////////////////////////////////////////////////////
    if (mParam->mergeContinuous == false)
        return false;

     uint64_t lastPreCycle = ranks[rank]->GetLastPreCycle(bank);
     if ((arrivalCycle < lastPreCycle) &&
        (ranks[rank]->GetLastRow(bank) == row)) { // w/o ACT
         return true;
     } else {
         return false;
     }
}

// See also MemControllerBase::ReturnChannel
void MemChannelBase::AddressMap(Address addr, uint32_t& row, uint32_t& col, uint32_t& rank, uint32_t& bank) {
    // FIXME (dsm): This is needlessly complex. See how addressing is done in DDRMemory (along with sizing)
    //
    // Address is cache line address. it has already shifted for containg process id.
    // interleaveType == 0: | Row | ColH | Bank | Rank | Chnl | ColL | DataBus |
    // interleaveType == 1: | Row | ColH | Rank | Bank | Chnl | ColL | DataBus |
    // interleaveType == 2: | Row | Bank | ColH | Rank | Chnl | ColL | DataBus |
    // interleaveType == 3: | Row | Rank | ColH | Bank | Chnl | ColL | DataBus |
    // interleaveType == 4: | Row | Bank | Rank | ColH | Chnl | ColL | DataBus |
    // interleaveType == 5: | Row | Rank | Bank | ColH | Chnl | ColL | DataBus |
    // interleaveType == 6: | Row | Rank | Bank | Chnl | Column | DataBus |
    // interleaveType == 7: | Row | Rank | Chnl | Bank | Column | DataBus |
    // interleaveType == 8: | Row | Chnl | Rank | Bank | Column | DataBus |

    uint32_t colLowWidth = 0;
    uint32_t colLow = 0;
    if (mParam->channelDataWidthLog < mParam->byteOffsetWidth) {
        colLowWidth = mParam->byteOffsetWidth - mParam->channelDataWidthLog;
        colLow = addr & ((1L << colLowWidth) - 1);
        addr >>= colLowWidth;
    }

    uint32_t chnl = (uint32_t)-1;
    if (mParam->interleaveType >= 0 && mParam->interleaveType <= 5) {
        // for non-power of 2 channels
        chnl = addr % mParam->channelCount;
        addr /= mParam->channelCount;
    }

    uint32_t colHighWidth = mParam->colAddrWidth - colLowWidth;
    uint32_t colHigh = 0;
    if (mParam->interleaveType >= 4) {
        colHigh = addr & ((1L << colHighWidth) - 1);
        col = (colHigh << colLowWidth) | colLow;
        addr >>= colHighWidth;
    }

    switch(mParam->interleaveType) {
    case 0:
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        colHigh = addr & ((1L << colHighWidth) - 1);
        addr >>= colHighWidth;
        col = (colHigh << colLowWidth) | colLow;
        break;
    case 1:
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        colHigh = addr & ((1L << colHighWidth) - 1);
        addr >>= colHighWidth;
        col = (colHigh << colLowWidth) | colLow;
        break;
    case 2:
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        colHigh = addr & ((1L << colHighWidth) - 1);
        addr >>= colHighWidth;
        col = (colHigh << colLowWidth) | colLow;
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        break;
    case 3:
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        colHigh = addr & ((1L << colHighWidth) - 1);
        addr >>= colHighWidth;
        col = (colHigh << colLowWidth) | colLow;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        break;
    case 4:
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        break;
    case 5:
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        break;
    case 6:
        chnl = addr % mParam->channelCount;
        addr /= mParam->channelCount;
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        break;
    case 7:
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        chnl = addr % mParam->channelCount;
        addr /= mParam->channelCount;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        break;
    case 8:
        bank = addr & ((1L << mParam->bankWidth) - 1);
        addr >>= mParam->bankWidth;
        rank = addr & ((1L << mParam->rankWidth) - 1);
        addr >>= mParam->rankWidth;
        chnl = addr % mParam->channelCount;
        addr /= mParam->channelCount;
        break;
    }

    assert(myId == chnl);

    row = addr;
    //row != addr & ((1L<<mParam->rowAddrWidth)-1);
    // row address may contains large number, even if it exceed memory capacity size.
    // Becase memory model receives PID + VA as a access address.
    // But it's OK. row address is only used for checking row buffer hit,
    // and no address translation is almost same as ideotically address translation.
}

uint64_t MemChannelBase::GetActivateCount(void) {
    uint64_t actCount = 0;
    for (uint32_t i = 0; i < mParam->rankCount; i++) {
        actCount +=  ranks[i]->GetActivateCount();
    }
    return actCount;
}

uint64_t MemChannelBase::GetPrechargeCount(void) {
    uint64_t preCount = 0;
    for (uint32_t i = 0; i < mParam->rankCount; i++) {
        preCount += ranks[i]->GetPrechargeCount();
    }
    return preCount;
}

uint64_t MemChannelBase::GetRefreshCount(void) {
    uint64_t refnum = 0;
    for(uint32_t i = 0; i < mParam->rankCount; i++) {
        refnum += ranks[i]->GetRefreshNum();
    }
    return refnum;
}

uint64_t MemChannelBase::GetBurstEnergy(void) {
    uint64_t writeBurstCount = 0;
    uint64_t readBurstCount = 0;
    for(uint32_t i = 0; i < mParam->rankCount; i++) {
        writeBurstCount += ranks[i]->GetWriteBurstCount();
        readBurstCount  += ranks[i]->GetReadBurstCount();
    }

    uint64_t burstPower = 0;
    uint64_t burstPower1;
    assert_msg((mParam->IDD_VDD1.IDD4W >= mParam->IDD_VDD1.IDD3N), "IDD4W must be larger or equal than IDD3N");
    assert_msg((mParam->IDD_VDD1.IDD4R >= mParam->IDD_VDD1.IDD3N), "IDD4R must be larger or equal than IDD3N");
    burstPower1 = writeBurstCount * (mParam->IDD_VDD1.IDD4W - mParam->IDD_VDD1.IDD3N) * mParam->tTrans;
    burstPower1 += readBurstCount * (mParam->IDD_VDD1.IDD4R - mParam->IDD_VDD1.IDD3N) * mParam->tTrans;
    burstPower += burstPower1 * mParam->VDD1;
    burstPower *= mParam->chipCountPerRank;
    burstPower /= 1000; // uW -> mW
    return burstPower;
}

uint64_t MemChannelBase::GetActPreEnergy(void) {
    uint64_t actPreCount = GetActivateCount();
    uint64_t actPrePower = 0;
    uint64_t actPrePower1;
    assert_msg((mParam->tRC >= mParam->tRAS), "tRC must be larger or equal than tRAS");
    actPrePower1 = actPreCount * ( (mParam->IDD_VDD1.IDD0  * mParam->tRC)
                                   - ((mParam->IDD_VDD1.IDD3N * mParam->tRAS)
                                   + (mParam->IDD_VDD1.IDD2N * (mParam->tRC - mParam->tRAS))));
    actPrePower += actPrePower1 * mParam->VDD1;
    actPrePower *= mParam->chipCountPerRank;
    actPrePower /= 1000; // uW -> mW
    return actPrePower;
}

uint64_t MemChannelBase::GetRefreshEnergy(void) {
    uint64_t refnum = GetRefreshCount();
    uint64_t refreshPower = 0;
    uint64_t refreshPower1;
    assert_msg((mParam->IDD_VDD1.IDD5 >= mParam->IDD_VDD1.IDD3N), "IDD5 must be larger or equal than IDD3N");
    refreshPower1 = refnum * (mParam->IDD_VDD1.IDD5 - mParam->IDD_VDD1.IDD3N) * mParam->tRFC;
    refreshPower += refreshPower1 * mParam->VDD1;

    refreshPower *= mParam->chipCountPerRank;
    refreshPower /= 1000; // uW -> mW
    return refreshPower;
}

uint64_t MemChannelBase::GetBackGroundEnergy(uint64_t memCycle, uint64_t lastMemCycle, bool bInstant) {
    assert(lastMemCycle < memCycle);
    uint64_t tickCycle = bInstant ? (memCycle - lastMemCycle) : memCycle;

    uint64_t backgroundPower = 0;
    for(uint32_t i = 0; i < mParam->rankCount; i++) {
        uint64_t lastAccessCycle = ranks[i]->GetLastAccessCycle();
        uint64_t idlePowerDownCycle;
        uint64_t actvPowerDownCycle;
        uint64_t idleStandbyCycle;
        if (mParam->powerDownCycle == 0) {
            idlePowerDownCycle = 0;
            actvPowerDownCycle = 0;
            idleStandbyCycle= 0;
        } else if (bInstant == false) {
            idlePowerDownCycle = ranks[i]->GetIdlePowerDownCycle();
            actvPowerDownCycle = ranks[i]->GetActvPowerDownCycle();
            idleStandbyCycle = ranks[i]->GetIdleStandbyCycle();
        } else {
            if (lastAccessCycle < lastMemCycle) {// No Access
                idlePowerDownCycle = tickCycle;
                actvPowerDownCycle = 0;
                idleStandbyCycle = 0;
            } else {
                idlePowerDownCycle = ranks[i]->GetIdlePowerDownCycle();
                idlePowerDownCycle -= ranks[i]->GetPrevIdlePowerDownCycle();
                actvPowerDownCycle = ranks[i]->GetActvPowerDownCycle();
                actvPowerDownCycle -= ranks[i]->GetPrevActvPowerDownCycle();
                idleStandbyCycle = ranks[i]->GetIdleStandbyCycle();
                idleStandbyCycle   -= ranks[i]->GetPrevIdleStandbyCycle();
            }
            ranks[i]->SaveBackgroundCycles();
        }
        uint64_t actvStandbyCycle = tickCycle - idlePowerDownCycle - actvPowerDownCycle - idleStandbyCycle;
        assert_msg(tickCycle >= (idlePowerDownCycle + actvPowerDownCycle + idleStandbyCycle),
                   "Power down cycle calculation error. bInstant = %d, memCycle=%ld, idlePowerDown=%ld, actvPowerDown=%ld, idleStandby=%ld",
                   bInstant, tickCycle, idlePowerDownCycle, actvPowerDownCycle, idleStandbyCycle);
        uint64_t idlePowerDown = mParam->VDD1 * (idlePowerDownCycle * mParam->IDD_VDD1.IDD2P) / tickCycle;
        uint64_t actPowerDown = mParam->VDD1 * (actvPowerDownCycle * mParam->IDD_VDD1.IDD3P) / tickCycle;
        uint64_t idleStandby = mParam->VDD1 * (idleStandbyCycle   * mParam->IDD_VDD1.IDD2N) / tickCycle;
        uint64_t actvStandby = mParam->VDD1 * (actvStandbyCycle   * mParam->IDD_VDD1.IDD3N) / tickCycle;
        backgroundPower += (idlePowerDown + actPowerDown + idleStandby + actvStandby);
    }
    backgroundPower *= mParam->chipCountPerRank;
    backgroundPower /= 1000;// uW -> mW
    return backgroundPower;
}


////////////////////////////////////////////////////////////////////////
// Default Memory Scheduler Class
MemSchedulerDefault::MemSchedulerDefault(uint32_t id, MemParam* mParam, MemChannelBase* mChnl)
    : MemSchedulerBase(id, mParam, mChnl)
{
    prioritizedAccessType = READ;
    wrQueueSize = mParam->schedulerQueueCount;
    wrQueueHighWatermark = mParam->schedulerQueueCount * 2 / 3;
    wrQueueLowWatermark = mParam->schedulerQueueCount * 1 / 3;
}

MemSchedulerDefault::~MemSchedulerDefault() {}

bool MemSchedulerDefault::CheckSetEvent(MemAccessEventBase* ev) {
    // Write Queue Hit Check
    g_vector<MemSchedQueueElem>::iterator it;
    for(it = wrQueue.begin(); it != wrQueue.end(); it++) {
        if (it->second == ev->getAddr()) {
            if (ev->getType() == WRITE) {
                wrQueue.erase(it);
                wrQueue.push_back(MemSchedQueueElem(nullptr, ev->getAddr()));
            }
            return true;
        }
    }

    // Write Done Queue Hit Check
    for(it = wrDoneQueue.begin(); it != wrDoneQueue.end(); it++) {
        if (it->second == ev->getAddr()) {
            if (ev->getType() == READ) {
                // Update LRU
                wrDoneQueue.erase(it);
                wrDoneQueue.push_back(MemSchedQueueElem(nullptr, ev->getAddr()));
            } else { // Write
                // Update for New Data
                wrDoneQueue.erase(it);
                wrQueue.push_back(MemSchedQueueElem(nullptr, ev->getAddr()));
            }
            return true;
        }
    }

    // No Hit
    if (ev->getType() == READ) {
        rdQueue.push_back(MemSchedQueueElem(ev, ev->getAddr()));
    } else { // Write
        wrQueue.push_back(MemSchedQueueElem(nullptr, ev->getAddr()));
        if (wrQueue.size() + wrDoneQueue.size() == wrQueueSize) {
            // Overflow case
            if (wrDoneQueue.empty() == false) {
                wrDoneQueue.erase(wrDoneQueue.begin());
            } else {
                // FIXME: Need to handle this - HK
                warn("Write Buffer Overflow!!");
            }
        }
    }
    return false;
}

bool MemSchedulerDefault::GetEvent(MemAccessEventBase*& ev, Address& addr, MemAccessType& type) {
    bool bRet = false;

    // Check Priority
    if (wrQueue.size() >= wrQueueHighWatermark)
        prioritizedAccessType = WRITE; // Write Priority
    else if (wrQueue.size() <= wrQueueLowWatermark)
        prioritizedAccessType = READ; // Read Priority

    //info("Id%d: Read Queue = %ld, Write Queue = %ld, Schedule = %d",
    //myId, rdQueue.size(), wrQueue.size(), prioritizedAccessType);

    uint32_t idx;
    g_vector<MemSchedQueueElem>::iterator it;
    if (prioritizedAccessType == READ) {
        bRet = FindBestRequest(&rdQueue, idx);
        if (bRet) {
            it = rdQueue.begin() + idx;
            ev = it->first;
            addr = ev->getAddr();
            type = ev->getType();
            rdQueue.erase(it);
        }
    }

    if (!bRet) { // Write Priority or No Read Entry
        bRet = FindBestRequest(&wrQueue, idx);
        if (bRet) {
            it = wrQueue.begin() + idx;
            ev = nullptr;
            addr = it->second;
            type = WRITE;
            wrQueue.erase(it);
            wrDoneQueue.push_back(MemSchedQueueElem(nullptr, addr));
        }
    }

    return bRet;
}

bool MemSchedulerDefault::FindBestRequest(g_vector<MemSchedQueueElem> *queue, uint32_t& idx) {
    idx = 0;
    uint32_t tmpIdx = 0;
    g_vector<MemSchedQueueElem>::iterator it;
    for(it = queue->begin(); it!= queue->end(); it++) {
        Address addr = it->second;
        uint32_t row, col, rank, bank;
        mChnl->AddressMap(addr, row, col, rank, bank);
        if (mChnl->IsRowBufferHit(row, rank, bank) == true) {
            idx = tmpIdx;
            break;
        }
        tmpIdx++;
    }

    return !queue->empty();
}


// Main Memory Class
MemControllerBase::MemControllerBase(g_string _memCfg, uint32_t _cacheLineSize, uint32_t _sysFreqMHz, uint32_t _domain, g_string& _name) {
    name = _name;
    domain = _domain;
    info("%s: domain %d", name.c_str(), domain);

    lastPhaseCycle = 0;
    lastAccessedCycle = 0;
    cacheLineSize = _cacheLineSize;

    futex_init(&updateLock);

    mParam = new MemParam();
    mParam->LoadConfig(_memCfg, _cacheLineSize);

    // Calculate Frequency
    sysFreqKHz = _sysFreqMHz * 1000;
    memFreqKHz = 1e9 / mParam->tCK / 1e3;
    info("MemControllerBase: sysFreq = %ld KHz memFreq = %ld KHz", sysFreqKHz, memFreqKHz);

    if (mParam->schedulerQueueCount != 0) {
        //Processor tick, memory ticks only every Nth cycle where N is proc:mem freq ratio
        // for Memory Scheduler
        nextSysTick = std::max((uint64_t)1, memToSysCycle(1));
    } else {
        // for periodic performance report
        // for avoiding tick scheduler limitation
        nextSysTick = usecToSysCycle(10);// once every 10us
    }
    reportPeriodCycle = usecToSysCycle(mParam->reportPhase);

    // setup controller parameters
    memMinLatency[0] = memToSysCycle(mParam->GetDataLatency(0));// Read
    memMinLatency[1] = memToSysCycle(mParam->GetDataLatency(1));// Write
    if (mParam->schedulerQueueCount == 0) {
        minLatency[0] = mParam->GetDataLatency(0);// Read
        minLatency[1] = mParam->GetDataLatency(1);// Write
    } else {
        minLatency[0] = 1;// scheduler queue hit case
        minLatency[1] = 1;// scheduler queue hit case
    }
    minLatency[0] = memToSysCycle(minLatency[0]) + mParam->controllerLatency;
    minLatency[1] = memToSysCycle(minLatency[1]) + mParam->controllerLatency;
    preDelay[0] = minLatency[0] / 2;
    preDelay[1] = minLatency[1] / 2;
    postDelay[0] = minLatency[0] - preDelay[0];
    postDelay[1] = minLatency[1] - preDelay[1];
    info("Latency: read minLatency is %d, write minLatency is %d", minLatency[0], minLatency[1]);

    memset(&lastPower, 0, sizeof(powerValue));
    lastAccesses = 0;
    maxBandwidth = 0;
    minBandwidth = (uint64_t)-1;

    chnls.resize(mParam->channelCount);
    sches.resize(mParam->channelCount);
    for(uint32_t i = 0; i < mParam->channelCount; i++) {
        chnls[i] = new MemChannelBase (i, mParam);
        sches[i] = new MemSchedulerDefault(i, mParam, chnls[i]);
    }

    if (mParam->schedulerQueueCount != 0) {
        TickEvent<MemControllerBase >* tickEv = new TickEvent<MemControllerBase >(this, domain);
        tickEv->queue(0); //start the sim at time 0
        info("MemControllerBase::tick() will be call in each %ld sysCycle", nextSysTick);
    }

    addrTraceLog = nullptr;
    if (mParam->addrTrace == true) {
        g_string gzFileName = g_string("ZsimMemAddrTrace_") + name.c_str() + ".gz";
        addrTraceLog = gzopen(gzFileName.c_str(), "wb1");
        if (addrTraceLog == nullptr)
            panic("Fail to open file %s for addrTraceLog.", gzFileName.c_str());
    }
}

MemControllerBase::~MemControllerBase() {
    if (mParam != nullptr) {
        for(uint32_t i = 0; i < mParam->channelCount; i++) {
            delete chnls[i];
            delete sches[i];
        }
        delete mParam;
    }
}

void MemControllerBase::enqueue(MemAccessEventBase* ev, uint64_t cycle) {
    if (mParam->schedulerQueueCount == 0) {
        MemAccessType type = ev->getType();
        uint64_t startCycle = cycle - preDelay[type] + mParam->controllerLatency;
        // FIXME: Shouldn't we use the next memCycle following startCycle as the
        // starting cycle of the dram request?
        uint64_t latency = LatencySimulate(ev->getAddr(), startCycle, type);
        ev->done(cycle + latency - minLatency[type] + mParam->controllerLatency);
        return;
    }

    // Write Queue Hit Check
    uint32_t channel = ReturnChannel(ev->getAddr());
    bool bRet = sches[channel]->CheckSetEvent(ev);
    if (ev->getType() == READ) {
        if (bRet)
            ev->done(cycle - minLatency[0] + mParam->controllerLatency);
        else
            ev->hold();
    } else { // Write
        // Write must be enqueued.
        ev->done(cycle - minLatency[1] + mParam->controllerLatency);
    }

    return;
}

uint32_t MemControllerBase::tick(uint64_t sysCycle) {
    // tick will be called each memCycle
    // for memory scheduler
    if (mParam->schedulerQueueCount != 0) {
        TickScheduler(sysCycle);
    }

    return nextSysTick;
}

void MemControllerBase::TickScheduler(uint64_t sysCycle) {
    for(uint32_t i = 0; i < mParam->channelCount; i++) {
        MemAccessEventBase* ev = nullptr;
        Address  addr = 0;
        MemAccessType type = READ;
        bool bRet = sches[i]->GetEvent(ev, addr, type);
        if (bRet) {
            uint64_t latency = LatencySimulate(addr, sysCycle, type);
            if (type == READ) {
                // Write has already ev->done
                ev->release();
                ev->done(sysCycle - minLatency[0] + latency);
            }
        }
    }
}

uint64_t MemControllerBase::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = E;
            break;
        case GETX:
            *req.state = M;
            break;

        default: panic("!?");
    }

    if (req.type == PUTS)
        return req.cycle;

    MemAccessType accessType = (req.type == PUTS || req.type == PUTX) ? WRITE : READ;
    uint64_t respCycle = req.cycle + minLatency[accessType];
    assert(respCycle >= req.cycle);

    if ((req.type != PUTS) && zinfo->eventRecorders[req.srcId]) {
        Address addr = req.lineAddr;
        MemAccessEventBase* memEv =
            new (zinfo->eventRecorders[req.srcId])
            MemAccessEventBase(this, accessType, addr, domain, preDelay[accessType], postDelay[accessType]);
        memEv->setMinStartCycle(req.cycle);
        TimingRecord tr = {addr, req.cycle, respCycle, req.type, memEv, memEv};
        zinfo->eventRecorders[req.srcId]->pushRecord(tr);
    }
    return respCycle;
}

void MemControllerBase::initStats(AggregateStat* parentStat) {
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Memory controller stats");

    profActivate.init("act", "Activate command Times");
    memStats->append(&profActivate);
    profReads.init("rd", "Read request command Times");
    memStats->append(&profReads);
    profWrites.init("wr", "Write request command Times");
    memStats->append(&profWrites);
    profPrecharge.init("pre", "Precharge command Times");
    memStats->append(&profPrecharge);
    profRefresh.init("ref", "Refresh command Times");
    memStats->append(&profRefresh);

    if (mParam->accAvgPowerReport == true) {
        AggregateStat* apStats = new AggregateStat();
        apStats->init("ap", "Cumulative Average Power Report");
        profAccAvgPower[0].init("total",  "Total average power (mW)");
        profAccAvgPower[1].init("actpre", "ActPre average power (mW)");
        profAccAvgPower[2].init("burst",  "Burst average power (mW)");
        profAccAvgPower[3].init("refr",   "Refersh average power (mW)");
        profAccAvgPower[4].init("bgnd",   "Background average power (mW)");
        profAccAvgPower[5].init("dq",     "DQ average power (mW)");
        profAccAvgPower[6].init("term",   "Terminate average power (mW)");
        for(uint32_t i = 0; i < pwCounterNum; i++)
            apStats->append(&profAccAvgPower[i]);
        memStats->append(apStats);
    }

    if (mParam->curAvgPowerReport == true) {
        AggregateStat* cpStats = new AggregateStat();
        cpStats->init("cp", "Current Average Power Report");
        profCurAvgPower[0].init("total",  "Total instant power (mW)");
        profCurAvgPower[1].init("actpre", "ActPre instant power (mW)");
        profCurAvgPower[2].init("burst",  "Burst instant power (mW)");
        profCurAvgPower[3].init("refr",   "Refersh instant power (mW)");
        profCurAvgPower[4].init("bgnd",   "Background instant power (mW)");
        profCurAvgPower[5].init("dq",     "DQ instant power (mW)");
        profCurAvgPower[6].init("term",   "Terminate instant power (mW)");
        for(uint32_t i = 0; i < pwCounterNum; i++)
            cpStats->append(&profCurAvgPower[i]);
        memStats->append(cpStats);
    }

    if (mParam->bandwidthReport == true) {
        AggregateStat* bwStats = new AggregateStat();
        bwStats->init("bw", "Bandwidth Report");
        profBandwidth[0].init("all", "Cumulative Average bandwidth (MB/s)");
        profBandwidth[1].init("cur", "Current Average bandwidth (MB/s)");
        profBandwidth[2].init("max", "Maximum bandwidth (MB/s)");
        profBandwidth[3].init("min", "Minimum bandwidth (MB/s)");
        for(uint32_t i = 0; i < bwCounterNum; i++)
            bwStats->append(&profBandwidth[i]);
        memStats->append(bwStats);
    }

    profTotalRdLat.init("rdlat", "Total latency experienced by read requests");
    memStats->append(&profTotalRdLat);
    profTotalWrLat.init("wrlat", "Total latency experienced by write requests");
    memStats->append(&profTotalWrLat);

    lhBinSize = 10;
    lhNumBins = 200;
    latencyHist.init("mlh","latency histogram for memory requests", lhNumBins);
    memStats->append(&latencyHist);

    parentStat->append(memStats);
}

void MemControllerBase::updateStats(void) {
    uint64_t sysCycle = zinfo->globPhaseCycles;
    uint64_t realTime = sysToMicroSec(sysCycle);
    uint64_t lastRealTime = sysToMicroSec(lastPhaseCycle);
    if (mParam->accAvgPowerReport == true || mParam->curAvgPowerReport == true)
        EstimatePowers(sysCycle);
    if (mParam->bandwidthReport == true)
        EstimateBandwidth(realTime, lastRealTime);
    UpdateCmdCounters();
    lastPhaseCycle = sysCycle;
}

void MemControllerBase::finish(void) {
    // This function will be called at the last process termination.
    uint64_t minCycle = usecToSysCycle(1);
    uint64_t endCycle = std::max(zinfo->globPhaseCycles, minCycle);
    uint64_t realTime = sysToMicroSec(endCycle);
    uint64_t lastRealTime = sysToMicroSec(lastPhaseCycle);

    if (mParam->anyReport == true)
        info("=== %s: Final Performance Report @ %ld usec (duration is %ld usec) ===",
             name.c_str(), realTime, realTime - lastRealTime);
    EstimatePowers(endCycle, true);
    EstimateBandwidth(realTime, lastRealTime, true);
    UpdateCmdCounters();

    if (addrTraceLog != nullptr)
        gzclose(addrTraceLog);
}

// See also MemChannelBase::AddressMap
uint64_t MemControllerBase::ReturnChannel(Address addr) {

    // addr is cache line address. it has already shifted for containg process id.

    uint32_t colLowWidth = 0;
    if (mParam->channelDataWidthLog < mParam->byteOffsetWidth) {
        colLowWidth = mParam->byteOffsetWidth - mParam->channelDataWidthLog;
        addr >>= colLowWidth;
    }

    uint64_t result = addr;

    //for non-power of 2 channels, simply shift and get modulo
    switch (mParam->interleaveType) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            // Cache block interleave
            result  %= mParam->channelCount;
            break;
        case 6:
            result >>= (mParam->colAddrWidth - colLowWidth);
            result  %= mParam->channelCount;
            break;
        case 7:
            result >>= (mParam->colAddrWidth - colLowWidth);
            result >>= mParam->bankWidth;
            result  %= mParam->channelCount;
            break;
        case 8:
            result >>= (mParam->colAddrWidth - colLowWidth);
            result >>= mParam->bankWidth;
            result >>= mParam->rankWidth;
            result  %= mParam->channelCount;
            break;
        default:
            panic("Invalid interleaveType!");
    }
    return result;
}

uint64_t MemControllerBase::LatencySimulate(Address lineAddr, uint64_t sysCycle, MemAccessType type) {
    uint32_t channel = ReturnChannel(lineAddr);
    uint64_t memCycle = sysToMemCycle(sysCycle);
    uint64_t lastMemCycle = sysToMemCycle(lastPhaseCycle);
    uint64_t memLatency = chnls[channel]->LatencySimulate(lineAddr, memCycle, lastMemCycle, type);
    uint64_t sysLatency = memToSysCycle(memLatency);
    assert_msg(sysLatency  >= (memMinLatency[type]),
               "Memory Model returned lower latency than memMinLatency! latency = %ld, memMinLatency = %d",
               sysLatency, memMinLatency[type]);
    uint32_t bin = std::min(sysLatency/lhBinSize, (uint64_t)(lhNumBins-1));
    latencyHist.inc(bin);

    if (addrTraceLog != nullptr)
        gzwrite(addrTraceLog, (char*)&lineAddr, sizeof(uint64_t));

    if (type == WRITE) {
        profWrites.atomicInc();
        profTotalWrLat.atomicInc(sysLatency);
    } else { // READ
        profReads.atomicInc();
        profTotalRdLat.atomicInc(sysLatency);
    }

    lastAccessedCycle = sysCycle;

    return sysLatency;
}

void MemControllerBase::UpdateCmdCounters(void) {
    uint64_t activateCnt = 0;
    uint64_t prechargeCnt = 0;
    uint64_t refreshCnt = 0;
    for(uint32_t i = 0; i < mParam->channelCount; i++) {
        activateCnt   += chnls[i]->GetActivateCount();
        prechargeCnt  += chnls[i]->GetPrechargeCount();
        refreshCnt    += chnls[i]->GetRefreshCount();
    }
    profActivate.set(activateCnt);
    profPrecharge.set(prechargeCnt);
    profRefresh.set(refreshCnt);
}

void MemControllerBase::EstimatePowers(uint64_t sysCycle, bool finish) {
    uint64_t memCycle = sysToMemCycle(sysCycle);
    uint64_t lastMemCycle = sysToMemCycle(lastPhaseCycle);
    uint64_t instCycle = memCycle - lastMemCycle;
    assert(memCycle > lastMemCycle);

    // 1/10V * 1/100mA = uW / 1000 = mW
    // dq & terminate : uW
    powerValue accPower;
    memset(&accPower, 0, sizeof(powerValue));
    powerValue curPower;
    memset(&curPower, 0, sizeof(powerValue));
    for(uint32_t i = 0; i < mParam->channelCount; i++) {
        chnls[i]->PeriodicUpdatePower(memCycle, lastMemCycle);

        accPower.actPre     += chnls[i]->GetActPreEnergy();
        accPower.burst      += chnls[i]->GetBurstEnergy();
        accPower.refresh    += chnls[i]->GetRefreshEnergy();
        accPower.background += chnls[i]->GetBackGroundEnergy(memCycle, lastMemCycle, false);
        if (mParam->curAvgPowerReport == true)
            curPower.background += chnls[i]->GetBackGroundEnergy(memCycle, lastMemCycle, true);
    }

    uint64_t avgRdActivity = profReads.get()  * mParam->tTrans;
    uint64_t avgWrActivity = profWrites.get() * mParam->tTrans;
    // readDq, writeDq: uW, DQ power in current accessed rank, calculate from Whole Chip full usage power
    accPower.dq = ((avgRdActivity * mParam->readDqPin) + (avgWrActivity * mParam->writeDqPin)) * mParam->chipCountPerRank;
    // readTerm, writeTerm: uW, terminate power in the other ranks, calculate from Whole Chip full usage power
    accPower.terminate = ((avgRdActivity * mParam->readTermPin) + (avgWrActivity * mParam->writeTermPin)) * mParam->chipCountPerRank;
    accPower.terminate *= (mParam->rankCount - 1);

    if (mParam->curAvgPowerReport == true) {
        // compute instant power
        // Regarding memory which has VDDQ domain like LPDDRx, VDDQ power is added to dq power.
        curPower.actPre = (accPower.actPre - lastPower.actPre) / instCycle;
        curPower.burst = (accPower.burst - lastPower.burst) / instCycle;
        curPower.refresh = (accPower.refresh - lastPower.refresh) / instCycle;
        curPower.dq = CalcDQTermCur(accPower.dq, lastPower.dq, instCycle, memCycle, lastMemCycle);
        curPower.terminate = (accPower.terminate - lastPower.terminate) / instCycle / 1000;
        curPower.total = curPower.burst + curPower.actPre + curPower.refresh + curPower.background + curPower.dq + curPower.terminate;

        // assertion
        assert_msg((accPower.burst >= lastPower.burst), "Burst power calculation problem.");
        assert_msg((accPower.actPre >= lastPower.actPre), "ActPre power calculation problem.");
        assert_msg((accPower.refresh >= lastPower.refresh), "Refresh power calculation problem.");
        assert_msg((accPower.dq >= lastPower.dq), "DQ power calculation problem.");
        assert_msg((accPower.terminate >= lastPower.terminate), "Terminate power calculation problem.");

        // profile update
        for(uint32_t i =0; i < pwCounterNum; i++)
            profCurAvgPower[i].set(*(&curPower.total + i));

        // backup for next compute
        lastPower = accPower;
    }

    if (mParam->accAvgPowerReport == true) {
        // compute average power
        // Regarding memory which has VDDQ domain like LPDDRx, VDDQ power is added to dq power.
        accPower.actPre = accPower.actPre / memCycle;
        accPower.burst = accPower.burst  / memCycle;
        accPower.refresh = accPower.refresh / memCycle;
        //accPower.background =accPower.background;
        accPower.dq = CalcDQTermAcc(accPower.dq, memCycle, lastMemCycle);
        accPower.terminate = accPower.terminate / memCycle / 1000;
        accPower.total = accPower.burst + accPower.actPre + accPower.refresh + accPower.background + accPower.dq + accPower.terminate;
        // profile update
        for(uint32_t i =0; i < pwCounterNum; i++)
            profAccAvgPower[i].set(*(&accPower.total + i));
    }

    if (mParam->accAvgPowerReport == true && finish == true) {
        info("Cummulative Average Power(mW): Total= %6ld, ActPre= %6ld, Burst= %6ld, Refresh= %6ld, BackGrnd= %6ld, ModuleDQ= %6ld, Terminate= %6ld",
             accPower.total, accPower.actPre, accPower.burst, accPower.refresh,
             accPower.background, accPower.dq, accPower.terminate);
    }
    //info("Current Average Power(mW): Total= %6ld, ActPre= %6ld, Burst= %6ld, Refresh= %6ld, BackGrnd= %6ld, ModuleDQ= %6ld, Terminate= %6ld",
    //curPower.total, curPower.actPre, curPower.burst, curPower.refresh,
    //curPower.background, curPower.dq, curPower.terminate);
}

uint64_t MemControllerBase::CalcDQTermCur(uint64_t acc_dq, uint64_t last_dq, uint64_t instCycle, uint64_t memCycle, uint64_t lastMemCycle) {
    // memCycle and lastMemCycle are used in LPDDRx mode
    return (acc_dq - last_dq) / instCycle / 1000;
};

uint64_t MemControllerBase::CalcDQTermAcc(uint64_t acc_dq, uint64_t memCycle, uint64_t lastMemCycle) {
    // memCycle and lastMemCycle are used in LPDDRx mode
    return acc_dq / memCycle / 1000;
};

void MemControllerBase::EstimateBandwidth(uint64_t realTime, uint64_t lastTime, bool finish) {
    // Access Count
    assert(realTime > lastTime);
    uint64_t totalAccesses = profReads.get() + profWrites.get();
    uint64_t avgBandwidth = (totalAccesses * cacheLineSize) / realTime;
    uint64_t curBandwidth = (totalAccesses - lastAccesses) * cacheLineSize / (realTime - lastTime);
    maxBandwidth = std::max(maxBandwidth, curBandwidth);
    minBandwidth = std::min(minBandwidth, curBandwidth);

    // Profile Update
    profBandwidth[0].set(avgBandwidth);
    profBandwidth[1].set(curBandwidth);
    profBandwidth[2].set(maxBandwidth);
    profBandwidth[3].set(minBandwidth);

    lastAccesses = totalAccesses;

    if (mParam->bandwidthReport == true && finish == true) {
        info("BandWidth (MB/s): CumulativeAvg= %ld, Current= %ld, Max= %ld, Min= %ld",
             avgBandwidth, curBandwidth, maxBandwidth, minBandwidth);
    }
}

