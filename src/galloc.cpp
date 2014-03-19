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

#include "galloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "log.h"  // NOLINT must precede dlmalloc, which defines assert if undefined
#include "g_heap/dlmalloc.h.c"
#include "locks.h"
#include "pad.h"

/* Base heap address. Has to be available cross-process. With 64-bit virtual
 * addresses, the address space is so sparse that it's quite easy to find
 * some random base that always works in practice. If for some weird reason
 * you want to compile this on a 32-bit address space, there are fancier,
 * more structured ways to get a common range (e.g. launch all the processes
 * before allocating the global heap segment, and find a common range either
 * by brute-force scanning and communicating through pipes, or by parsing
 * /proc/{pid}/maps).
 *
 * But, since I'm using a 64-bit address space, I don't really care to make
 * it fancy.
 */
#define GM_BASE_ADDR ((const void*)0x00ABBA000000)

struct gm_segment {
    volatile void* base_regp; //common data structure, accessible with glob_ptr; threads poll on gm_isready to determine when everything has been initialized
    volatile void* secondary_regp; //secondary data structure, used to exchange information between harness and initializing process
    mspace mspace_ptr;

    PAD();
    lock_t lock;
    PAD();
};

static gm_segment* GM = nullptr;
static int gm_shmid = 0;

/* Heap segment size, in bytes. Can't grow for now, so choose something sensible, and within the machine's limits (see sysctl vars kernel.shmmax and kernel.shmall) */
int gm_init(size_t segmentSize) {
    /* Create a SysV IPC shared memory segment, attach to it, and mark the segment to
     * auto-destroy when the number of attached processes becomes 0.
     *
     * IMPORTANT: There is a small window of vulnerability between shmget and shmctl that
     * can lead to major issues: between these calls, we have a segment of persistent
     * memory that will survive the program if it dies (e.g. someone just happens to send us
     * a SIGKILL)
     */

    assert(GM == nullptr);
    assert(gm_shmid == 0);
    gm_shmid = shmget(IPC_PRIVATE, segmentSize, 0644 | IPC_CREAT /*| SHM_HUGETLB*/);
    if (gm_shmid == -1) {
        perror("gm_create failed shmget");
        exit(1);
    }
    GM = static_cast<gm_segment*>(shmat(gm_shmid, GM_BASE_ADDR, 0));
    if (GM != GM_BASE_ADDR) {
        perror("gm_create failed shmat");
        warn("shmat failed, shmid %d. Trying not to leave garbage behind before dying...", gm_shmid);
        int ret = shmctl(gm_shmid, IPC_RMID, nullptr);
        if (ret) {
            perror("shmctl failed, we're leaving garbage behind!");
            panic("Check /proc/sysvipc/shm and manually delete segment with shmid %d", gm_shmid);
        } else {
            panic("shmctl succeeded, we're dying in peace");
        }
    }

    //Mark the segment to auto-destroy when the number of attached processes becomes 0.
    int ret = shmctl(gm_shmid, IPC_RMID, nullptr);
    assert(!ret);

    char* alloc_start = reinterpret_cast<char*>(GM) + 1024;
    size_t alloc_size = segmentSize - 1 - 1024;
    GM->base_regp = nullptr;

    GM->mspace_ptr = create_mspace_with_base(alloc_start, alloc_size, 1 /*locked*/);
    futex_init(&GM->lock);
    assert(GM->mspace_ptr);

    return gm_shmid;
}

void gm_attach(int shmid) {
    assert(GM == nullptr);
    assert(gm_shmid == 0);
    gm_shmid = shmid;
    GM = static_cast<gm_segment*>(shmat(gm_shmid, GM_BASE_ADDR, 0));
    if (GM != GM_BASE_ADDR) {
        warn("shmid %d \n", shmid);
        panic("gm_attach failed allocation");
    }
}


void* gm_malloc(size_t size) {
    assert(GM);
    assert(GM->mspace_ptr);
    futex_lock(&GM->lock);
    void* ptr = mspace_malloc(GM->mspace_ptr, size);
    futex_unlock(&GM->lock);
    if (!ptr) panic("gm_malloc(): Out of global heap memory, use a larger GM segment");
    return ptr;
}

void* __gm_calloc(size_t num, size_t size) {
    assert(GM);
    assert(GM->mspace_ptr);
    futex_lock(&GM->lock);
    void* ptr = mspace_calloc(GM->mspace_ptr, num, size);
    futex_unlock(&GM->lock);
    if (!ptr) panic("gm_calloc(): Out of global heap memory, use a larger GM segment");
    return ptr;
}

void* __gm_memalign(size_t blocksize, size_t bytes) {
    assert(GM);
    assert(GM->mspace_ptr);
    futex_lock(&GM->lock);
    void* ptr = mspace_memalign(GM->mspace_ptr, blocksize, bytes);
    futex_unlock(&GM->lock);
    if (!ptr) panic("gm_memalign(): Out of global heap memory, use a larger GM segment");
    return ptr;
}


void gm_free(void* ptr) {
    assert(GM);
    assert(GM->mspace_ptr);
    futex_lock(&GM->lock);
    mspace_free(GM->mspace_ptr, ptr);
    futex_unlock(&GM->lock);
}


char* gm_strdup(const char* str) {
    size_t l = strlen(str);
    char* res = static_cast<char*>(gm_malloc(l+1));
    memcpy(res, str, l+1);
    return res;
}


void gm_set_glob_ptr(void* ptr) {
    assert(GM);
    assert(GM->base_regp == nullptr);
    GM->base_regp = ptr;
}

void* gm_get_glob_ptr() {
    assert(GM);
    assert(GM->base_regp);
    return const_cast<void*>(GM->base_regp);  // devolatilize
}

void gm_set_secondary_ptr(void* ptr) {
    assert(GM);
    assert(GM->secondary_regp == nullptr);
    GM->secondary_regp = ptr;
}

void* gm_get_secondary_ptr() {
    assert(GM);
    assert(GM->secondary_regp != nullptr);
    return const_cast<void*>(GM->secondary_regp);  // devolatilize
}

void gm_stats() {
    assert(GM);
    mspace_malloc_stats(GM->mspace_ptr);
}

bool gm_isready() {
    assert(GM);
    return (GM->base_regp != nullptr);
}

void gm_detach() {
    assert(GM);
    shmdt(GM);
    GM = nullptr;
    gm_shmid = 0;
}


