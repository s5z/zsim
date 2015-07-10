import os, sys
from os.path import join as joinpath

useIcc = False
#useIcc = True

def buildSim(cppFlags, dir, type, pgo=None):
    ''' Build the simulator with a specific base buid dir and config type'''

    buildDir = joinpath(dir, type)
    print "Building " + type + " zsim at " + buildDir

    env = Environment(ENV = os.environ, tools = ['default', 'textfile'])
    env["CPPFLAGS"] = cppFlags

    allSrcs = [f for dir, subdirs, files in os.walk("src") for f in Glob(dir + "/*")]
    versionFile = joinpath(buildDir, "version.h")
    if os.path.exists(".git"):
        env.Command(versionFile, allSrcs + [".git/index", "SConstruct"],
            'echo -e "#define ZSIM_BUILDDATE \\""`date`\\""\\\\n#define ZSIM_BUILDVERSION \\""`python misc/gitver.py`\\""" >>' + versionFile)
    else:
        env.Command(versionFile, allSrcs + ["SConstruct"],
            'echo -e "#define ZSIM_BUILDDATE \\""`date`\\""\\\\n#define ZSIM_BUILDVERSION \\""no git repo\\""" >>' + versionFile)

    # Parallel builds?
    #env.SetOption('num_jobs', 32)

    # Use link-time optimization? It's still a bit buggy, so be careful
    #env['CXX'] = 'g++ -flto -flto-report -fuse-linker-plugin'
    #env['CC'] = 'gcc -flto'
    #env["LINKFLAGS"] = " -O3 -finline "
    if useIcc:
        env['CC'] = 'icc'
        env['CXX'] = 'icpc -ipo'

    # Required paths
    if "PINPATH" in os.environ:
        PINPATH = os.environ["PINPATH"]
    else:
       print "ERROR: You need to define the $PINPATH environment variable with Pin's path"
       sys.exit(1)

    ROOT = Dir('.').abspath

    # NOTE: These flags are for the 28/02/2011 2.9 PIN kit (rev39599). Older versions will not build.
    # NOTE (dsm 10 Jan 2013): Tested with Pin 2.10 thru 2.12 as well
    # NOTE: Original Pin flags included -fno-strict-aliasing, but zsim does not do type punning
    # NOTE (dsm 16 Apr 2015): Update flags code to support Pin 2.14 while retaining backwards compatibility
    env["CPPFLAGS"] += " -g -std=c++0x -Wall -Wno-unknown-pragmas -fomit-frame-pointer -fno-stack-protector"
    env["CPPFLAGS"] += " -MMD -DBIGARRAY_MULTIPLIER=1 -DUSING_XED -DTARGET_IA32E -DHOST_IA32E -fPIC -DTARGET_LINUX"

    # Pin 2.12+ kits have changed the layout of includes, detect whether we need
    # source/include/ or source/include/pin/
    pinInclDir = joinpath(PINPATH, "source/include/")
    if not os.path.exists(joinpath(pinInclDir, "pin.H")):
        pinInclDir = joinpath(pinInclDir, "pin")
        assert os.path.exists(joinpath(pinInclDir, "pin.H"))

    # Pin 2.14 changes location of XED
    xedName = "xed2"  # used below
    xedPath = joinpath(PINPATH, "extras/" + xedName + "-intel64/include")
    if not os.path.exists(xedPath):
        xedName = "xed"
        xedPath = joinpath(PINPATH, "extras/" + xedName + "-intel64/include")
        assert os.path.exists(xedPath)

    env["CPPPATH"] = [xedPath,
            pinInclDir, joinpath(pinInclDir, "gen"),
            joinpath(PINPATH, "extras/components/include")]

    # Perform trace logging?
    ##env["CPPFLAGS"] += " -D_LOG_TRACE_=1"

    # Uncomment to get logging messages to stderr
    ##env["CPPFLAGS"] += " -DDEBUG=1"

    # Be a Warning Nazi? (recommended)
    env["CPPFLAGS"] += " -Werror "

    # Enables lib and harness to use the same info/log code,
    # but only lib uses pin locks for thread safety
    env["PINCPPFLAGS"] = " -DMT_SAFE_LOG "

    # PIN-specific libraries
    env["PINLINKFLAGS"] = " -Wl,--hash-style=sysv -Wl,-Bsymbolic -Wl,--version-script=" + joinpath(pinInclDir, "pintool.ver")

    # To prime system libs, we include /usr/lib and /usr/lib/x86_64-linux-gnu
    # first in lib path. In particular, this solves the issue that, in some
    # systems, Pin's libelf takes precedence over the system's, but it does not
    # include symbols that we need or it's a different variant (we need
    # libelfg0-dev in Ubuntu systems)
    env["PINLIBPATH"] = ["/usr/lib", "/usr/lib/x86_64-linux-gnu", joinpath(PINPATH, "extras/" + xedName + "-intel64/lib"),
            joinpath(PINPATH, "intel64/lib"), joinpath(PINPATH, "intel64/lib-ext")]

    # Libdwarf is provided in static and shared variants, Ubuntu only provides
    # static, and I don't want to add -R<pin path/intel64/lib-ext> because
    # there are some other old libraries provided there (e.g., libelf) and I
    # want to use the system libs as much as possible. So link directly to the
    # static version of libdwarf.

    # Pin 2.14 uses unambiguous libpindwarf
    pindwarfPath = joinpath(PINPATH, "intel64/lib-ext/libdwarf.a")
    pindwarfLib = File(pindwarfPath)
    if not os.path.exists(pindwarfPath):
        pindwarfLib = "pindwarf"

    env["PINLIBS"] = ["pin", "xed", pindwarfLib, "elf", "dl", "rt"]

    # Non-pintool libraries
    env["LIBPATH"] = []
    env["LIBS"] = ["config++"]

    env["LINKFLAGS"] = ""

    if useIcc:
        # icc libs
        env["LINKFLAGS"] += " -Wl,-R/data/sanchez/tools/intel/composer_xe_2013.1.117/compiler/lib/intel64/"

    # Use non-standard library paths if defined
    if "LIBCONFIGPATH" in os.environ:
        LIBCONFIGPATH = os.environ["LIBCONFIGPATH"]
        env["LINKFLAGS"] += " -Wl,-R" + joinpath(LIBCONFIGPATH, "lib")
        env["LIBPATH"] += [joinpath(LIBCONFIGPATH, "lib")]
        env["CPPPATH"] += [joinpath(LIBCONFIGPATH, "include")]


    if "POLARSSLPATH" in os.environ:
        POLARSSLPATH = os.environ["POLARSSLPATH"]
        env["PINLIBPATH"] += [joinpath(POLARSSLPATH, "library")]
        env["CPPPATH"] += [joinpath(POLARSSLPATH, "include")]
        env["PINLIBS"] += ["polarssl"]
        env["CPPFLAGS"] += " -D_WITH_POLARSSL_=1 "

    # Only include DRAMSim if available
    if "DRAMSIMPATH" in os.environ:
        DRAMSIMPATH = os.environ["DRAMSIMPATH"]
        env["LINKFLAGS"] += " -Wl,-R" + DRAMSIMPATH
        env["PINLIBPATH"] += [DRAMSIMPATH]
        env["CPPPATH"] += [DRAMSIMPATH]
        env["PINLIBS"] += ["dramsim"]
        env["CPPFLAGS"] += " -D_WITH_DRAMSIM_=1 "

    env["CPPPATH"] += ["."]

    # HDF5
    env["PINLIBS"] += ["hdf5", "hdf5_hl"]

    # Harness needs these defined
    env["CPPFLAGS"] += ' -DPIN_PATH="' + joinpath(PINPATH, "intel64/bin/pinbin") + '" '
    env["CPPFLAGS"] += ' -DZSIM_PATH="' + joinpath(ROOT, joinpath(buildDir, "libzsim.so")) + '" '

    # Do PGO?
    if pgo == "generate":
        genFlags = " -prof-gen " if useIcc else " -fprofile-generate "
        env["PINCPPFLAGS"] += genFlags
        env["PINLINKFLAGS"] += genFlags
    elif pgo == "use":
        if useIcc: useFlags = " -prof-use "
        else: useFlags = " -fprofile-use -fprofile-correction "
        # even single-threaded sims use internal threads, so we need correction
        env["PINCPPFLAGS"] += useFlags
        env["PINLINKFLAGS"] += useFlags

    env.SConscript("src/SConscript", variant_dir=buildDir, exports= {'env' : env.Clone()})

