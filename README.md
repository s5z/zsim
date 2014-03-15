zsim
====

zsim is a fast x86-64 simulator. It was originally written to evaluate ZCache
(Sanchez and Kozyrakis, MICRO-44, Dec 2010), hence the name, but it has since
outgrown its purpose.
zsim's main goals are to be fast, simple, and accurate, with a focus on
simulating memory hierarchies and large, heterogeneous systems. It is parallel
and uses DBT extensively, resulting in speeds of hundreds of millions of
instructions/second in a modern multicore host. Unlike conventional simulators,
zsim is organized to scale well (almost linearly) with simulated core count.

You can find more details about zsim in our ISCA 2013 paper:
http://people.csail.mit.edu/sanchez/papers/2013.zsim.isca.pdf.


License & Copyright
-------------------

zsim is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
version 2.

zsim was originally written by Daniel Sanchez at Stanford University, and per
Stanford University policy, the copyright of this original code remains with
Stanford (specifically, the Board of Trustees of Leland Stanford Junior
University). Since then, zsim has been substantially modified and enhanced at
MIT by Daniel Sanchez, Nathan Beckmann, and Harshad Kasture. zsim also
incorporates contributions on main memory performance models from Krishna
Malladi, Makoto Takami, and Kenta Yasufuku.

zsim was also modified and enhanced while Daniel Sanchez was an intern at
Google. Google graciously agreed to share these modifications under a GPLv2
license. This code is (C) 2011 Google Inc. Files containing code developed at
Google have a different license header with the correct copyright attribution.

Additionally, if you use this software in your research, we request that you
reference the zsim paper ("ZSim: Fast and Accurate Microarchitectural
Simulation of Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June
2013) as the source of the simulator in any publications that use this
software, and that you send us a citation of your work.


Setup
-----

External dependencies: `gcc >=4.6, pin, scons, libconfig, libhdf5, libelfg0`

**Natively:** If you use a relatively recent Linux distribution:

1. Clone a fresh copy of the git zsim repository (`git clone <path to zsim repo>`).

2. Download Pin, http://www.pintool.org . Tested with Pin 2.8+ on an x86-64
   architecture. Compiler flags are set up for Pin 2.9 on x86-64. To get flags
   for other versions, examine the Pin makefile or derive from sample pintools.
   Set the PINPATH environment variable to Pin's base directory.

   NOTE: Linux 3.0+ systems require Pin 2.10+, just because Pin does a kernel
   version check that 3.0 fails. Use Pin 2.12 with Sandy/Ivy Bridge systems,
   earlier Pin versions have strange performance regressions on this machine
   (extremely low IPC).

3. zsim requires some additional libraries. If they are not installed in your
   system, you will need to download and build them:

  3.1 libconfig, http://www.hyperrealm.com/libconfig. You may use the system's
      package if it's recent enough, or build your own. To install locally, untar,
      run `./configure --prefix=<libconfig install path> && make install`.  Then
      define the env var `LIBCONFIGPATH=<libconfig install path>`.

  3.2 libhdf5, http://www.hdfgroup.org (v1.8.4 path 1 or higher), and libelfg0.
      The SConstruct file assumes these are installed in the system.

  3.3 (OPTIONAL) polarssl (currently used just for their SHA-1 hash function),
      http://www.polarssl.org Install locally as in 3.1 and define the env var
      `POLARSSLPATH=<polarssl install path>`.
      
      NOTE: You may need to add `-fPIC` to the Makefile's C(PP/XX)FLAGS depending
      on the version.

  3.4 (OPTIONAL) DRAMSim2 for main memory simulation. Build locally and define
      the env var DRAMSIMPATH as in 3.1 and 3.3.

4. In some distributions you may need to make minor changes to the host
   configuration to support large shmem segments and ptrace. See the notes
   below for more details.

5. Compile zsim: `scons -j16`

6. Launch a test run: `./build/opt/zsim tests/simple.cfg`

For more compilation options, run scons --help. You can build debug, optimized
and release variants of the simulator (--d, --o, --r options). Optimized (opt)
is the default. You can build profile-guided optimized (PGO) versions of the
code with --p. These improve simulation performance with OOO cores by about
30%.

NOTE: zsim uses C++11 features available in `gcc >=4.6` (such as range-based for
loops, strictly typed enums, lambdas, and type inference). Older version of gcc
will not work. zsim can also be built with `icc` (see the `SConstruct` file).

