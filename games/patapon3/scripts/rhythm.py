# Press one Patapon 3 drum button on a fixed period over the debug socket.
"""rhythm.py <mask hex> <period s> <count> [press ms]: press a button every <period> seconds (drum input)."""
import socket, sys, time
m, per, n = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
ms = int(sys.argv[4]) if len(sys.argv) > 4 else 60
t0 = time.perf_counter()
for i in range(n):
    while time.perf_counter() < t0 + i * per: pass
    s = socket.create_connection(('127.0.0.1', 9999), timeout=5); s.sendall(f'B {m} {ms}\n'.encode()); s.makefile('rb').readline(); s.close()
