"""Copy PPSSPP's Core/HLE/sceChnnlsv.cpp without its HLE wrappers (guest-pointer sceSd* entry
points, module tables, Register_*): only the savedata cipher stays.
Usage: python -I vendor_chnnlsv.py <ppsspp sceChnnlsv.cpp> <out .cpp>"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8", newline="").read().replace("\r\n", "\n")
old_inc = ('#include "Core/MemMapHelpers.h"\n#include "Core/HLE/HLE.h"\n'
           '#include "Core/HLE/FunctionWrappers.h"\n\n#include "Core/HLE/sceChnnlsv.h"\n'
           '#include "Core/HLE/sceKernel.h"\n')
new_inc = ('// psprecomp: HLE wrappers (guest-pointer sceSd* entry points, module tables, Register_*)\n'
           '// removed; only the savedata cipher remains. See third_party/kirk/README.md.\n'
           '#include <cstring>\n\n'
           '#include "Common/CommonTypes.h"\n'
           '#include "Common/Swap.h"\n'
           '#include "Core/HLE/sceChnnlsv.h"\n')
assert old_inc in s, "PPSSPP include block changed"
s = s.replace(old_inc, new_inc)
s, n = re.subn(r"static int sceSd\w+\(u32[^\n]*\{\n(?:.*\n)*?\}\n\n", "", s)
assert n == 6, f"expected 6 guest-pointer wrappers, removed {n}"
s = s[:s.index("const HLEFunction sceChnnlsv[]")].rstrip("\n") + "\n"
for bad in ("Memory::", "hleLog", "PSPPointer", "RegisterHLEModule", "_LOG("):
    assert bad not in s, bad
open(dst, "w", encoding="utf-8", newline="\n").write(s)
print(f"{dst}: {s.count(chr(10))} lines")
