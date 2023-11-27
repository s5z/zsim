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

/* General logging/info/warn/panic routines */

#ifndef LOG_H_
#define LOG_H_

#include <sstream>
#include <stdio.h>
#include <stdlib.h>

void __log_lock();
void __log_unlock();

#ifdef MT_SAFE_LOG
#define log_lock() __log_lock()
#define log_unlock() __log_unlock()
#else
#define log_lock()
#define log_unlock()
#endif

#define PANIC_EXIT_CODE (112)

// assertions are often frequently executed but never inlined. Might as well tell the compiler about it
#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)

typedef enum {
    LOG_Harness,
    LOG_Config,
    LOG_Process,
    LOG_Cache,
    LOG_Mem,
    LOG_Sched,
    LOG_FSVirt,
    LOG_TimeVirt,
} LogType;

// defined in log.cpp
extern const char* logTypeNames[];
extern const char* logHeader;
extern FILE* logFdOut;
extern FILE* logFdErr;

/* Set per-process header for log/info/warn/panic messages
 * Calling this is not needed (the default header is ""),
 * but it helps in multi-process runs
 * If file is nullptr or InitLog is not called, logs to stdout/stderr
 */
void InitLog(const char* header, const char* file = nullptr);

/* Helper class to print expression with values
 * Inpired by Phil Nash's CATCH, https://github.com/philsquared/Catch
 * const enough that asserts that use this are still optimized through
 * loop-invariant code motion
 */
class PrintExpr {
    private:
        std::stringstream& ss;

    public:
        PrintExpr(std::stringstream& _ss) : ss(_ss) {}

        // Start capturing values
        template<typename T> const PrintExpr operator->* (T t) const { ss << t; return *this; }

        // Overloads for all lower-precedence operators
        template<typename T> const PrintExpr operator == (T t) const { ss << " == " << t; return *this; }
        template<typename T> const PrintExpr operator != (T t) const { ss << " != " << t; return *this; }
        template<typename T> const PrintExpr operator <= (T t) const { ss << " <= " << t; return *this; }
        template<typename T> const PrintExpr operator >= (T t) const { ss << " >= " << t; return *this; }
        template<typename T> const PrintExpr operator <  (T t) const { ss << " < "  << t; return *this; }
        template<typename T> const PrintExpr operator >  (T t) const { ss << " > "  << t; return *this; }
        template<typename T> const PrintExpr operator &  (T t) const { ss << " & "  << t; return *this; }
        template<typename T> const PrintExpr operator |  (T t) const { ss << " | "  << t; return *this; }
        template<typename T> const PrintExpr operator ^  (T t) const { ss << " ^ "  << t; return *this; }
        template<typename T> const PrintExpr operator && (T t) const { ss << " && " << t; return *this; }
        template<typename T> const PrintExpr operator || (T t) const { ss << " || " << t; return *this; }
        template<typename T> const PrintExpr operator +  (T t) const { ss << " + "  << t; return *this; }
        template<typename T> const PrintExpr operator -  (T t) const { ss << " - "  << t; return *this; }
        template<typename T> const PrintExpr operator *  (T t) const { ss << " * "  << t; return *this; }
        template<typename T> const PrintExpr operator /  (T t) const { ss << " / "  << t; return *this; }
        template<typename T> const PrintExpr operator %  (T t) const { ss << " % "  << t; return *this; }
        template<typename T> const PrintExpr operator << (T t) const { ss << " << " << t; return *this; }
        template<typename T> const PrintExpr operator >> (T t) const { ss << " >> " << t; return *this; }

        // std::nullptr_t overloads (for nullptr's in assertions)
        // Only a few are needed, since most ops w/ nullptr are invalid
        const PrintExpr operator->* (std::nullptr_t t) const { ss << "nullptr"; return *this; }
        const PrintExpr operator == (std::nullptr_t t) const { ss << " == nullptr"; return *this; }
        const PrintExpr operator != (std::nullptr_t t) const { ss << " != nullptr"; return *this; }

    private:
        template<typename T> const PrintExpr operator =  (T t) const;  // will fail, can't assign in assertion
};


#define panic(args...) \
{ \
    fprintf(logFdErr, "%sPanic on %s:%d: ", logHeader, __FILE__, __LINE__); \
    fprintf(logFdErr, args); \
    fprintf(logFdErr, "\n"); \
    fflush(logFdErr); \
    /**reinterpret_cast<int*>(0L) = 42;*/ /*SIGSEGVs*/ \
    exit(PANIC_EXIT_CODE); \
}

#define warn(args...) \
{ \
    log_lock(); \
    fprintf(logFdErr, "%sWARN: ", logHeader); \
    fprintf(logFdErr, args); \
    fprintf(logFdErr, "\n"); \
    fflush(logFdErr); \
    log_unlock(); \
}

#define info(args...) \
{ \
    log_lock(); \
    fprintf(logFdOut, "%s", logHeader); \
    fprintf(logFdOut, args); \
    fprintf(logFdOut, "\n"); \
    fflush(logFdOut); \
    log_unlock(); \
}

/* I would call these macros log, but there's this useless math function
 * that happens to conflict with this...
 */
/* FIXME: Better conditional tracing (e.g., via mask) */
#ifdef _LOG_TRACE_
#define trace(type, args...) \
{ \
    if ( LOG_##type == LOG_Sched) { \
        log_lock(); \
        fprintf(logFdErr, "%sLOG(%s): ", logHeader, logTypeNames[(int) LOG_##type]); \
        fprintf(logFdErr, args); \
        fprintf(logFdErr, "\n"); \
        fflush(logFdErr); \
        log_unlock(); \
    } \
}
#else
#define trace(type, args...)
#endif


#ifndef NASSERT
// yyf: using do {...} while(0) in macro solves two  (http://c-faq.com/cpp/multistmt.html)
// 1. avoid dangling else problem https://en.wikipedia.org/wiki/Dangling_else
// 2. if the programmer forgets to put semicolon after the macro, the compiler will yield syntax error
#define assert(expr)                                   \
do {                                                   \
    if (unlikely(!(expr))) {                           \
        std::stringstream __assert_ss__LINE__;         \
        (PrintExpr(__assert_ss__LINE__)->*expr);       \
        fprintf(logFdErr, "%sFailed assertion on %s:%d '%s' (with '%s')\n", logHeader, __FILE__, __LINE__, #expr, __assert_ss__LINE__.str().c_str()); \
        fflush(logFdErr);                              \
        *reinterpret_cast<int*>(0L) = 42; /*SIGSEGVs*/ \
        exit(1);                                       \
    }                                                  \
} while(0)

#define assert_msg(cond, args...)                      \
do {                                                   \
    if (unlikely(!(cond))) {                           \
        fprintf(logFdErr, "%sFailed assertion on %s:%d: ", logHeader, __FILE__, __LINE__); \
        fprintf(logFdErr, args);                       \
        fprintf(logFdErr, "\n");                       \
        fflush(logFdErr);                              \
        *reinterpret_cast<int*>(0L) = 42; /*SIGSEGVs*/ \
        exit(1);                                       \
    }                                                  \
} while(0)

#else
// Avoid unused warnings, never emit any code
// see http://cnicholson.net/2009/02/stupid-c-tricks-adventures-in-assert/
#define assert(cond) do { (void)sizeof(cond); } while (0)
#define assert_msg(cond, args...) do { (void)sizeof(cond); } while (0)
#endif

#define checkpoint()                                            \
    do {                                                        \
        info("%s:%d %s", __FILE__, __LINE__, __FUNCTION__);     \
    } while (0)

#endif  // LOG_H_
