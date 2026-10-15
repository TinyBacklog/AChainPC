"""Generate N access sites across several C files, to check the exception
table at scale: every site must be found and recovered, whatever order the
linker puts the sections in. Usage: gen_sites.py OUT_DIR N FILES"""
import sys
from pathlib import Path

out, n, files = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
per = (n + files - 1) // files
for f in range(files):
    lo, hi = f * per, min(n, (f + 1) * per)
    lines = ['#include "fm2.h"']
    for i in range(lo, hi):
        op = ("fm2_load32(c,a)" if i % 2 == 0 else "(fm2_store64(c,a,%d), 0)" % i)
        # The last file's code goes to a .text.unlikely section, which the
        # linker places before plain .text, while its table entries still come
        # last: the table is then out of address order and has to be sorted.
        attr = '__attribute__((section(".text.unlikely.fm2poc"))) ' if f == files - 1 else ""
        lines.append("%suint64_t site_%d(Ctx* c, uint64_t a){ FM2_BLOCK_LIMIT; return %s + %d; }" % (attr, i, op, i))
    (out / f"sites_{f}.c").write_text("\n".join(lines) + "\n")
decls = "\n".join("uint64_t site_%d(Ctx*, uint64_t);" % i for i in range(n))
table = ",\n".join("site_%d" % i for i in range(n))
(out / "sites_table.c").write_text('#include "fm2.h"\n%s\nuint64_t (*const g_sites[%d])(Ctx*, uint64_t) = {\n%s\n};\nconst unsigned g_nsites = %d;\n'
                                   % (decls, n, table, n))
