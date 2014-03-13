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

#include "detailed_mem_params.h"
#include <math.h>
#include <string.h>
#include "bithacks.h"

// FIXME(dsm): Here be dragons. I don't know why this uses a separate cfg file to begin with, it makes runs much harder to script.

MemParam::MemParam()
{
    rowBufferPolicy = RB_CLOSE;
    memset(constraints,  0, sizeof(uint32_t) * 32);
}

MemParam::~MemParam()
{
}

bool MemParam::IsOpenRowBufPolicy() {
    return (rowBufferPolicy == RB_OPEN);
}

bool MemParam::IsCloseRowBufPolicy() {
    return (rowBufferPolicy == RB_CLOSE);
}

void MemParam::LoadConfig(g_string _cfgFile, uint32_t _cacheLineSize)
{
    info("Loading Memory Config from %s", _cfgFile.c_str());
    Config cfg(_cfgFile.c_str());
    LoadConfigMain(cfg, _cacheLineSize);
    LoadTiming(cfg);
    LoadPower(cfg);
    // Make Constraints from Timing Paramteres
    MakeConstraints();
}

void MemParam::LoadConfigMain(Config &cfg, uint32_t _cacheLineSize)
{
    // loading Simulation paramters
    reportPhase = cfg.get<uint32_t>("sim.reportPhase", 10000);
    if(reportPhase == 0) {
        warn("!!! Please set non-0 value to sim.reportPhase.");
        assert(false);
    }
    reportStart = (uint64_t)cfg.get<uint32_t>("sim.reportStart", 0);
    reportFinish = (uint64_t)cfg.get<uint32_t>("sim.reportFinish", 0);
    if(reportFinish == 0)
        reportFinish = (uint64_t)-1;
    accAvgPowerReport = cfg.get<bool>("sim.accAvgPowerReport", false);
    curAvgPowerReport = cfg.get<bool>("sim.curAvgPowerReport", false);
    bandwidthReport = cfg.get<bool>("sim.bandwidthReport", false);
    anyReport = (accAvgPowerReport | curAvgPowerReport | bandwidthReport);
    info("AccAvgPower=%d, CurAvgPower=%d, BandWidth=%d will be reported.",
         accAvgPowerReport, curAvgPowerReport, bandwidthReport);
    info("Reports are in each %d phase, Start phase cycle=%ld, Finish phase cycle=%ld",
         reportPhase, reportStart, reportFinish);

    addrTrace = cfg.get<bool>("sim.addressTrace", false);
    if(addrTrace == true) {
        info("Address Traces are output to file");
    }

    // loading Memory Controller parameters
    totalCapacity = cfg.get<uint32_t>("mc_spec.capacityMB", 4096);
    channelCount = cfg.get<uint32_t>("mc_spec.channels", 2);
    channelDataWidth = cfg.get<uint32_t>("mc_spec.channelDataWidth", 64);
    g_string _rowBufferPolicy = cfg.get<const char*>("mc_spec.rowBufferPolicy", "close");
    rowBufferPolicy = (_rowBufferPolicy == "open") ? RB_OPEN : RB_CLOSE;
    interleaveType = cfg.get<uint32_t>("mc_spec.interleaveType", 0);
    powerDownCycle = cfg.get<uint32_t>("mc_spec.powerDownCycle", 50);
    controllerLatency = cfg.get<uint32_t>("mc_spec.controllerLatency", 0);
    schedulerQueueCount = cfg.get<uint32_t>("mc_spec.schedulerQueueCount", 0);
    accessLogDepth = cfg.get<uint32_t>("mc_spec.accessLogDepth", 4);
    mergeContinuous  = cfg.get<bool>("mc_spec.mergeContinuous", false);
    cacheLineSize = _cacheLineSize;

    // loading Memory parameters
    chipCapacity = cfg.get<uint32_t>("mem_spec.capacityMb", 2048);
    bankCount    = cfg.get<uint32_t>("mem_spec.bankCount", 2);
    rowAddrWidth = cfg.get<uint32_t>("mem_spec.rowAddrWidth", 10);
    colAddrWidth = cfg.get<uint32_t>("mem_spec.colAddrWidth", 10);
    dataBusWidth = cfg.get<uint32_t>("mem_spec.dataBusWidth", 8);

    // Calculate paramers
    chipCountPerRank = channelDataWidth / dataBusWidth;
    rankCount = (totalCapacity * 8) / (chipCapacity * chipCountPerRank * channelCount);
    if(rankCount == 0)
        panic("Illegal specs!!! Please check mc_spec.capacityMB, mc_spec.channels, mem_spec.cpacityMb and mem_spec.dataBusWidth.");
    if((totalCapacity % channelCount) != 0)
        panic("Illegal specs!!! mc_spec.capacityMB must be multiple of mc_spec.channels.");

    info("totalCapacity = %d MBytes, chipCapacity = %d Mbits", totalCapacity, chipCapacity);
    info("channel data width = %d, chips per rank = %d, rank per channel = %d",
         channelDataWidth, chipCountPerRank, rankCount);

    rankWidth   = ilog2(rankCount);
    channelWidth = ilog2(channelCount);
    channelDataWidthLog = ilog2(channelDataWidth);
    bankWidth   = ilog2(bankCount);
    byteOffsetWidth = ilog2(cacheLineSize);
}

