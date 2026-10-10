"""pptrace.py <port> <nhits> <hex[:regs]>... : PPSSPP exec breakpoints; print each hit in order.
regs per address (default a0,a1,a2,ra), e.g. 0887A814:a0,a1,ra. Resumes after each hit; removes all at the end."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gelist, struct
port, n = int(sys.argv[1]), int(sys.argv[2]); bps = {}
for a in sys.argv[3:]:
    addr, _, regs = a.partition(':'); bps[int(addr, 16)] = (regs or 'a0,a1,a2,ra').split(',')
m = gelist.PpMem(port)
def reg(r): return m.call({'event': 'cpu.getReg', 'name': r}, 'cpu.getReg')['uintValue']
def rd32(a):
    import base64
    r = m.call({'event': 'memory.read', 'address': a, 'size': 4}, 'memory.read'); return struct.unpack('<I', base64.b64decode(r['base64']))[0]
for a in bps: m.call({'event': 'cpu.breakpoint.add', 'address': a, 'enabled': True}, 'cpu.breakpoint.add')
try:
    for k in range(n):
        m.recv(('cpu.stepping',)); pc = reg('pc'); out = []
        for r in bps.get(pc, ['ra']):
            if r.startswith('*'):  # *a0+4 = word at a0+4
                base, _, off = r[1:].partition('+'); v = rd32(reg(base) + int(off or '0', 16)); out.append('%s=%08X' % (r, v))
            else: out.append('%s=%08X' % (r, reg(r)))
        print('%08X ' % pc + ' '.join(out), flush=True)
        m.call({'event': 'cpu.resume'}, 'cpu.resume')
finally:
    for a in bps: m.call({'event': 'cpu.breakpoint.remove', 'address': a}, 'cpu.breakpoint.remove')
    m.call({'event': 'cpu.resume'}, 'cpu.resume')
