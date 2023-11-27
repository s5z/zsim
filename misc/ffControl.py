#!/usr/bin/python

# Copyright (C) 2013-2015 by Massachusetts Institute of Technology
#
# This file is part of zsim.
#
# zsim is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, version 2.
#
# If you use this software in your research, we request that you reference
# the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
# Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
# source of the simulator in any publications that use this software, and that
# you send us a citation of your work.
#
# zsim is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <http://www.gnu.org/licenses/>.

import os, sys, subprocess
from optparse import OptionParser

parser = OptionParser()
parser.add_option("--procIdx", type="int", default=0, dest="procIdx", help="Process index to signal")
parser.add_option("--lineMatch", default=" ROI", dest="lineMatch", help="Matching line to stdin will trigger signal")
parser.add_option("--maxMatches", type="int", default=0, dest="maxMatches", help="Exit after this many matches (0 to disable)")
parser.add_option("--fftogglePath", default="./build/opt", dest="fftogglePath", help="")
(opts, args) = parser.parse_args()

targetShmid = -1
matches = 0
while matches < opts.maxMatches or opts.maxMatches <= 0:
    try:
        line = sys.stdin.readline()
    except:
        print("stdin done, exiting")
        break

    if line.startswith("[H] Global segment shmid = "):
        targetShmid = int(line.split("=")[1].lstrip().rstrip())
        print("Target shmid is", targetShmid)

    if line.find(opts.lineMatch) >= 0:
        if targetShmid >= 0:
            print("Match, calling fftoggle")
            matches += 1
            subprocess.call([os.path.join(opts.fftogglePath, "fftoggle"), str(targetShmid), str(opts.procIdx)])
        else:
            print("Match but shmid is not valid, not sending signal (are you sure you specified procIdx correctly? it's not the PID)")
print("Done, %d matches" % matches)