void MemParam::LoadTiming(Config &cfg)
{
    info("MemParam: Loading Timing Paramters");

    tCK  = cfg.get<double>("mem_spec.timing.tCK", 1.0);
    tCMD = ceil(cfg.get<double>("mem_spec.timing.tCMD", tCK)  / tCK);
    tRC  = ceil(cfg.get<double>("mem_spec.timing.tRC",  tCK)  / tCK);
    tRAS = ceil(cfg.get<double>("mem_spec.timing.tRAS", tCK)  / tCK);
    tRCD = ceil(cfg.get<double>("mem_spec.timing.tRCD", tCK)  / tCK);
    tRP  = ceil(cfg.get<double>("mem_spec.timing.tRP",  tCK)  / tCK);
    tRPab = ceil(cfg.get<double>("mem_spec.timing.tRPab", tCK) / tCK);
    tRTRS = ceil(cfg.get<double>("mem_spec.timing.tRTRS", tCK) / tCK);
    tRRD = ceil(cfg.get<double>("mem_spec.timing.tRRD", tCK)  / tCK);
    tWR  = ceil(cfg.get<double>("mem_spec.timing.tWR",  tCK)  / tCK);
    tWTR = ceil(cfg.get<double>("mem_spec.timing.tWTR", tCK)  / tCK);
    tCAS = ceil(cfg.get<double>("mem_spec.timing.tCAS", tCK)  / tCK);
    tCWD = ceil(cfg.get<double>("mem_spec.timing.tCWD", tCK)  / tCK);
    tCCD = ceil(cfg.get<double>("mem_spec.timing.tCCD", tCK)  / tCK);
    tTrans = ceil(cfg.get<double>("mem_spec.timing.tTrans", tCK*4) / tCK);
    tTransCrit = tTrans / 4;
    tXP  = ceil(cfg.get<double>("mem_spec.timing.tXP", tCK)   / tCK);
    tREFI = ceil(cfg.get<double>("mem_spec.timing.tREFI", tCK) / tCK);
    tRFC = ceil(cfg.get<double>("mem_spec.timing.tRFC", tCK)  / tCK);
    tFAW = ceil(cfg.get<double>("mem_spec.timing.tFAW", tCK)  / tCK);
    tRTP = ceil(cfg.get<double>("mem_spec.timing.tRTP", tCK)  / tCK);

    info("tCK  = %f", tCK);
    info("tCMD = %d tCK", tCMD);
    info("tRC  = %d tCK", tRC);
    info("tRAS = %d tCK", tRAS);
    info("tRCD = %d tCK", tRCD);
    info("tRP  = %d tCK", tRP);
    info("tRPab = %d tCK", tRPab);
    info("tRTRS = %d tCK", tRTRS);
    info("tRRD = %d tCK", tRRD);
    info("tWR  = %d tCK", tWR);
    info("tWTR = %d tCK", tWTR);
    info("tCAS = %d tCK", tCAS);
    info("tCWD = %d tCK", tCWD);
    info("tCCD = %d tCK", tCCD);
    info("tTrans = %d tCK", tTrans);
    info("tTransCrit = %d tCK", tTransCrit);
    info("tXP  = %d tCK", tXP);
    info("tREFI = %d tCK", tREFI);
    info("tRFC = %d tCK", tRFC);
    info("tFAW = %d tCK", tFAW);
    info("tRTP = %d tCK", tRTP);
}

