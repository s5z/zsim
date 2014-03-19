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

#include "partitioner.h"

// UMon

UMonMonitor::UMonMonitor(uint32_t _numLines, uint32_t _umonLines, uint32_t _umonBuckets, uint32_t _numPartitions, uint32_t _buckets)
        : PartitionMonitor(_buckets)
        , missCache(nullptr)
        , missCacheValid(false)
        , monitors(_numPartitions, nullptr) {
    assert(_numPartitions > 0);

    missCache = gm_calloc<uint32_t>(_buckets * _numPartitions);

    for (auto& monitor : monitors) {
        monitor = new UMon(_numLines, _umonLines, _umonBuckets);
    }
}

UMonMonitor::~UMonMonitor() {
    for (auto monitor : monitors) {
        delete monitor;
    }
    gm_free(missCache);
    monitors.clear();
}

void UMonMonitor::access(uint32_t partition, Address lineAddr) {
    assert(partition < monitors.size());
    monitors[partition]->access(lineAddr);

    // check optimization assumption -- we shouldn't cache all misses
    // if they are getting accessed while they are updated! -nzb
    assert(!missCacheValid);
    missCacheValid = false;
}

uint32_t UMonMonitor::getNumAccesses(uint32_t partition) const {
    assert(partition < monitors.size());

    auto monitor = monitors[partition];
    return monitor->getNumAccesses();
}

uint32_t UMonMonitor::get(uint32_t partition, uint32_t bucket) const {
    assert(partition < monitors.size());

    if (!missCacheValid) {
        getMissCurves();
        missCacheValid = true;
    }

    return missCache[partition*buckets+bucket];
}

void UMonMonitor::getMissCurves() const {
    for (uint32_t partition = 0; partition < getNumPartitions(); partition++) {
        getMissCurve(&missCache[partition*buckets], partition);
    }
}

void UMonMonitor::getMissCurve(uint32_t* misses, uint32_t partition) const {
    assert(partition < monitors.size());

    auto monitor = monitors[partition];
    uint32_t umonBuckets = monitor->getBuckets();
    uint64_t umonMisses[ umonBuckets ];

    monitor->getMisses(umonMisses);

    // Upsample or downsample

    // We have an odd number of elements; the last one is the one that
    // should not be aliased, as it is the one without buckets
    if (umonBuckets >= buckets) {
        uint32_t downsampleRatio = umonBuckets/buckets;
        assert(umonBuckets % buckets == 0);
        //info("Downsampling (or keeping sampling), ratio %d", downsampleRatio);
        for (uint32_t j = 0; j < buckets; j++) {
            misses[j] = umonMisses[j*downsampleRatio];
        }
        misses[buckets] = umonMisses[umonBuckets];
    } else {
        uint32_t upsampleRatio = buckets/umonBuckets;
        assert(buckets % umonBuckets == 0);
        //info("Upsampling , ratio %d", upsampleRatio);
        for (uint32_t j = 0; j < umonBuckets; j++) {
            misses[upsampleRatio*j] = umonMisses[j];
            double m0 = umonMisses[j];
            double m1 = umonMisses[j+1];
            for (uint32_t k = 1; k < upsampleRatio; k++) {
                double frac = ((double)k)/((double)upsampleRatio);
                double m = m0*(1-frac) + m1*(frac);
                misses[upsampleRatio*j + k] = (uint64_t)m;
            }
            misses[buckets] = umonMisses[umonBuckets];
        }
    }

    /*info("Miss utility curves %d:", partition);
      for (uint32_t j = 0; j <= buckets; j++) info(" misses[%d] = %ld", j, misses[j]);
      for (uint32_t j = 0; j <= umonBuckets; j++) info(" umonMisses[%d] = %ld", j, umonMisses[j]);
      */
}

void UMonMonitor::reset() {
    for (auto monitor : monitors) {
        monitor->startNextInterval();
    }
    missCacheValid = false;
}
