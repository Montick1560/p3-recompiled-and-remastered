"""ppbt.py <port> <hexaddr> [a0_hex] [n]: PPSSPP exec breakpoint; on each hit (optionally only when
a0 == a0_hex) print a0-a3/ra and the guest backtrace. Waits indefinitely; flushes every hit."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gelist
port, addr = int(sys.argv[1]), int(sys.argv[2], 16)
want = int(sys.argv[3], 16) if len(sys.argv) > 3 and sys.argv[3] != '-' else None
n = int(sys.argv[4]) if len(sys.argv) > 4 else 1
m = gelist.PpMem(port)
def reg(r): return m.call({'event': 'cpu.getReg', 'name': r}, 'cpu.getReg')['uintValue']
m.call({'event': 'cpu.breakpoint.add', 'address': addr, 'enabled': True}, 'cpu.breakpoint.add')
try:
    got = 0
    while got < n:
        m.s.settimeout(None); m.recv(('cpu.stepping',)); m.s.settimeout(10)
        a0 = reg('a0')
        if want is None or a0 == want:
            got += 1
            print('hit %d: a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X' % (got, a0, reg('a1'), reg('a2'), reg('a3'), reg('ra')), flush=True)
            for f in m.call({'event': 'hle.backtrace'}, 'hle.backtrace')['frames'][:20]:
                print('   entry=%08X pc=%08X  %s' % (f['entry'], f['pc'], f.get('code', '')), flush=True)
        m.call({'event': 'cpu.resume'}, 'cpu.resume')
finally:
    m.call({'event': 'cpu.breakpoint.remove', 'address': addr}, 'cpu.breakpoint.remove')
    m.call({'event': 'cpu.resume'}, 'cpu.resume')
