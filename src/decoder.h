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

#ifndef DECODER_H_
#define DECODER_H_

#include <stdint.h>
#include <vector>
#include "pin.H"

// Uncomment to get a count of BBLs run. This is currently used to get a distribution of inaccurate instructions decoded that are actually run
// NOTE: This is not multiprocess-safe
// #define BBL_PROFILING
// #define PROFILE_ALL_INSTRS

// uop reg limits
#define MAX_UOP_SRC_REGS 2
#define MAX_UOP_DST_REGS 2

/* NOTE this uses stronly typed enums, a C++11 feature. This saves a bunch of typecasts while keeping UopType enums 1-byte long.
 * If you use gcc < 4.6 or some other compiler, either go back to casting or lose compactness in the layout.
 */
enum UopType : uint8_t {UOP_GENERAL, UOP_LOAD, UOP_STORE, UOP_STORE_ADDR, UOP_FENCE};

struct DynUop {
    uint16_t rs[MAX_UOP_SRC_REGS];
    uint16_t rd[MAX_UOP_DST_REGS];
    uint16_t lat;
    uint16_t decCycle;
    UopType type; //1 byte
    uint8_t portMask;
    uint8_t extraSlots; //FU exec slots
    uint8_t pad; //pad to 4-byte multiple

    void clear();
};  // 16 bytes. TODO(dsm): check performance with wider operands

struct DynBbl {
#ifdef BBL_PROFILING
    uint64_t bblIdx;
#endif
    uint64_t addr;
    uint32_t uops;
    uint32_t approxInstrs;
    DynUop uop[1];

    static uint32_t bytes(uint32_t uops) {
        return offsetof(DynBbl, uop) + sizeof(DynUop)*uops /*wtf... offsetof doesn't work with uop[uops]*/;
    }

    void init(uint64_t _addr, uint32_t _uops, uint32_t _approxInstrs) {
        // NOTE: this is a POD type, so we don't need to call a constructor; otherwise, we should use placement new
        uops = _uops;
        approxInstrs = _approxInstrs;
    }
};

struct BblInfo;  // defined in core.h

/* These are absolute maximums per instruction. If there is some non-conforming instruction, either increase these limits or
 * treat it as a special case.
 */
#define MAX_INSTR_LOADS 4
#define MAX_INSTR_REG_READS 4
#define MAX_INSTR_REG_WRITES 4
#define MAX_INSTR_STORES 4

#define MAX_UOPS_PER_INSTR 12  // technically, even full decoders produce 1-4 uops; we increase this for common microsequenced instructions (e.g. xchg).

/* Temporary register offsets */
#define REG_LOAD_TEMP (REG_LAST + 1)  // REG_LAST defined by PIN
#define REG_STORE_TEMP (REG_LOAD_TEMP + MAX_INSTR_LOADS)
#define REG_STORE_ADDR_TEMP (REG_STORE_TEMP + MAX_INSTR_STORES)
#define REG_EXEC_TEMP (REG_STORE_ADDR_TEMP + MAX_INSTR_STORES)

#define MAX_REGISTERS (REG_EXEC_TEMP + 64)

typedef std::vector<DynUop> DynUopVec;

//Nehalem-style decoder. Fully static for now
class Decoder {
    private:
        struct Instr {
            INS ins;

            uint32_t loadOps[MAX_INSTR_LOADS];
            uint32_t numLoads;

            //These contain the register indices; by convention, flags registers are stored last
            uint32_t inRegs[MAX_INSTR_REG_READS];
            uint32_t numInRegs;
            uint32_t outRegs[MAX_INSTR_REG_WRITES];
            uint32_t numOutRegs;

            uint32_t storeOps[MAX_INSTR_STORES];
            uint32_t numStores;

            explicit Instr(INS _ins);

            private:
                //Put registers in some canonical order -- non-flags first
                void reorderRegs(uint32_t* regArray, uint32_t numRegs);
        };

    public:
        //If oooDecoding is true, produces a DynBbl with DynUops that can be used in OOO cores
        static BblInfo* decodeBbl(BBL bbl, bool oooDecoding);

#ifdef BBL_PROFILING
        static void profileBbl(uint64_t bblIdx);
        static void dumpBblProfile();
#endif

    private:
        //Return true if inaccurate decoding, false if accurate
        static bool decodeInstr(INS ins, DynUopVec& uops);

        /* Every emit function can produce 0 or more uops; it returns the number of uops. These are basic templates to make our life easier */

        //By default, these emit to temporary registers that depend on the index; this can be overriden, e.g. for moves
        static void emitLoad(Instr& instr, uint32_t idx, DynUopVec& uops, uint32_t destReg = 0);
        static void emitStore(Instr& instr, uint32_t idx, DynUopVec& uops, uint32_t srcReg = 0);

        //Emit all loads and stores for this uop
        static void emitLoads(Instr& instr, DynUopVec& uops);
        static void emitStores(Instr& instr, DynUopVec& uops);

        //Emits a load-store fence uop
        static void emitFence(DynUopVec& uops, uint32_t lat);

        static void emitExecUop(uint32_t rs0, uint32_t rs1, uint32_t rd0, uint32_t rd1,
                DynUopVec& uops, uint32_t lat, uint8_t ports, uint8_t extraSlots = 0);

        /* Instruction emits */

        static void emitBasicMove(Instr& instr, DynUopVec& uops, uint32_t lat, uint8_t ports);
        static void emitConditionalMove(Instr& instr, DynUopVec& uops, uint32_t lat, uint8_t ports);

        // 1 "exec" uop, 0-2 inputs, 0-2 outputs
        static void emitBasicOp(Instr& instr, DynUopVec& uops, uint32_t lat, uint8_t ports,
                uint8_t extraSlots = 0, bool reportUnhandled = true);

        // >1 exec uops in a chain: each uop takes 2 inputs, produces 1 output to the next op
        // in the chain; the final op writes to the 0-2 outputs
        static void emitChainedOp(Instr& instr, DynUopVec& uops, uint32_t numUops,
                uint32_t* latArray, uint8_t* portsArray);

        // Some convert ops need 2 chained exec uops, though they have a single input and output
        static void emitConvert2Op(Instr& instr, DynUopVec& uops, uint32_t lat1, uint32_t lat2,
                uint8_t ports1, uint8_t ports2);

        /* Specific cases */
        static void emitXchg(Instr& instr, DynUopVec& uops);
        static void emitMul(Instr& instr, DynUopVec& uops);
        static void emitDiv(Instr& instr, DynUopVec& uops);

        static void emitCompareAndExchange(Instr&, DynUopVec&);

        /* Other helper functions */
        static void reportUnhandledCase(Instr& instr, const char* desc);
        static void populateRegArrays(Instr& instr, uint32_t* srcRegs, uint32_t* dstRegs);
        static void dropStackRegister(Instr& instr);

        /* Macro-op (ins) fusion */
        static bool canFuse(INS ins);
        static bool decodeFusedInstrs(INS ins, DynUopVec& uops);
};

#endif  // DECODER_H_
