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

/* dsm: An attempt at having locks that don't suck */

#ifndef LOCKS_H_
#define LOCKS_H_

#include <linux/futex.h>
#include <stdint.h>
#include <syscall.h>
#include <unistd.h>

#ifdef WITH_MWAIT //careful with this define; most kernels don't allow mwait in userspace
#include <pmmintrin.h>  // NOLINT
#else
#include <xmmintrin.h>  // NOLINT
#endif

#include "log.h"

typedef volatile uint32_t lock_t;

/* SPINLOCK: A simple T&T&S spinlock. Lock can use monitor/mwait */

static inline void spin_init(volatile uint32_t* lock) {
    *lock = 0;
    __sync_synchronize();
}

static inline void spin_destroy(volatile uint32_t* lock) {}


static inline void spin_lock(volatile uint32_t* lock) {
    while (1) {
        if ((*lock) == 0 /*test (read)*/ && __sync_bool_compare_and_swap(lock, 0, 1) /*test&set*/) {
            break;
        }

        // At this point, we have the line in S/E/O, or M if we have tried the test&set and failed.
#if WITH_MWAIT
        //Monitor / mwait
        _mm_monitor((const void*)lock, 0, 0);

        //Must test again, might have intervening write BEFORE monitor (so we would get stuck in mwait)
        if (*lock) {
            _mm_mwait(0, 0);
        }
#else
        //If we don't have mwait, we can at least pause
        _mm_pause();
#endif
    }
}

static inline int spin_trylock(volatile uint32_t* lock) {
    return !((*lock) == 0 /*T*/ && __sync_bool_compare_and_swap(lock, 0, 1) /*T&S*/);
}


static inline void spin_unlock(volatile uint32_t* lock) {
    assert(*lock == 1); //should own lock if we're unlocking...
    *lock = 0;
    __sync_synchronize();
}

/* TICKET LOCK: Provides FIFO ordering for fairness.
 * WARNING: Will not work with more than 64K threads
 */

#define TICKET_MASK ((1<<16) - 1)

static inline void ticket_init(volatile uint32_t* lock) {
    *lock = 0;
    __sync_synchronize();
}

static inline void ticket_destroy(volatile uint32_t* lock) {}

static inline void ticket_lock(volatile uint32_t* lock) {
    /* Technically, we want to do this, but I'm guessing the 64-bit
     * datapath is not very well optimized for 16-bit xadd...
     * volatile uint16_t* low = ((volatile uint16_t*) lock) + 1;
     * uint32_t ticket = atomic_fetchadd_16(low, 1);
     */
    uint32_t val, hi, newLo;
    while (true) {
        val = *lock;
        hi = val & (TICKET_MASK << 16);
        newLo = (val + 1) & TICKET_MASK;
        if (__sync_bool_compare_and_swap(lock, val, (hi | newLo))) break;
    }

    uint32_t ticket = val & TICKET_MASK;

    while ((((*lock) >> 16) & TICKET_MASK) != ticket) {
#if WITH_MWAIT
        //Monitor / mwait
        _mm_monitor((const void*)lock, 0, 0);

        //Must test again, might have intervening write BEFORE monitor (so we would get stuck in mwait)
        if (*lock) {
            _mm_mwait(0, 0);
        }
#else
        //If we don't have mwait, we can at least pause
        _mm_pause();
#endif
    }
}

static inline int ticket_trylock(volatile uint32_t* lock) {
    uint32_t val = *lock;
    uint32_t hi = (val >> 16) & TICKET_MASK;
    uint32_t lo = val & TICKET_MASK;
    uint32_t newLo = (lo + 1) & TICKET_MASK;
    return (hi == lo /*This is up for grabs*/ && __sync_bool_compare_and_swap(lock, val, ((hi << 16) | newLo)) /*T&S*/);
}


static inline void ticket_unlock(volatile uint32_t* lock) {
    __sync_fetch_and_add(lock, 1<<16);
}


static inline void futex_init(volatile uint32_t* lock) {
    spin_init(lock);
}

/* NOTE: The current implementation of this lock is quite unfair. Not that we care for its current use. */
static inline void futex_lock(volatile uint32_t* lock) {
    uint32_t c;
    do {
        for (uint32_t i = 0; i < 5; i++) { //this should be tuned to balance syscall/context-switch and user-level spinning costs
            if (*lock == 0 && __sync_bool_compare_and_swap(lock, 0, 1)) {
                return;
            }

            // Do linear backoff instead of a single mm_pause; this reduces ping-ponging, and allows more time for the other hyperthread
            for (uint32_t j = 1; j < i+2; j++) _mm_pause();
        }

        //At this point, we will block
        c = __sync_lock_test_and_set(lock, 2); //this is not exactly T&S, but atomic exchange; see GCC docs
        if (c == 0) return;
        syscall(SYS_futex, lock, FUTEX_WAIT, 2, nullptr, nullptr, 0);
        c = __sync_lock_test_and_set(lock, 2); //atomic exchange
    } while (c != 0);
}

static inline void futex_lock_nospin(volatile uint32_t* lock) {
    uint32_t c;
    do {
        if (*lock == 0 && __sync_bool_compare_and_swap(lock, 0, 1)) {
            return;
        }

        //At this point, we will block
        c = __sync_lock_test_and_set(lock, 2); //this is not exactly T&S, but atomic exchange; see GCC docs
        if (c == 0) return;
        syscall(SYS_futex, lock, FUTEX_WAIT, 2, nullptr, nullptr, 0);
        c = __sync_lock_test_and_set(lock, 2); //atomic exchange
    } while (c != 0);
}

#define BILLION (1000000000L)
static inline bool futex_trylock_nospin_timeout(volatile uint32_t* lock, uint64_t timeoutNs) {
    if (*lock == 0 && __sync_bool_compare_and_swap(lock, 0, 1)) {
        return true;
    }

    //At this point, we will block
    uint32_t c = __sync_lock_test_and_set(lock, 2); //this is not exactly T&S, but atomic exchange; see GCC docs
    if (c == 0) return true;
    const struct timespec timeout = {(time_t) timeoutNs/BILLION, (time_t) timeoutNs % BILLION};
    syscall(SYS_futex, lock, FUTEX_WAIT, 2, &timeout, nullptr, 0);
    c = __sync_lock_test_and_set(lock, 2); //atomic exchange
    if (c == 0) return true;
    return false;
}

static inline void futex_unlock(volatile uint32_t* lock) {
    if (__sync_fetch_and_add(lock, -1) != 1) {
        *lock = 0;
        /* This may result in additional wakeups, but avoids completely starving processes that are
         * sleeping on this. Still, if there is lots of contention in userland, this doesn't work
         * that well. But I don't care that much, as this only happens between phase locks.
         */
        syscall(SYS_futex, lock, FUTEX_WAKE, 1 /*wake next*/, nullptr, nullptr, 0);
    }
}

// Returns true if this futex has *detectable waiters*, i.e., waiters in the kernel
// There may still be waiters spinning, but if you (a) acquire the lock, and (b) want
// to see if someone is queued behind you, this will eventually return true
// No false positives (if true, for sure there's someone)
static inline bool futex_haswaiters(volatile uint32_t* lock) {
    return *lock == 2;
}

#endif  // LOCKS_H_
