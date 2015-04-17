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

#include "access_tracing.h"
#include "bithacks.h"
#include <hdf5.h>
#include <hdf5_hl.h>

#define PT_CHUNKSIZE (1024*256u)  // 256K records (~6MB)

AccessTraceReader::AccessTraceReader(std::string _fname) : fname(_fname.c_str()) {
    hid_t fid = H5Fopen(fname.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid == H5I_INVALID_HID) panic("Could not open HDF5 file %s", fname.c_str());

    // Check that the trace finished
    hid_t fAttr = H5Aopen(fid, "finished", H5P_DEFAULT);
    uint32_t finished;
    H5Aread(fAttr, H5T_NATIVE_UINT, &finished);
    H5Aclose(fAttr);

    if (!finished) panic("Trace file %s unfinished (halted simulation?)", fname.c_str());

    // Populate numRecords & numChildren
    hsize_t nPackets;
    hid_t table = H5PTopen(fid, "accs");
    if (table == H5I_INVALID_HID) panic("Could not open HDF5 packet table");
    H5PTget_num_packets(table, &nPackets);
    numRecords = nPackets;

    hid_t ncAttr = H5Aopen(fid, "numChildren", H5P_DEFAULT);
    H5Aread(ncAttr, H5T_NATIVE_UINT, &numChildren);
    H5Aclose(ncAttr);

    curFrameRecord = 0;
    cur = 0;
    max = MIN(PT_CHUNKSIZE, numRecords);
    buf = max? gm_calloc<PackedAccessRecord>(max) : nullptr;

    if (max) {
        H5PTread_packets(table, 0, max, buf);
    }

    H5PTclose(table);
    H5Fclose(fid);
}

void AccessTraceReader::nextChunk() {
    assert(cur == max);
    curFrameRecord += max;

    if (curFrameRecord < numRecords) {
        cur = 0;
        max = MIN(PT_CHUNKSIZE, numRecords - curFrameRecord);
        hid_t fid = H5Fopen(fname.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        if (fid == H5I_INVALID_HID) panic("Could not open HDF5 file %s", fname.c_str());
        hid_t table = H5PTopen(fid, "accs");
        if (table == H5I_INVALID_HID) panic("Could not open HDF5 packet table");
        H5PTread_packets(table, curFrameRecord, max, buf);
        H5PTclose(table);
        H5Fclose(fid);
    } else {
        assert_msg(curFrameRecord == numRecords, "%ld %ld", curFrameRecord, numRecords);  // aaand we're done
    }
}


AccessTraceWriter::AccessTraceWriter(g_string _fname, uint32_t numChildren) : fname(_fname) {
    // Create record structure
    hid_t accType = H5Tenum_create(H5T_NATIVE_USHORT);
    uint16_t val;
    H5Tenum_insert(accType, "GETS", (val=GETS,&val));
    H5Tenum_insert(accType, "GETX", (val=GETX,&val));
    H5Tenum_insert(accType, "PUTS", (val=PUTS,&val));
    H5Tenum_insert(accType, "PUTX", (val=PUTX,&val));

    size_t offset = 0;
    size_t size = H5Tget_size(H5T_NATIVE_ULONG)*2 + H5Tget_size(H5T_NATIVE_UINT) + H5Tget_size(H5T_NATIVE_USHORT) + H5Tget_size(accType);
    hid_t recType = H5Tcreate(H5T_COMPOUND, size);
    auto insertType = [&](const char* name, hid_t type) {
        H5Tinsert(recType, name, offset, type);
        offset += H5Tget_size(type);
    };

    insertType("lineAddr", H5T_NATIVE_ULONG);
    insertType("cycle", H5T_NATIVE_ULONG);
    insertType("lat", H5T_NATIVE_UINT);
    insertType("childId", H5T_NATIVE_USHORT);
    insertType("accType", accType);

    hid_t fid = H5Fcreate(fname.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (fid == H5I_INVALID_HID) panic("Could not create HDF5 file %s", fname.c_str());
    
    // HACK: We want to use the SHUF filter... create the raw dataset instead of the packet table
    // hid_t table = H5PTcreate_fl(fid, "accs", recType, PT_CHUNKSIZE, 9);
    // if (table == H5I_INVALID_HID) panic("Could not create HDF5 packet table");
    hsize_t dims[1] = {0};
    hsize_t dims_chunk[1] = {PT_CHUNKSIZE};
    hsize_t maxdims[1] = {H5S_UNLIMITED};
    hid_t space_id = H5Screate_simple(1, dims, maxdims);

    hid_t plist_id = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist_id, 1, dims_chunk);
    H5Pset_shuffle(plist_id);
    H5Pset_deflate(plist_id, 9);

    hid_t table = H5Dcreate2(fid, "accs", recType, space_id, H5P_DEFAULT, plist_id, H5P_DEFAULT);
    if (table == H5I_INVALID_HID) panic("Could not create HDF5 dataset");
    H5Dclose(table);

    // info("%ld %ld %ld %ld", sizeof(PackedAccessRecord), size, offset, H5Tget_size(recType));
    assert(offset == size);
    assert(size == sizeof(PackedAccessRecord));

    hid_t ncAttr = H5Acreate2(fid, "numChildren", H5T_NATIVE_UINT, H5Screate(H5S_SCALAR), H5P_DEFAULT, H5P_DEFAULT);
    H5Awrite(ncAttr, H5T_NATIVE_UINT, &numChildren);
    H5Aclose(ncAttr);

    hid_t fAttr = H5Acreate2(fid, "finished", H5T_NATIVE_UINT, H5Screate(H5S_SCALAR), H5P_DEFAULT, H5P_DEFAULT);
    uint32_t finished = 0;
    H5Awrite(fAttr, H5T_NATIVE_UINT, &finished);
    H5Aclose(fAttr);

    H5Fclose(fid);

    // Initialize buffer
    buf = gm_calloc<PackedAccessRecord>(PT_CHUNKSIZE);
    cur = 0;
    max = PT_CHUNKSIZE;
    assert((uint32_t)(((char*) &buf[1]) - ((char*) &buf[0])) == sizeof(PackedAccessRecord));
}

void AccessTraceWriter::dump(bool cont) {
    hid_t fid = H5Fopen(fname.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    if (fid == H5I_INVALID_HID) panic("Could not open HDF5 file %s", fname.c_str());
    hid_t table = H5PTopen(fid, "accs");
    if (table == H5I_INVALID_HID) panic("Could not open HDF5 packet table");
    herr_t err = H5PTappend(table, cur, buf);
    assert(err >= 0);

    if (!cont) {
        hid_t fAttr = H5Aopen(fid, "finished", H5P_DEFAULT);
        uint32_t finished = 1;
        H5Awrite(fAttr, H5T_NATIVE_UINT, &finished);
        H5Aclose(fAttr);

        gm_free(buf);
        buf = nullptr;
        max = 0;
    }

    cur = 0;
    H5PTclose(table);
    H5Fclose(fid);
}
