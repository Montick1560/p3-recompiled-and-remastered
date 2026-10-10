# Symbolize [BT] lines with llvm-nm.
# Usage: python tools/bt.py <log> [<exe>]
# Requires llvm-nm on PATH (the script runs `llvm-nm -C <exe>`).
"""bt.py <log> [<exe>]: symbolize [BT] host backtraces (image-relative) into guest function names.

Guest functions are host functions named FUN_xxxxxxxx (main) or ovBank::FUN_xxxxxxxx (overlay
banks); each frame maps to the nearest symbol at or below it (llvm-nm, image base 0x140000000).
The exe path is the second argument or the env var PSPRECOMP_EXE.
"""
import bisect, os, re, subprocess, sys

if len(sys.argv) < 2 or (len(sys.argv) < 3 and not os.environ.get('PSPRECOMP_EXE')):
    print('usage: python tools/bt.py <log> [<exe>]')
    print('exe is a required argument, or set PSPRECOMP_EXE')
    sys.exit(1)
log = sys.argv[1]
exe = sys.argv[2] if len(sys.argv) > 2 else os.environ['PSPRECOMP_EXE']
BASE = 0x140000000
syms = []
for line in subprocess.run(['llvm-nm', '-C', exe], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(None, 2)
    if len(parts) == 3 and parts[1] in 'tT':
        syms.append((int(parts[0], 16) - BASE, parts[2]))
syms.sort()
addrs = [a for a, _ in syms]


def name(off):
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0:
        return '?'
    n = syms[i][1]
    m = re.search(r'(?:(ov\w+)::)?(FUN_[0-9a-fA-F]{8}|func_[0-9A-F]{8}_entry|thunk_\w+|entry)', n)
    return (m.group(1) + ':' if m and m.group(1) else '') + m.group(2) if m else n.split('(')[0]


for line in open(log, encoding='utf-8', errors='replace'):
    if line.startswith('[BT]'):
        tag, _, frames = line.partition(':')
        names = [name(int(f[1:], 16)) for f in frames.split()]
        print(tag, ' <- '.join(names))
    elif line.startswith('[OVL-ARGS-IN]') or line.startswith('[FUNC-WATCH]'):
        print(line.split(' path:')[0].split('; this')[0].rstrip())
