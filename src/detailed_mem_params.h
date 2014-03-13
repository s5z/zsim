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

#ifndef __DETAILED_MEM_PARAMS_H__
#define __DETAILED_MEM_PARAMS_H__

#include "g_std/g_string.h"
#include "config.h"

class MemParam : public GlobAlloc{
    protected:
        enum eRowBufferPolicy {
            RB_CLOSE = 0,
            RB_OPEN
        };
        uint32_t rowBufferPolicy;
        int32_t constraints[4];

        virtual void LoadConfigMain(Config &cfg, uint32_t _chacheLineSize = 64);
        virtual void LoadTiming(Config &cfg);
        virtual void LoadPower(Config &cfg);
        virtual void MakeConstraints(void);

    public:
        MemParam();
        virtual ~MemParam();

        virtual void LoadConfig(g_string _cfgFile, uint32_t _chacheLineSize = 64);

        bool IsOpenRowBufPolicy();
        bool IsCloseRowBufPolicy();
        virtual uint32_t GetDataLatency(uint32_t type);
        virtual uint32_t GetDataDelay(uint32_t type);
        virtual uint32_t GetDataSlot(uint32_t type);
        virtual uint32_t GetPreDelay(uint32_t type);
        virtual uint32_t GetRefreshCycle(void);
        virtual uint32_t GetRdWrDelay(uint32_t type, uint32_t lastType);

        // Simulation Parameter
        uint32_t reportPhase;
        uint64_t reportStart;
        uint64_t reportFinish;

        // FIXME(dsm): These violate transparency... use info/warn!
        // I'm also not sure why these are here; can we move all the power-related reporting to a separate tool?
        bool anyReport;
        bool accAvgPowerReport;
        bool curAvgPowerReport;
        bool bandwidthReport;
        bool addrTrace;

        // Memory Controller Parameter
        uint32_t totalCapacity; // mega byte
        uint32_t channelCount;
        uint32_t interleaveType;
        uint32_t powerDownCycle;
        uint32_t controllerLatency;
        uint32_t cacheLineSize;
        uint32_t byteOffsetWidth;
        uint32_t accessLogDepth;
        bool mergeContinuous;
        uint32_t schedulerQueueCount;

        // Device Architectural Parameter
        uint32_t chipCapacity; // megabits
        uint32_t bankCount;
        uint32_t rowAddrWidth;
        uint32_t colAddrWidth;
        uint32_t dataBusWidth;

        uint32_t chipCountPerRank;
        uint32_t rankCount;
        uint32_t rankWidth;
        uint32_t channelWidth;
        uint32_t bankWidth;
        uint32_t channelDataWidth; // Data bus bits (= JEDEC_BUS_WIDTH)
        uint32_t channelDataWidthLog; // ilog2(Datawdith / 8)

        // Timing Parameters
        double tCK;
        uint32_t tCMD;
        uint32_t tRC;
        uint32_t tRAS;
        uint32_t tRCD;
        uint32_t tRP;
        uint32_t tRPab;
        uint32_t tRTRS;
        uint32_t tRRD;
        uint32_t tWR;
        uint32_t tWTR;
        uint32_t tCAS;
        uint32_t tCWD;
        uint32_t tCCD;
        uint32_t tTrans;
        uint32_t tTransCrit;
        uint32_t tXP;
        uint32_t tREFI;
        uint32_t tRFC;
        uint32_t tFAW;
        uint32_t tRTP;

        // Power Parameters
        // Voltage
        uint32_t VDD1;

        struct IDDs {
            uint32_t IDD0;
            uint32_t IDD2P;
            uint32_t IDD2N;
            uint32_t IDD3P;
            uint32_t IDD3N;
            uint32_t IDD4R;
            uint32_t IDD4W;
            uint32_t IDD5;
        };
        // Statically Allocate
        IDDs IDD_VDD1;

        uint32_t readDqPin;
        uint32_t writeDqPin;
        uint32_t readTermPin;
        uint32_t writeTermPin;
};

#endif /* __DETAILED_MEM_PARAMS_H__ */
