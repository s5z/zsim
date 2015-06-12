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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "process_tree.h"
#include "virt/common.h"
#include "virt/port_virtualizer.h"
#include "zsim.h"

// Helper function
static struct sockaddr_in* GetSockAddr(ADDRINT guestAddr, size_t guestSize) {
    if (guestSize != sizeof(struct sockaddr_in)) return nullptr;
    struct sockaddr_in* res = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in));
    if (!safeCopy((struct sockaddr_in*) guestAddr, res) || res->sin_family != AF_INET) {
        free(res);
        return nullptr;
    }
    return res;
}

// Patch functions

PostPatchFn PatchBind(PrePatchArgs args) {
    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;

    ADDRINT sAddrPtr = PIN_GetSyscallArgument(ctxt, std, 1);
    ADDRINT sLen = PIN_GetSyscallArgument(ctxt, std, 2);
    struct sockaddr_in* servAddr = GetSockAddr(sAddrPtr, sLen);
    if (!servAddr) return NullPostPatch;  // invalid input or socketaddr family

    int port = ntohs(servAddr->sin_port);
    if (port != 0) {  // if port is 0, we don't need to virtualize, OS will assign a free one
        uint32_t portDomain = zinfo->procArray[procIdx]->getPortDomain();
        info("Virtualizing bind() to port %d (domain %d)", port, portDomain);
        zinfo->portVirt[portDomain]->lock(); //unlocked either on write failure below, or after the syscall
        int prevPort = zinfo->portVirt[portDomain]->lookupReal(port);
        if (prevPort == -1) {
            // No previous bind(), request whatever
            servAddr->sin_port = htons(0);
        } else {
            // There was a previous bind() on this port, so we reuse the translation
            // This should work in MOST cases, but may fail if the port is reused by something else and we conflict. Should be quite rare, since Linux tries to space out anonymous reassigns to the same port
            warn("bind() to port %d, this port already has a translation %d, using it --- in rare cases this may fail when the unvirtualized case should succeed", port, prevPort);
            servAddr->sin_port = htons(prevPort);
        }
        PIN_SetSyscallArgument(ctxt, std, 1, (ADDRINT) servAddr);

        auto postFn = [sAddrPtr](PostPatchArgs args) {
            struct sockaddr_in* servAddr = (struct sockaddr_in*) PIN_GetSyscallArgument(args.ctxt, args.std, 1);
            int virtPort = ntohs(((struct sockaddr_in*)sAddrPtr)->sin_port);

            uint32_t portDomain = zinfo->procArray[procIdx]->getPortDomain();
            REG out = (REG) PIN_GetSyscallNumber(args.ctxt, args.std);
            if (out == 0) {
                int sockfd = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
                struct sockaddr_in sockName; //NOTE: sockaddr_in to sockaddr casts are fine
                socklen_t sockLen = sizeof(sockName);
                if (getsockname(sockfd, (struct sockaddr*)&sockName, &sockLen) != 0) {
                    panic("bind() succeeded, but getsockname() failed...");
                }
                int realPort = ntohs(sockName.sin_port);

                info("Virtualized bind(), v: %d r: %d (domain %d)", virtPort, realPort, portDomain);
                zinfo->portVirt[portDomain]->registerBind(virtPort, realPort);
            } else {
                info("bind(): tried to virtualize port, but bind() failed, not registering (domain %d)", portDomain);
            }
            zinfo->portVirt[portDomain]->unlock();  // note lock was in prepatch

            // Restore original descriptor, free alloc
            PIN_SetSyscallArgument(args.ctxt, args.std, 1, sAddrPtr);
            free(servAddr);
            return PPA_NOTHING;
        };
        return postFn;
    } else {
        free(servAddr);
        return NullPostPatch;
    }
}

PostPatchFn PatchGetsockname(PrePatchArgs args) {
    return [](PostPatchArgs args) {
        CONTEXT* ctxt = args.ctxt;
        SYSCALL_STANDARD std = args.std;

        REG out = (REG) PIN_GetSyscallNumber(ctxt, std);
        if (out == 0) {
            ADDRINT sockAddrPtr = PIN_GetSyscallArgument(ctxt, std, 1);
            struct sockaddr_in sockAddr;
            //safecopy may fail here and that's OK, it's just not a sockaddr_in, so not IPv4
            if (safeCopy((struct sockaddr_in*) sockAddrPtr, &sockAddr) && sockAddr.sin_family == AF_INET) {
                int realPort = ntohs(sockAddr.sin_port);
                uint32_t portDomain = zinfo->procArray[procIdx]->getPortDomain();
                zinfo->portVirt[portDomain]->lock();
                int virtPort = zinfo->portVirt[portDomain]->lookupVirt(realPort);
                zinfo->portVirt[portDomain]->unlock();
                if (virtPort != -1) {
                    info("Virtualizing getsockname() on previously bound port, r: %d, v: %d (domain %d)", realPort, virtPort, portDomain);
                    sockAddr.sin_port = htons(virtPort);
                    if (!safeCopy(&sockAddr, (struct sockaddr_in*) sockAddrPtr)) {
                        panic("getsockname() virt fail");
                    }
                }
            }
        } //else this failed, no need to virtualize
        return PPA_NOTHING;
    };
}

PostPatchFn PatchConnect(PrePatchArgs args) {
    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;

    ADDRINT sAddrPtr = PIN_GetSyscallArgument(ctxt, std, 1);
    ADDRINT sLen = PIN_GetSyscallArgument(ctxt, std, 2);
    struct sockaddr_in* servAddr = GetSockAddr(sAddrPtr, sLen);
    if (!servAddr) return NullPostPatch;  // invalid input or socketaddr family

    int virtPort = ntohs(servAddr->sin_port);
    uint32_t portDomain = zinfo->procArray[procIdx]->getPortDomain();
    zinfo->portVirt[portDomain]->lock();
    int realPort = zinfo->portVirt[portDomain]->lookupReal(virtPort);
    zinfo->portVirt[portDomain]->unlock();
    if (realPort != -1) {
        info("Virtualizing connect(), v: %d r: %d (domain %d)", virtPort, realPort, portDomain);
        servAddr->sin_port = htons(realPort);
        PIN_SetSyscallArgument(ctxt, std, 1, (ADDRINT) servAddr);

        auto postFn = [sAddrPtr, servAddr](PostPatchArgs args) {
            //Restore original (virt) port (NOTE: regardless of whether connect() succeeded or not)
            PIN_SetSyscallArgument(args.ctxt, args.std, 1, sAddrPtr);
            free(servAddr);
            return PPA_NOTHING;
        };
        return postFn;
    } else {
        free(servAddr);
        return NullPostPatch;
    }
}

