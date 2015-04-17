# -*- mode:python -*-

import os
Import("env")

commonSrcs = ["config.cpp", "galloc.cpp", "log.cpp", "pin_cmd.cpp"]
harnessSrcs = ["zsim_harness.cpp", "debug_harness.cpp"]

# By default, we compile all cpp files in libzsim.so. List the cpp files that
# should be excluded below (one per line and in order, to ease merges)
excludeSrcs = [
"fftoggle.cpp",
"dumptrace.cpp",
"sorttrace.cpp",
]
excludeSrcs += harnessSrcs

libEnv = env.Clone()
libEnv["CPPFLAGS"] += libEnv["PINCPPFLAGS"]
libEnv["LINKFLAGS"] += libEnv["PINLINKFLAGS"]
libEnv["LIBPATH"] += libEnv["PINLIBPATH"]
libEnv["LIBS"] += libEnv["PINLIBS"]

# Build syscall name file
def getSyscalls(): return os.popen("python ../../misc/list_syscalls.py").read().strip()
syscallSrc = libEnv.Substfile("virt/syscall_name.cpp", "virt/syscall_name.cpp.in",
        SUBST_DICT = {"SYSCALL_NAME_LIST" : getSyscalls()})

# Build libzsim.so
globSrcNodes = Glob("*.cpp") + Glob("virt/*.cpp")
libSrcs = [str(x) for x in globSrcNodes if str(x) not in excludeSrcs]
libSrcs += [str(x) for x in syscallSrc]
libSrcs = list(set(libSrcs)) # ensure syscallSrc is not duplicated
libEnv.SharedLibrary("zsim.so", libSrcs)

# Build tracing utilities (need hdf5 & dynamic linking)
traceEnv = env.Clone()
traceEnv["LIBS"] += ["hdf5", "hdf5_hl"]
traceEnv["OBJSUFFIX"] += "t"
traceEnv.Program("dumptrace", ["dumptrace.cpp", "access_tracing.cpp", "memory_hierarchy.cpp"] + commonSrcs)
traceEnv.Program("sorttrace", ["sorttrace.cpp", "access_tracing.cpp"] + commonSrcs)

# Build harness (static to make it easier to run across environments)
env["LINKFLAGS"] += " --static "
env["LIBS"] += ["pthread"]
env.Program("zsim", harnessSrcs + commonSrcs)

# Build additional utilities below
env.Program("fftoggle", ["fftoggle.cpp"] + commonSrcs)
