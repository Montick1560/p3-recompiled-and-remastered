"""ppops.py <port> <bp_hex> <n> [press_button] [pre_button]: in PPSSPP, optionally tap pre_button
(e.g. circle) and wait, arm an exec breakpoint, tap press_button (e.g. square) and log n hits:
a0, a1 and the first 4 words at a0 (script-op tracing). Button names: PPSSPP input.buttons names."""
import os, sys, time, base64, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gelist
port, bp, n = int(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3])
press = sys.argv[4] if len(sys.argv) > 4 else None
pre = sys.argv[5] if len(sys.argv) > 5 else None
m = gelist.PpMem(port)
def reg(r): return m.call({'event': 'cpu.getReg', 'name': r}, 'cpu.getReg')['uintValue']
def words(a, k=4):
    r = m.call({'event': 'memory.read', 'address': a, 'size': 4 * k}, 'memory.read')
    return struct.unpack('<%dI' % k, base64.b64decode(r['base64']))
def tap(b):
    # No wait for the reply: PPSSPP answers when the press ends, which never
    # happens while a breakpoint holds the CPU.
    m.send({'event': 'input.buttons.press', 'button': b, 'duration': 6})
if pre:
    tap(pre); time.sleep(2.0)
m.call({'event': 'cpu.breakpoint.add', 'address': bp, 'enabled': True}, 'cpu.breakpoint.add')
try:
    if press: tap(press)
    for k in range(n):
        m.s.settimeout(30); m.recv(('cpu.stepping',))
        a0, a1 = reg('a0'), reg('a1')
        if os.environ.get('PPOPS_REGS'):
            print('a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X' % (a0, a1, reg('a2'), reg('a3'), reg('ra')), flush=True)
        else:
            w = words(a0)
            print('a0=%08X a1=%08X op=%04X  %s' % (a0, a1, (w[0] >> 16) & 0xFFFF, ' '.join('%08X' % x for x in w)), flush=True)
        m.call({'event': 'cpu.resume'}, 'cpu.resume')
finally:
    m.call({'event': 'cpu.breakpoint.remove', 'address': bp}, 'cpu.breakpoint.remove')
    try: m.call({'event': 'cpu.resume'}, 'cpu.resume')
    except Exception: pass
