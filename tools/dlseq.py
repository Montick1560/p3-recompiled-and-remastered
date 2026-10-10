# Run-length summary of [DL] prim lines from a log, from a start line.
# Usage: python -I dlseq.py <log> [start_line] [keys,comma,separated]
import sys, re
log = sys.argv[1]; start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
keys = (sys.argv[3] if len(sys.argv) > 3 else 'prim,n,vt,fb,clr,thr,tex,ta,tf,tsz,bw,cf,tfn,blend,bm,fa,fb2,at,zt,zw,lit,mat,amb').split(',')
prev = None; cnt = 0; first = 0
def flush():
    if prev is not None: print('%5d x%-4d %s' % (first, cnt, prev))
with open(log, encoding='utf-8', errors='replace') as f:
    for i, line in enumerate(f, 1):
        if i < start or not line.startswith('[DL] '): continue
        if line.startswith('[DL] ----'):
            flush(); prev = None; cnt = 0; print(line.strip()); continue
        if not line.startswith('[DL] prim='): continue
        kv = dict(re.findall(r'(\w+)=(\S+)', line))
        s = ' '.join('%s=%s' % (k, kv.get(k, '?')) for k in keys)
        if s == prev: cnt += 1
        else: flush(); prev = s; cnt = 1; first = i
flush()