####

AddOption('--buildDir', dest='buildDir', type='string', default="build/", nargs=1, action='store', metavar='DIR', help='Base build directory')
AddOption('--d', dest='debugBuild', default=False, action='store_true', help='Do a debug build')
AddOption('--o', dest='optBuild', default=False, action='store_true', help='Do an opt build (optimized, with assertions and symbols)')
AddOption('--r', dest='releaseBuild', default=False, action='store_true', help='Do a release build (optimized, no assertions, no symbols)')
AddOption('--p', dest='pgoBuild', default=False, action='store_true', help='Enable PGO')
AddOption('--pgoPhase', dest='pgoPhase', default="none", action='store', help='PGO phase (just run with --p to do them all)')


baseBuildDir = GetOption('buildDir')
buildTypes = []
if GetOption('debugBuild'): buildTypes.append("debug")
if GetOption('releaseBuild'): buildTypes.append("release")
if GetOption('optBuild') or len(buildTypes) == 0: buildTypes.append("opt")

march = "core2" # ensure compatibility across condor nodes
#march = "native" # for profiling runs

buildFlags = {"debug": "-g -O0",
              "opt": "-march=%s -g -O3 -funroll-loops" % march, # unroll loops tends to help in zsim, but in general it can cause slowdown
              "release": "-march=%s -O3 -DNASSERT -funroll-loops -fweb" % march} # fweb saves ~4% exec time, but makes debugging a world of pain, so careful

