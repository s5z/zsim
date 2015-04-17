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

#ifndef MUTEX_H_
#define MUTEX_H_

#include "g_std/stl_galloc.h"
#include "locks.h"
#include "pad.h"

// Until GCC is compliant with this, just inherit:
class mutex : public GlobAlloc {
    public:
        mutex() {
            futex_init(&futex);
        }

        void lock() {
            futex_lock(&futex);
        }

        void unlock() {
            futex_unlock(&futex);
        }

        bool haswaiters() {
            return futex_haswaiters(&futex);
        }

    private:
        volatile uint32_t futex;
};

class aligned_mutex : public mutex {} ATTR_LINE_ALIGNED;

class scoped_mutex : public GlobAlloc {
    public:
        scoped_mutex(mutex& _mut)
                : mut(&_mut) {
            mut->lock();
        }

        scoped_mutex(scoped_mutex&& that) {
            mut = that.mut;
            that.release();
        }

        scoped_mutex()
                : mut(0) {}

        ~scoped_mutex() {
            if (mut) mut->unlock();
        }

        scoped_mutex& operator=(scoped_mutex&& that) {
            this->~scoped_mutex();
            mut = that.mut;
            that.release();
            return *this;
        }

        void release() {
            mut = 0;
        }

        const mutex* get() const {
            return mut;
        }

    private:
        mutex* mut;
};

/* Read-write mutex based on futex locks. Fair implementation, with read
 * operations being somewhat less expensive in the common case of multiple
 * readers. Supports atomic downgrades from writer to reader.
 */
class rwmutex : public GlobAlloc {
    private:
        mutex wq;
        mutex rb;
        volatile uint32_t readers;

    public:
        rwmutex() {
            readers = 0;
        }

        void rdLock() {
            scoped_mutex r(rb);
            if (xadd(1) == 0) wq.lock();  // first reader disables writers
        }

        void rdUnlock() {
            if (xadd(-1) == 1) wq.unlock();  // last reader enables writers
        }

        void wrLock() {
            scoped_mutex r(rb);
            wq.lock();
        }

        void wrUnlock() {
            wq.unlock();
        }

        // NOTE: upgrade and downgrade have uncontended fastpaths. If this is a bottleneck, they could be optimized.

        // rdlocker -> wrlocker
        // MUST lose atomicity
        void upgrade() {
            rdUnlock();
            wrLock();
        }

        // wrlocker -> rdlocker
        void downgrade() {
            // This sequence does not drop atomically. We'd like to go from writer to reader without allowing intervening writers
#if 0
            wrUnlock();
            rdLock();
#else
            /* We want drops to be atomic, i.e., allow other readers to
             * progress with us, but never allow an intervening writer. There
             * are three possible situations:
             *
             * 1.- Nobody is blocked in anything else -> readers == 0, and we
             * can just become the first reader and keep wrlock.
             *
             * 2.- A writer is blocked in wq, and is possibly blocking all the
             * readers and writers on rb -> readers == 0, same.
             *
             * 3.- A *reader* is blocked in wq, and is possibly blocking all
             * other readers and writers on rb -> readers == 1, if we unlock wq
             * we'll let that reader and subsequent ones go through, and
             * writers will still be locked.
             *
             * readers > 1 is impossible, as we have wq. Similarly, because
             * both rdLock and wrLock are protected by rb, we cannot have more
             * than one waiter (reader or writer) in wq.
             */

            uint32_t oldReaders = xadd(1);
            if (oldReaders == 0) {
                // Cases 1, 2, or we raced with a reader but won first spot on xadd (which really is case 1)
                // We have wq, nothing left to do
            } else {
                assert(oldReaders == 1);
                // Case 3
                wq.unlock();
                // waiting reader will relock and proceed, and last of the bunch of concurrent readers will unlock
            }
#endif
        }

    private:
        inline uint32_t xadd(uint32_t v) {
            return __sync_fetch_and_add(&readers, v);
        }
};

#endif  // MUTEX_H_
