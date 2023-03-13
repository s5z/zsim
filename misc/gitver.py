# Return a pretty-printed short git version (like hg/svnversion)
import os
def cmd(c): return os.popen(c).read().strip()
branch = cmd("git rev-parse --abbrev-ref HEAD")
revnum = cmd("git log | grep ^commit | wc -l")
rshort = cmd("git rev-parse --short HEAD")
dfstat = cmd("git diff HEAD --shortstat")
dfhash = cmd("git diff HEAD | md5sum")[:8]
shstat = dfstat.replace(" files changed", "fc").replace(" file changed", "fc") \
               .replace(" insertions(+)", "+").replace(" insertion(+)", "+") \
               .replace(" deletions(-)", "-").replace(" deletion(-)", "-") \
               .replace(",", "")
diff = "clean" if len(dfstat) == 0 else shstat +  " " + dfhash
print(":".join([branch, revnum, rshort, diff]))
