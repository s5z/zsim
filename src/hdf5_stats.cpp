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

#include <fstream>
#include <hdf5.h>
#include <hdf5_hl.h>
#include <iostream>
#include <vector>
#include "galloc.h"
#include "log.h"
#include "stats.h"
#include "zsim.h"

/** Implements the HDF5 backend. Creates one big table in the file, and writes one row per dump.
 * NOTE: Because dump may be called from multiple processes, we close and open the HDF5 file every dump.
 * This is inefficient, but dumps are not that common anyhow, and we get the ability to read hdf5 files mid-simulation.
 * (alternatively, we could have an extra thread exclusively dedicated to writing stats).
 */
class HDF5BackendImpl : public GlobAlloc {
    private:
        const char* filename;
        AggregateStat* rootStat;
        bool skipVectors;
        bool sumRegularAggregates;

        uint64_t* dataBuf; //buffered record data
        uint64_t* curPtr; //points to next element to write in dump
        uint64_t recordSize; // in bytes
        uint32_t recordsPerWrite; //how many records to buffer; determines chunk size as well

        uint32_t bufferedRecords; //number of records buffered (dumped w/o being written), <= recordsPerWrite

        // Always have a single function to determine when to skip a stat to avoid inconsistencies in the code
        bool skipStat(Stat* s) {
            return skipVectors && dynamic_cast<VectorStat*>(s);
        }

        // Dump the stats, inorder walk
        void dumpWalk(Stat* s) {
            if (skipStat(s)) return;
            if (AggregateStat* as = dynamic_cast<AggregateStat*>(s)) {
                if (as->isRegular() && sumRegularAggregates) {
                    //Dump first record
                    uint64_t* startPtr = curPtr;
                    dumpWalk(as->get(0));
                    uint64_t* tmpPtr = curPtr;
                    uint32_t sz = tmpPtr - startPtr;
                    //Dump others below, and add them up
                    for (uint32_t i = 1; i < as->size(); i++) {
                        dumpWalk(as->get(i));
                        //Add record with previous ones
                        assert(curPtr == tmpPtr + sz);
                        for (uint32_t j = 0; j < sz; j++) startPtr[j] += tmpPtr[j];
                        //Rewind
                        curPtr = tmpPtr;
                    }
                } else {
                    for (uint32_t i = 0; i < as->size(); i++) {
                        dumpWalk(as->get(i));
                    }
                }
            } else if (Counter* cs = dynamic_cast<Counter*>(s)) {
                *(curPtr++) = cs->count();
            } else if (ScalarStat* ss = dynamic_cast<ScalarStat*>(s)) {
                *(curPtr++) = ss->get();
            } else if (VectorStat* vs = dynamic_cast<VectorStat*>(s)) {
                for (uint32_t i = 0; i < vs->size(); i++) {
                    *(curPtr++) = vs->count(i);
                }
            } else if (ProxyStat* ps = dynamic_cast<ProxyStat*>(s)) {
                *(curPtr++) = ps->stat();
            } else if (ProxyFuncStat* pfs = dynamic_cast<ProxyFuncStat*>(s)) {
                *(curPtr++) = pfs->stat();
            } else {
                panic("Unrecognized stat type");
            }
        }

        //Note this is a local vector, b/c it's only used at initialization.
        std::vector<hid_t> uniqueTypes;

        /* Gets an HDF5 type, compares it with every prior unique type, and returns the ID of the type to use.
         * Note that it will close the current type if it is a duplicate!
         * I'm not sure that this reduces type size (maybe with committed types?). It is good practice though --
         * we don't need thousands of equivalent types flying around inside the HDF5 library, who knows what goes
         * inside that place.
         */
        hid_t deduplicateH5Type(hid_t type) {
            std::vector<hid_t>::iterator it;
            for (it = uniqueTypes.begin(); it != uniqueTypes.end(); it++) {
                if (*it == type) {
                    // Avoid closing a type that was registered before
                    return type;
                }
                if (H5Tequal(*it, type)) {
                    //Must check we have created the type before closing it, otherwise the library screams :)
                    H5T_class_t typeClass = H5Tget_class(type);
                    if (typeClass == H5T_COMPOUND || typeClass == H5T_ARRAY) {
                        H5Tclose(type);
                    }
                    return *it;
                }
            }
            // This is indeed a new type
            uniqueTypes.push_back(type);
            return type;
        }

