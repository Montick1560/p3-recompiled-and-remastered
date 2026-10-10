"""ppwatch.py <port> <hexaddr> [n]: PPSSPP write-breakpoint on <hexaddr>; print value written + guest backtrace n times."""
import sys, json
sys.path.insert(0, __import__('os').path.dirname(__file__))
import gelist
port, addr = int(sys.argv[1]), int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
m = gelist.PpMem(port)
def reg(r): return m.call({'event': 'cpu.getReg', 'name': r}, 'cpu.getReg')['uintValue']
m.call({'event': 'memory.breakpoint.add', 'address': addr, 'size': 4, 'enabled': True, 'read': False, 'write': True, 'change': False, 'log': False}, 'memory.breakpoint.add')
try:
    for k in range(n):
        m.recv(('cpu.stepping',))
        regs = {r: reg(r) for r in ('pc', 'ra', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 's0', 's1')}
        bt = m.call({'event': 'hle.backtrace'}, 'hle.backtrace')['frames']
        print('hit %d: ' % k + ' '.join('%s=%08X' % kv for kv in regs.items()))
        for f in bt[:24]: print('   entry=%08X pc=%08X sp=%08X  %s' % (f['entry'], f['pc'], f['sp'], f.get('code', '')))
        m.call({'event': 'cpu.resume'}, 'cpu.resume')
finally:
    m.call({'event': 'memory.breakpoint.remove', 'address': addr, 'size': 4}, 'memory.breakpoint.remove')
    m.call({'event': 'cpu.resume'}, 'cpu.resume')