void MemParam::LoadPower(Config &cfg)
{
    // loading Power Paramters
    // V -> 1/10V
    VDD1          = cfg.get<double>("mem_spec.power.VDD1.VDD1",  1.5) * 10;
    // mA -> 1/100mA
    IDD_VDD1.IDD0 = cfg.get<double>("mem_spec.power.VDD1.IDD0",  0.0) * 1e2;
    IDD_VDD1.IDD2P = cfg.get<double>("mem_spec.power.VDD1.IDD2P", 0.0) * 1e2;
    IDD_VDD1.IDD2N = cfg.get<double>("mem_spec.power.VDD1.IDD2N", 0.0) * 1e2;
    IDD_VDD1.IDD3P = cfg.get<double>("mem_spec.power.VDD1.IDD3P", 0.0) * 1e2;
    IDD_VDD1.IDD3N = cfg.get<double>("mem_spec.power.VDD1.IDD3N", 0.0) * 1e2;
    IDD_VDD1.IDD4R = cfg.get<double>("mem_spec.power.VDD1.IDD4R", 0.0) * 1e2;
    IDD_VDD1.IDD4W = cfg.get<double>("mem_spec.power.VDD1.IDD4W", 0.0) * 1e2;
    IDD_VDD1.IDD5 = cfg.get<double>("mem_spec.power.VDD1.IDD5", 0.0)  * 1e2;
    // mW -> uW
    readDqPin = cfg.get<double>("mem_spec.power.pins.readDQ", 0.0) * 1e3;
    writeDqPin = cfg.get<double>("mem_spec.power.pins.writeDQ", 0.0) * 1e3;
    readTermPin = cfg.get<double>("mem_spec.power.pins.readTerm", 0.0) * 1e3;
    writeTermPin = cfg.get<double>("mem_spec.power.pins.writeTerm", 0.0) * 1e3;

    info("Loading Memory Power Parameters");
    info("VDD1 (mV)      = %d", VDD1 * 100);
    info("VDD1.IDD0 (uA) = %d", IDD_VDD1.IDD0  * 10);
    info("VDD1.IDD2P (uA) = %d", IDD_VDD1.IDD2P * 10);
    info("VDD1.IDD2N (uA) = %d", IDD_VDD1.IDD2N * 10);
    info("VDD1.IDD3P (uA) = %d", IDD_VDD1.IDD3P * 10);
    info("VDD1.IDD3N (uA) = %d", IDD_VDD1.IDD3N * 10);
    info("VDD1.IDD4R (uA) = %d", IDD_VDD1.IDD4R * 10);
    info("VDD1.IDD4W (uA) = %d", IDD_VDD1.IDD4W * 10);
    info("VDD1.IDD5 (uA) = %d", IDD_VDD1.IDD5  * 10);
    info("readDq (uW)    = %d", readDqPin);
    info("writeDq (uW)   = %d", writeDqPin);
    info("readTerm (uW)  = %d", readTermPin);
    info("writeTerm (uW) = %d", writeTermPin);
}

uint32_t MemParam::GetDataLatency(uint32_t type)
{
    // Return read/write to data latency
    return (type == 0) ? tCAS : tCWD;
}

uint32_t MemParam::GetDataDelay(uint32_t type)
{
    // Return read/write to first data
    return ((type == 0) ? tCAS : tCWD) + tTransCrit;
}

uint32_t MemParam::GetDataSlot(uint32_t type)
{
    // Return data length
    return tTrans;
}

uint32_t MemParam::GetPreDelay(uint32_t type)
{
    // Return read/write to precharge
    return (type == 0) ? tRTP : (tCWD + tTrans + tWR);
}

uint32_t MemParam::GetRefreshCycle(void)
{
    // Return required cycle for refresh
    if(IsOpenRowBufPolicy())
        return tRFC + tRPab;
    else
        return tRFC;
}

uint32_t MemParam::GetRdWrDelay(uint32_t type, uint32_t lastType)
{
    // Return read/write to read/write constraint
    uint32_t index = lastType << 1 | type;
    return constraints[index];
}

void MemParam::MakeConstraints(void)
{
    //////////////////////////////////////////
    // make constraint for read/write to read/write command
    // [0bAB] : A=lastType, B=type
    //////////////////////////////////////////
    info("Generate DDR3 Timing Constraints for read/write to read/write");

    //idx=0 R->R
    constraints[0b00] = std::max(tTrans, tCCD);

    //idx=1 R->W
    constraints[0b01] = tCAS + tCCD/2 + 2 - tCWD;

    //idx=2 W->R
    constraints[0b10] = tCWD + tTrans + tWTR;

    //idx=3 W->W
    constraints[0b11] = std::max(tCCD, tTrans);
}