pgoPhase = GetOption('pgoPhase')

# The PGO flow calls scons recursively. Hacky, but pretty much the only option:
# scons can't build the same file twice, and although gcc enables you to change
# the fprofile path, it considers the whole relative path as the filename
# (e.g., build/opt/zsim.os), and all hell breaks loose when it tries to create
# files in another dir. And because it uses checksums for filenames, it breaks
# when you move the files. Check the repo for a version that tries this.
if GetOption('pgoBuild'):
    for type in buildTypes:
        print "Building PGO binary"
        root = Dir('.').abspath
        testsDir = joinpath(root, "tests")
        trainCfgs = [f for f in os.listdir(testsDir) if f.startswith("pgo")]
        print "Using training configs", trainCfgs

        baseDir = joinpath(baseBuildDir, "pgo-" + type)
        genCmd = "scons -j16 --pgoPhase=generate-" + type
        runCmds = []
        for cfg in trainCfgs:
            runCmd = "mkdir -p pgo-tmp && cd pgo-tmp && ../" + baseDir + "/zsim ../tests/" + cfg + " && cd .."
            runCmds.append(runCmd)
        useCmd = "scons -j16 --pgoPhase=use-" + type
        Environment(ENV = os.environ).Command("dummyTgt-" + type, [], " && ".join([genCmd] + runCmds + [useCmd]))
elif pgoPhase.startswith("generate"):
    type = pgoPhase.split("-")[1]
    buildSim(buildFlags[type], baseBuildDir, "pgo-" + type, "generate")
elif pgoPhase.startswith("use"):
    type = pgoPhase.split("-")[1]
    buildSim(buildFlags[type], baseBuildDir, "pgo-" + type, "use")
    baseDir = joinpath(baseBuildDir, "pgo-" + type)
    Depends(Glob(joinpath(baseDir, "*.os")), "pgo-tmp/zsim.out") #force a rebuild
else:
    for type in buildTypes:
        buildSim(buildFlags[type], baseBuildDir, type)