**Using a virtual machine:** If you use another OS, can't make system-wide
configuration changes, or just want to test zsim without modifying your system,
you can run zsim on a Linux VM. We have included a vagrant configuration file
(http://vagrantup.com) that will provision an Ubuntu 12.04 VM to run zsim.
You can also follow this Vagrantfile to figure out how to setup zsim on an
Ubuntu system. Note that **zsim will be much slower on a VM** because it relies
on fast context-switching, so we don't recommend this for purposes other than
testing and development. Assuming you have vagrant installed (`sudo apt-get
install vagrant` on Ubuntu or Debian), follow these:

1. Copy `misc/Vagrantfile` from the repo into an empty folder.

2. Run `vagrant up` to set up the base VM and install all dependencies.

3. SSH into the VM with `vagrant ssh`.

4. Inside the VM, you can clone the zsim repo, and build it and use it as usual
   (steps 1, 5, and 6 above).


Notes
-----

**Accuracy & validation:** While we have validated zsim against a real system,
you should be aware that we sometimes sacrifice some accuracy for speed and
simplicity. The ISCA 2013 paper details the possible sources of inaccuracy.
Despite our validation efforts, if you are using zsim with workloads or
architectures that are significantly different from ours, you should not
blindly trust these results. Also, zsim can be configured with varying degrees
of accuracy, which may be OK in some cases but not others (e.g., longer bound
phases to reduce overheads are often OK if your application has little
communication, but not with fine-grained parallelism and synchronization).
Finally, in some cases you will need to modify the code to model what you want,
and for some purposes, zsim is just not the right tool. In any case, we
strongly recommend validating your baseline configuration and workloads against
a real machine.

In addition to the results in the zsim paper,
http://zsim.csail.mit.edu/validation has updated validation results.

**Memory Management:** zsim can simulate multiple processes, which introduces some
complexities in memory management. Each Pin process uses SysV IPC shared
memory to communicate through a global heap. Be aware that Pin processes have a
global and a process-local heap, and all simulator objects should be allocated
in the global heap. A global heap allocator is implemented (galloc.c and g\_heap
folder) using Doug Lea's malloc. The global heap allocator functions are as the
usual ones, with the gm\_ prefix (e.g. gm\_malloc, gm\_calloc, gm\_free). Objects
can be allocated in the global heap automatically by making them inherit from
GlobAlloc, which redefines the new and delete operators. STL classes use their
own internal allocators, so they cannot be members of globally visible objects.
To ease this, the g\_stl folder has template specializations of commonly used
STL classes that are changed to use our own STL-compliant allocator that
allocates from the global heap. Use these classes as drop-in replacements when
you need a globally visible STL class, e.g. substitute std::vector with
g\_vector, etc.

**Harness:** While most of zsim is implemented as a pintool (`libzsim.so`), a harness
process (`zsim`) is used to control the simulation: set up the shared memory
segment, launch pin processes, check for deadlock, and ensure termination of
the whole process tree when it is killed. In prior revisions of the simulator,
you could launch the pintool directly, but now you should use the harness.

**Transparency & I/O:** To maintain transparency w.r.t instrumented
applications, zsim does all logging through info/warn/panic methods. With the
sim.logToFile option, these dump to per-process log files instead of the
console. *You should never use cout/cerr or printf in simulator code* ---
simple applications will work, but more complex setups, e.g., anything that
uses pipes, will break.

**Interfacing with applications:** You can use special instruction sequences to
control the simulation from the application (e.g., fast-forward to the region
you want to simulate). `misc/hooks` has wrappers for C/C++, Fortran, and Java,
and extending this to other languages should be easy.

**Host Configuration:** The system configuration may need some tweaks to support
zsim. First, it needs to allow for large shared memory segments. Second, for
Pin to work, it must allow a process to attach to any other from the user, not
just to a child. Use sysctl to ensure that `kernel.shmmax=1073741824` (or larger)
and `kernel.yama.ptrace_scope=0`. zsim has mainly been used in
Ubuntu 11.10, 12.04, 12.10, 13.04, and 13.10, but it should work in other Linux
distributions. Using it in OSs other than Linux (e.g,, OS X, Windows) will be
non-trivial, since the user-level virtualization subsystem has deep ties into
the Linux syscall interface.

**Stats:** The simulator outputs periodic, eventual and end-of-sim stats files.
Stats can be output in both HDF5 and plain text. Read the README.stats file
and the associated scripts repository to see how to use these stats.

**Configuration & Getting Started:** A detailed use guide is out of the scope of
this README, because the simulator options change fairly often. In general,
*the documentation is the source code*. You should be willing to occasionally
read the source code to see how different zsim features work. To get familiar
with the way to configure the simulator, the following three steps usually work
well when getting started:

1. Check the examples in the `tests/` folder, play around with the settings, and
   launch a few runs. Config files have three sections, sys (configures the
   simulated system, e.g., core and cache parameters), sim (configures simulation
   parameters, e.g., how frequent are periodic stats output, phase length, etc.),
   and process{0, 1, 2, ...} entries (what processes to run). 

2. Most parameters have implicit defaults. zsim produces an out.cfg file that
   includes all the default choices (and we recommend that your analysis scripts
   automatically parse this file to check that what you are simulating makes
   sense). Inspecting the out.cfg file reveals more configuration options to play
   with, as well as their defaults.

3. Finally, check the source code for more info on options. The whole system is
   configured in the init.cpp (sys and sim sections) and process\_tree.cpp
   (processX sections) files, so there is no need to grok the whole simulator
   source to find out all the configuration options.

**Hacking & Style Guidelines:** zsim is mostly consistent with Google's C++ style
guide. You can use cpplint.py to check rule violations. We depart from these
guidelines on a couple of aspects:

- 4-space indentation instead of 2 spaces

- 120-character lines instead of 80-char (though you'll see a clear disregard
  for strict line length limits every now and then)

You can use cpplint.py (included in misc/ with slight modifications) to check
your changes. misc/ also has a script to tidy includes, which should be in
alphabetical order within each type (own, system, and project headers).

vim will indent the code just fine with the following options:
`set cindent shiftwidth=4 expandtab smarttab`

Finally, as Google's style guidelines say, please be consistent with the
current style used elsewhere. For example, the parts of code that deal with Pin
follow a style consistent with pintools.

Happy hacking, and hope you find zsim useful!