        /* Code to create a large compund datatype from an aggregate stat. ALWAYS returns deduplicated types */
        hid_t getH5Type(Stat* stat) { //I'd like to make this functional, but passing a member function as an argument is non-trivial...
            AggregateStat* aggrStat = dynamic_cast<AggregateStat*>(stat);
            if (aggrStat == NULL) {
                return getBaseH5Type(stat);
            } else if (aggrStat->isRegular()) {
                //This is a regular aggregate, i.e. an array of possibly compound types
                assert(aggrStat->size() > 0);
                assert(!skipStat(aggrStat->get(0))); //should not happen unless we start skipping compounds in the future.
                hid_t childType = getH5Type(aggrStat->get(0));
                //Sanity check
                for (uint32_t i = 1; i < aggrStat->size(); i++) {
                    hid_t otherType = getH5Type(aggrStat->get(i)); //already deduplicated
                    if (otherType != childType) {
                        panic("In regular aggregate %s, child %d has a different type than first child. Doesn't look regular to me!", stat->name(), i);
                    }
                }
                if (sumRegularAggregates) {
                    return childType; //this is already deduplicated
                } else {
                    hsize_t dims[] = {aggrStat->size()};
                    hid_t res = H5Tarray_create2(childType, 1 /*rank*/, dims);
                    return deduplicateH5Type(res);
                }
            } else {
                //Irregular aggregate
                //First pass to get sizes
                size_t size = 0;
                for (uint32_t i = 0; i < aggrStat->size(); i++) {
                    Stat* child = aggrStat->get(i);
                    if (skipStat(child)) continue;
                    size += H5Tget_size(getH5Type(child));
                }
                hid_t res = H5Tcreate(H5T_COMPOUND, size);
                size_t offset = 0;
                for (uint32_t i = 0; i < aggrStat->size(); i++) {
                    Stat* child = aggrStat->get(i);
                    if (skipStat(child)) continue;
                    hid_t childType = getH5Type(child);
                    H5Tinsert(res, child->name(), offset, childType);
                    offset += H5Tget_size(childType);
                }
                assert(size == offset);
                return deduplicateH5Type(res);
            }
        }

        /* Return type of non-aggregates. ALWAYS returns deduplicated types. */
        hid_t getBaseH5Type(Stat* s) {
            assert(dynamic_cast<AggregateStat*>(s) == NULL); //this can't be an aggregate
            hid_t res;
            uint32_t size = 1; //scalar by default
            if (VectorStat* vs = dynamic_cast<VectorStat*>(s)) {
                size = vs->size();
            }
            if (size > 1) {
                hsize_t dims[] = {size};
                res = H5Tarray_create2(H5T_NATIVE_ULONG, 1 /*rank*/, dims);
            } else {
                assert(size == 1);
                res = H5T_NATIVE_ULONG;
            }
            return deduplicateH5Type(res);
        }

    public:
        HDF5BackendImpl(const char* _filename, AggregateStat* _rootStat, size_t _bytesPerWrite, bool _skipVectors, bool _sumRegularAggregates) :
            filename(_filename), rootStat(_rootStat), skipVectors(_skipVectors), sumRegularAggregates(_sumRegularAggregates)
        {
            // Create stats file
            info("HDF5 backend: Opening %s", filename);
            hid_t fileID = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

            hid_t rootType = getH5Type(rootStat);

            //TODO: Use of table interface is vestigial at this point. Just create the dataset...
            size_t fieldOffsets[] = {0};
            hid_t fieldTypes[] = {rootType};
            const char* fieldNames[] = {rootStat->name()};
            recordSize = H5Tget_size(rootType);

            recordsPerWrite = _bytesPerWrite/recordSize + 1;

            herr_t hErrVal = H5TBmake_table("stats", fileID, "stats",
                    1 /*# fields*/, 0 /*# records*/,
                    recordSize, fieldNames, fieldOffsets, fieldTypes,
                    recordsPerWrite /*chunk size, in records, might as well be our aggregation degree*/,
                    NULL, 9 /*compression*/, NULL);
            assert(hErrVal == 0);

            size_t bufSize = recordsPerWrite*recordSize;
            if (sumRegularAggregates) bufSize += recordSize; //conservatively add space for a record. See dumpWalk(), we bleed into the buffer a bit when dumping a regular aggregate.
            dataBuf = static_cast<uint64_t*>(gm_malloc(bufSize));
            curPtr = dataBuf;

            bufferedRecords = 0;

            info("HDF5 backend: Created table, %ld bytes/record, %d records/write", recordSize, recordsPerWrite);
            H5Fclose(fileID);
        }

        ~HDF5BackendImpl() {}

        void dump(bool buffered) {
            // Copy stats to data buffer
            dumpWalk(rootStat);
            bufferedRecords++;

            assert_msg(dataBuf + bufferedRecords*recordSize/sizeof(uint64_t) == curPtr, "HDF5 (%s): %p + %d * %ld / %ld != %p", filename, dataBuf, bufferedRecords, recordSize, sizeof(uint64_t), curPtr);

            // Write to table if needed
            if (bufferedRecords == recordsPerWrite || !buffered) {
                hid_t fileID = H5Fopen(filename, H5F_ACC_RDWR, H5P_DEFAULT);

                size_t fieldOffsets[] = {0};
                size_t fieldSizes[] = {recordSize};
                H5TBappend_records(fileID, "stats", bufferedRecords, recordSize, fieldOffsets, fieldSizes, dataBuf);
                H5Fclose(fileID);

                //Rewind
                bufferedRecords = 0;
                curPtr = dataBuf;
            }
        }
};


HDF5Backend::HDF5Backend(const char* filename, AggregateStat* rootStat, size_t bytesPerWrite, bool skipVectors, bool sumRegularAggregates) {
    backend = new HDF5BackendImpl(filename, rootStat, bytesPerWrite, skipVectors, sumRegularAggregates);
}

void HDF5Backend::dump(bool buffered) {
    backend->dump(buffered);
}

