#!/usr/bin/python

# Copyright (C) 2013-2014 by Massachusetts Institute of Technology
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
 

import os, sys

#dryRun = True
dryRun = False

srcs = sys.argv[1:]

def sortIncludes(lines, fname):
    def prefix(l):
        if l.find("<") >= 0:
            return "2"
            # if you want to differentiate...
            #if l.find(".h") >= 0: return "2" # C system headers
            #else: return "3" # C++ system headers
        else:
            if l.find('"' + fname + '.') >= 0: return "1" # Our own header
            return "4" # Program headers

    ll = [prefix(l) + l.strip() for l in lines if len(l.strip()) > 0]
    sl = [l[1:] for l in sorted(ll)]
    if lines[-1].strip() == "": sl += [""]
    #print sl
    return sl

for src in srcs:
    f = open(src, 'r+')  # we open for read/write here to fail early on read-only files
    txt = f.read()
    f.close()

    bName = os.path.basename(src).split(".")[0]
    print bName

    lines = [l for l in txt.split("\n")]
   
    includeBlocks = []
    blockStart = -1
    for i in range(len(lines)):
        l = lines[i].strip()
        isInclude = l.startswith("#include") and l.find("NOLINT") == -1
        isEmpty = l == ""
        if blockStart == -1:
            if isInclude: blockStart = i  # start block
        else:
            if not (isInclude or isEmpty): # close block
                includeBlocks.append((blockStart, i))
                blockStart = -1

    print src, len(includeBlocks), "blocks"

    newIncludes = [(s , e, sortIncludes(lines[s:e], bName)) for (s, e) in includeBlocks]
    for (s , e, ii) in newIncludes:
        # Print?
        if ii == lines[s:e]:
            print "Block in lines %d-%d matches" % (s, e-1)
            continue
        for i in range(s, e):
            print "%3d: %s%s | %s" % (i, lines[i], " "*(40 - len(lines[i][:39])), ii[i-s] if i-s < len(ii) else "")
        print ""
    
    prevIdx = 0
    newLines = []
    for (s , e, ii) in newIncludes:
        newLines += lines[prevIdx:s] + ii
        prevIdx = e
    newLines += lines[prevIdx:]

    if not dryRun and len(includeBlocks):
        outTxt = "\n".join(newLines)
        f = open(src, 'w')
        f.write(outTxt)
        f.close()

print "Done!"
