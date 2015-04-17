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

#ifndef PARTITION_MAPPER_H_
#define PARTITION_MAPPER_H_

#include <stdint.h>
#include "galloc.h"
#include "memory_hierarchy.h"

//Interface
class PartMapper : public GlobAlloc {
    public:
        virtual uint32_t getNumPartitions()=0;
        virtual uint32_t getPartition(const MemReq& req)=0;
};

class CorePartMapper : public PartMapper {
    private:
        uint32_t numCores;
    public:
        explicit CorePartMapper(uint32_t _numCores) : numCores(_numCores) {}
        virtual uint32_t getNumPartitions() {return numCores;}
        virtual uint32_t getPartition(const MemReq& req);
};

class InstrDataPartMapper : public PartMapper {
    public:
        virtual uint32_t getNumPartitions() {return 2;}
        virtual uint32_t getPartition(const MemReq& req);
};

class InstrDataCorePartMapper : public PartMapper {
    private:
        uint32_t numCores;
    public:
        explicit InstrDataCorePartMapper(uint32_t _numCores) : numCores(_numCores) {}
        virtual uint32_t getNumPartitions() {return 2*numCores;}
        virtual uint32_t getPartition(const MemReq& req);
};

class ProcessPartMapper : public PartMapper {
    private:
        uint32_t numProcs;
    public:
        explicit ProcessPartMapper(uint32_t _numProcs) : numProcs(_numProcs) {}
        virtual uint32_t getNumPartitions() {return numProcs;}
        virtual uint32_t getPartition(const MemReq& req);
};

class InstrDataProcessPartMapper : public PartMapper {
    private:
        uint32_t numProcs;
    public:
        explicit InstrDataProcessPartMapper(uint32_t _numProcs) : numProcs(_numProcs) {}
        virtual uint32_t getNumPartitions() {return 2*numProcs;}
        virtual uint32_t getPartition(const MemReq& req);
};

class ProcessGroupPartMapper : public PartMapper {
    public:
        ProcessGroupPartMapper() {}
        virtual uint32_t getNumPartitions();
        virtual uint32_t getPartition(const MemReq& req);
};

#endif  // PARTITION_MAPPER_H_


