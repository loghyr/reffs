import sys
sys.path.insert(0, "/home/loghyr/obj/ffv2-d1b-store-model/v3/mutants")
from _lib import patch
d = sys.argv[1]
import re
src = open(d + "/d1_journal.c").read()
m = re.search(r"\n(\t+)if \([^\n]*format[^\n]*\)\n\t+return[^\n]*\n", src)
if not m:
    sys.stderr.write("no format check found\n"); sys.exit(1)
patch(d + "/d1_journal.c", m.group(0), "\n")
