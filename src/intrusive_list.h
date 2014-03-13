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

#ifndef INTRUSIVE_LIST_H_
#define INTRUSIVE_LIST_H_

/* Intrusive doubly-linked list -- simple enough to not include boost,
 * but might want to switch at some point
 */

#include "log.h"

template <typename T>
class InList;

template <typename T>
struct InListNode {
    T* next;
    T* prev;
    InList<T>* owner;

    InListNode() {
        next = NULL;
        prev = NULL;
        owner = NULL;
    }

    void unlink(InList<T>* lst) {
        if (next) next->prev = prev;
        if (prev) prev->next = next;
        next = NULL;
        prev = NULL;
        assert(lst == owner);
        owner = NULL;
    }

    void linkPrev(T* p, InList<T>* lst) {
        assert(p);
        assert(owner == NULL);
        assert(prev == NULL && next == NULL);
        if (p->next) {
            assert(p->next->prev == p);
            p->next->prev = static_cast<T*>(this);
            next = p->next;
        }
        p->next = static_cast<T*>(this);
        prev = p;
        owner = lst;
    }
};

template <typename T>
class InList {
    private:
        T* head;
        T* tail;
        size_t elems;

    public:
        InList() : head(NULL), tail(NULL), elems(0) {}
        bool empty() const {return !head;}

        T* front() const {return head;}
        T* back() const {return tail;}

        void push_front(T* e) {
            assert(e && e->next == NULL && e->prev == NULL && e->owner == NULL);
            if (empty()) {
                head = e;
                tail = e;
            } else {
                assert(head && head->prev == NULL && head->owner == this);
                e->next = head;
                head->prev = e;
                head = e;
            }
            e->owner = this;
            elems++;
        }

        void push_back(T* e) {
            assert(e && e->next == NULL && e->prev == NULL && e->owner == NULL);
            if (empty()) {
                head = e;
                tail = e;
                e->owner = this;
            } else {
                assert(tail);
                e->linkPrev(tail, this);
                tail = e;
            }
            elems++;
        }

        void pop_front() {
            if (empty()) return;
            T* e = head;
            head = e->next;
            e->unlink(this);
            if (!head) tail = NULL;
            elems--;
        }

        void pop_back() {
            if (empty()) return;
            T* e = tail;
            tail = e->prev;
            e->unlink(this);
            if (!tail) head = NULL;
            elems--;
        }

        //Note how remove is O(1)
        void remove(T* e) {
            //info("Remove PRE h=%p t=%p e=%p", head, tail, e);
            if (e == head) head = e->next;
            if (e == tail) tail = e->prev;
            e->unlink(this);
            elems--;
            //info("Remove POST h=%p t=%p e=%p", head, tail);
        }

        void insertAfter(T* prev, T* e) {
            assert(e && e->owner == NULL);
            assert(prev && prev->owner == this);
            e->linkPrev(prev, this);
            if (prev == tail) tail = e;
            elems++;
        }

        size_t size() const {
            return elems;
        }

#if 0  // Verify all internal state; call to test list implementation
        void verify() {
            if (empty()) {
                assert(head == NULL && tail == NULL && elems == 0);
            } else {
                T* c = head;
                size_t count = 0;
                while (c) {
                    if (c->next) assert(c->next->prev);
                    if (!c->next) assert(c == tail);
                    assert(c->owner == this);
                    count++;
                    c = c->next;
                }
                assert(count == elems);
            }
        }
#endif
};

#endif  // INTRUSIVE_LIST_H_

