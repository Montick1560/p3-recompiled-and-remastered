#!/usr/bin/env python3
"""Join the network event logs of two infrastructure-mode instances (PSPRECOMP_INFRA_STATS) and, optionally, their
frame logs (PSPRECOMP_FRAMELOG), into delivery delays per hop and a list of the long frames.

Both instances run on one PC, so their steady_clock microseconds (QueryPerformanceCounter) are one clock. A frame
log's t_ms = 0 is placed on that clock by the "[FRAMELOG] start steady_us=N" line of the instance's stderr log.

Each tx_call on one side is matched, in order, to the rx and then the pop on the other side with the same
(kind, src, dst, len, hash). Per direction and kind it reports count, lost/dropped/unread and p50/p95/p99/max of
  send_block = tx_done - tx_call   (the guest thread waiting for the network)
  wire       = rx - tx_call        (until the peer's reader has it)
  queue      = pop - rx            (waiting for the game to read it)
  total      = pop - tx_call
Per instance: frames over 100 ms, each with the wire/queue maxima and the longest gap between arrivals of the
2 s before it, and (--driver, build/infra/two.py's driver.csv) what the test driver was doing to that instance
during the frame: debug-socket presses, shots, marks, or the 4 s beat_sync polling before a drum command. When a frame log has marks (debug socket "M", F8), only the frames between its first and last mark
(or after the only one) count, and so do the datagrams sent in that span.

    python -I tools/infra_lagjoin.py A_net.csv B_net.csv [--frames A_frames.csv:A.log B_frames.csv:B.log]
        [--driver driver.csv] [--md OUT]
    python -I tools/infra_lagjoin.py --selftest
"""
import argparse
import collections
import os
import re
import sys
import tempfile

KINDS = {2: 'dgram', 3: 'stream'}
STALL_MS = 100.0
WINDOW_US = 2_000_000
SYNC_US = 4_200_000  # gfx_tour.beat_sync polls the beat level over the debug socket for 4 s


def read_driver(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        f.readline()
        for line in f:
            c = line.strip().split(',')
            if len(c) == 3:
                rows.append((int(c[0]), c[1], c[2]))
    return rows


def driver_during(driver, side, t_end, dt_ms):
    """What the driver did to side during the frame ending at t_end: events at a time, a beat_sync window
    ('sync', SYNC_US long) or a disk stall of the PC ('*' target, 'disk_stall:<ms>' long) overlapping it."""
    lo = t_end - int(dt_ms * 1000) - 50_000
    seen = []
    for t, target, ev in driver:
        if target not in (side, '*'):
            continue
        name, _, ms = ev.partition(':')
        span = SYNC_US if name == 'sync' else int(ms) * 1000 if ms.isdigit() else 0
        if t <= t_end and t + span >= lo and name not in seen:
            seen.append(name)
    return seen


def read_net(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        header = f.readline().strip().split(',')
        if header[:2] != ['t_us', 'ev']:
            raise SystemExit(f'{path}: not a PSPRECOMP_INFRA_STATS file')
        for line in f:
            c = line.strip().split(',')
            if len(c) != 9:
                continue  # a killed instance leaves a partial last line
            rows.append((int(c[0]), c[1], int(c[2]), int(c[3], 16), int(c[4]), int(c[5]), int(c[6]), c[7],
                         int(c[8])))
    return rows


def read_frames(spec):
    """'frames.csv:instance.log' -> (abs frame rows [(t_end_us, dt_ms, mark)], window (lo_us, hi_us))."""
    parts = re.split(r':(?![/\\])', spec)
    if len(parts) != 2:
        raise SystemExit(f'--frames {spec}: expected FRAMES.csv:LOG')
    frames_path, log_path = parts
    start = None
    with open(log_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'\[FRAMELOG\] start steady_us=(-?\d+)', line)
            if m:
                start = int(m.group(1))
    if start is None:
        raise SystemExit(f'{log_path}: no "[FRAMELOG] start steady_us=" line')
    rows = []
    with open(frames_path, encoding='utf-8') as f:
        f.readline()
        for line in f:
            c = line.strip().split(',')
            if len(c) < 11:
                continue
            rows.append((start + int(float(c[0]) * 1000), float(c[1]), c[10] == '1'))
    marks = [t for t, _, m in rows if m]
    if len(marks) >= 2:
        window = (marks[0], marks[-1])
    elif marks:
        window = (marks[0], rows[-1][0])
    else:
        window = (rows[0][0] if rows else 0, rows[-1][0] if rows else 0)
    return rows, window


class Rec:
    __slots__ = ('kind', 't_call', 't_done', 't_rx', 't_pop', 'dropped')

    def __init__(self, kind, t_call):
        self.kind, self.t_call = kind, t_call
        self.t_done = self.t_rx = self.t_pop = None
        self.dropped = False


def join(net):
    """net = {'A': rows, 'B': rows} -> {(src_side, kind): [Rec]} and the count of rx with no tx."""
    sides = list(net)
    other = {sides[0]: sides[1], sides[1]: sides[0]}
    order = {'tx_call': 0, 'tx_done': 1, 'drop': 2, 'rx': 3, 'pop': 4}
    events = sorted(((r[0], order.get(r[1], 9), side, r) for side in sides for r in net[side]),
                    key=lambda e: (e[0], e[1]))
    awaiting_done = collections.defaultdict(collections.deque)  # (side, key) -> sent, no tx_done yet
    on_wire = collections.defaultdict(collections.deque)  # (sender, key) -> sent, not received
    in_queue = collections.defaultdict(collections.deque)  # (receiver, key) -> received, not read
    out = collections.defaultdict(list)
    stray_rx = 0
    for _, _, side, (t, ev, kind, _vip, src, dst, ln, h, _extra) in events:
        key = (kind, src, dst, ln, h)
        if ev == 'tx_call':
            rec = Rec(kind, t)
            out[(side, kind)].append(rec)
            awaiting_done[(side, key)].append(rec)
            on_wire[(side, key)].append(rec)
        elif ev == 'tx_done':
            q = awaiting_done[(side, key)]
            if q:
                q.popleft().t_done = t
        elif ev == 'rx':
            q = on_wire[(other[side], key)]
            if q:
                rec = q.popleft()
                rec.t_rx = t
                in_queue[(side, key)].append(rec)
            else:
                stray_rx += 1
        elif ev == 'pop':
            q = in_queue[(side, key)]
            if q:
                q.popleft().t_pop = t
        elif ev == 'drop':
            q = in_queue[(side, key)]
            if q:  # a received datagram the game never got
                q.popleft().dropped = True
                continue
            pend = [r for r in awaiting_done[(side, key)] if r.t_done is None]
            if pend:  # dropped from the outbox before it was sent
                rec = pend[0]
                rec.dropped = True
                awaiting_done[(side, key)].remove(rec)
                if rec in on_wire[(side, key)]:
                    on_wire[(side, key)].remove(rec)
    return out, stray_rx


def pct(values, p):
    if not values:
        return None
    v = sorted(values)
    k = max(0, min(len(v) - 1, -(-p * len(v) // 100) - 1))  # nearest rank
    return v[k]


def fmt_ms(us):
    return '-' if us is None else f'{us / 1000:.1f}'


def metric(recs, a, b):
    return [getattr(r, b) - getattr(r, a) for r in recs if getattr(r, a) is not None and getattr(r, b) is not None]


def report(net, frames=None, driver=None):
    """Returns (markdown lines, verdicts [(name, ok)], numbers dict for tests)."""
    recs, stray = join(net)
    sides = list(net)
    window = None
    if frames:
        lo = min(w[0] for _, w in frames.values())
        hi = max(w[1] for _, w in frames.values())
        window = (lo, hi)
    lines = ['# Infrastructure delivery report', '']
    if window:
        lines.append(f'Window: {(window[1] - window[0]) / 1e6:.1f} s (frame-log marks or the whole log).')
        lines.append('')
    lines += ['| dir | kind | sent | lost | dropped | unread | send_block p50/p95/p99/max | wire p50/p95/p99/max '
              '| queue p50/p95/p99/max | total p50/p95/p99/max |',
              '|---|---|---|---|---|---|---|---|---|---|']
    numbers = {}
    verdicts = []
    for s in sides:
        d = f'{s}->{[x for x in sides if x != s][0]}'
        for kind in (2, 3):
            rs = [r for r in recs.get((s, kind), []) if not window or window[0] <= r.t_call <= window[1]]
            if not rs:
                continue
            lost = sum(1 for r in rs if r.t_rx is None and not r.dropped)
            dropped = sum(1 for r in rs if r.dropped)
            unread = sum(1 for r in rs if r.t_rx is not None and r.t_pop is None and not r.dropped)
            cols = []
            m = {}
            for name, a, b in (('send_block', 't_call', 't_done'), ('wire', 't_call', 't_rx'),
                               ('queue', 't_rx', 't_pop'), ('total', 't_call', 't_pop')):
                v = metric(rs, a, b)
                m[name] = {p: pct(v, p) for p in (50, 95, 99, 100)}
                cols.append('/'.join(fmt_ms(m[name][p]) for p in (50, 95, 99, 100)))
            numbers[(d, KINDS[kind])] = dict(m, sent=len(rs), lost=lost, dropped=dropped, unread=unread,
                                              matched=sum(1 for r in rs if r.t_rx is not None))
            lines.append(f'| {d} | {KINDS[kind]} | {len(rs)} | {lost} | {dropped} | {unread} | ' + ' | '.join(cols)
                         + ' |')
            if kind == 2:
                p99 = m['total'][99]
                qmax = m['queue'][100]
                verdicts.append((f'{d} dgram total p99 < 50 ms ({fmt_ms(p99)})', p99 is not None and p99 < 50_000))
                verdicts.append((f'{d} dgram queue max <= 200 ms ({fmt_ms(qmax)})',
                                 qmax is None or qmax <= 200_000))
    lines.append('')
    lines.append('All times in ms. lost = sent, never received; dropped = a queue dropped it; unread = received, '
                 'never read by the game.')
    if stray:
        lines.append(f'{stray} received datagrams matched no send.')
    if frames:
        for s in sides:
            if s not in frames:
                continue
            rows, w = frames[s]
            stalls = [(t, dt) for t, dt, _ in rows if w[0] <= t <= w[1] and dt > STALL_MS]
            numbers[('stalls', s)] = len(stalls)
            verdicts.append((f'{s} frames over {STALL_MS:.0f} ms <= 1 ({len(stalls)})', len(stalls) <= 1))
            lines += ['', f'## {s}: {len(stalls)} frames over {STALL_MS:.0f} ms', '']
            if not stalls:
                continue
            lines += ['| t (s) | dt ms | ' + ' | '.join(f'{d} wire max | {d} queue max' for d in
                                                         (f'{x}->{y}' for x in sides for y in sides if x != y))
                      + f' | longest gap into {s} | driver |', '|---|---|' + '---|---|' * 2 + '---|---|']
            busy = 0
            for t, dt in stalls:
                lo = t - WINDOW_US
                cells = []
                for x in sides:
                    rs = [r for k, v in recs.items() if k[0] == x for r in v]
                    wire = [r.t_rx - r.t_call for r in rs if r.t_rx is not None and lo <= r.t_rx <= t]
                    queue = [r.t_pop - r.t_rx for r in rs if r.t_pop is not None and lo <= r.t_pop <= t]
                    cells += [fmt_ms(max(wire)) if wire else '-', fmt_ms(max(queue)) if queue else '-']
                into = sorted(r.t_rx for k, v in recs.items() if k[0] != s for r in v
                              if r.t_rx is not None and lo <= r.t_rx <= t)
                gap = max((b - a for a, b in zip([lo] + into, into + [t])), default=None)
                act = driver_during(driver, s, t, dt) if driver is not None else []
                busy += bool(act)
                lines.append(f'| {(t - w[0]) / 1e6:.2f} | {dt:.0f} | ' + ' | '.join(cells) + f' | {fmt_ms(gap)} | '
                             + (' '.join(act) or '-') + ' |')
            numbers[('stalls_driver', s)] = busy
            if driver is not None:
                lines += ['', f'{busy} of the {len(stalls)} overlap test-driver activity on {s}.']
    lines += ['', '## Verdict', '']
    lines += [f'- {"PASS" if ok else "FAIL"}: {name}' for name, ok in verdicts]
    return lines, verdicts, numbers


def selftest():
    tmp = tempfile.mkdtemp(prefix='lagjoin_')
    hdr = 't_us,ev,kind,vip,src,dst,len,hash,extra_us\n'

    def row(t, ev, h, kind=2, src=3658, dst=3658, extra=0):
        return f'{t},{ev},{kind},0a500002,{src},{dst},4,{h},{extra}\n'

    a = hdr + ''.join(row(t, 'tx_call', h) + row(t + 100, 'tx_done', h)
                      for t, h in ((1000, '00000001'), (2000, '00000002'), (3000, '00000003')))
    # B: #2 never arrives; #1 waits 1000 us in the queue, #3 200 us.
    b = hdr + row(1500, 'rx', '00000001') + row(2500, 'pop', '00000001') + row(3700, 'rx', '00000003') + \
        row(3900, 'pop', '00000003')
    # Two identical payloads in flight are matched in order.
    a += row(5000, 'tx_call', 'aaaaaaaa') + row(5100, 'tx_call', 'aaaaaaaa')
    b += row(5300, 'rx', 'aaaaaaaa') + row(5400, 'rx', 'aaaaaaaa') + row(9000, 'pop', 'aaaaaaaa') + \
        row(9100, 'drop', 'aaaaaaaa')
    pa, pb = os.path.join(tmp, 'net_A.csv'), os.path.join(tmp, 'net_B.csv')
    open(pa, 'w').write(a)
    open(pb, 'w').write(b)
    lines, verdicts, n = report({'A': read_net(pa), 'B': read_net(pb)})
    d = n[('A->B', 'dgram')]
    assert d['sent'] == 5 and d['matched'] == 4 and d['lost'] == 1 and d['dropped'] == 1, d
    assert d['wire'][50] == 300 and d['wire'][100] == 700, d['wire']  # 500, 700, 300, 300
    assert d['queue'][100] == 3700 and d['send_block'][100] == 100, d
    assert sorted(metric([r for r in join({'A': read_net(pa), 'B': read_net(pb)})[0][('A', 2)]][:3], 't_rx',
                         't_pop')) == [200, 1000]
    # Frames: start anchor 0 on the shared clock, marks at 1 ms and 4 ms, a 150 ms frame at 3.0 ms.
    fa = os.path.join(tmp, 'frames_A.csv')
    open(fa, 'w').write('t_ms,dt_ms,ge_ms,tex_miss_ms,pack_decode_ms,swap_ms,cpu_wait_ms,draws,tex_misses,'
                        'tex_hash_mb,mark\n0.5,500,0,0,0,0,0,0,0,0,0\n1.0,16,0,0,0,0,0,0,0,0,1\n'
                        '3.0,150,0,0,0,0,0,0,0,0,0\n4.0,16,0,0,0,0,0,0,0,0,1\n6.0,300,0,0,0,0,0,0,0,0,0\n')
    la = os.path.join(tmp, 'A.log')
    open(la, 'w').write('[INFRA] x\n[FRAMELOG] start steady_us=0\n')
    fr = read_frames(f'{fa}:{la}')
    assert fr[1] == (1000, 4000), fr[1]
    lines, verdicts, n = report({'A': read_net(pa), 'B': read_net(pb)}, {'A': fr})
    assert n[('stalls', 'A')] == 1, n  # the 500 ms boot frame and the 300 ms frame are outside the marks
    assert n[('A->B', 'dgram')]['sent'] == 3, n  # sends inside the window only
    assert any(l.startswith('| 0.00 | 150 |') for l in lines), '\n'.join(lines)
    assert ('A frames over 100 ms <= 1 (1)', True) in verdicts, verdicts
    # Driver: a beat_sync that started 2 ms before the 150 ms frame overlaps it; a press 1 s later does not.
    drv = [(1000, 'A', 'sync'), (1_003_000, 'A', 'press'), (2900, 'B', 'shot'), (9_000_000, '*', 'disk_stall:500')]
    assert driver_during(drv, 'A', 3000, 150) == ['sync'], driver_during(drv, 'A', 3000, 150)
    assert driver_during(drv, 'A', 10_000_000, 30) == []
    assert driver_during(drv, 'B', 9_400_000, 30) == ['disk_stall'], driver_during(drv, 'B', 9_400_000, 30)
    lines, verdicts, n = report({'A': read_net(pa), 'B': read_net(pb)}, {'A': fr}, drv)
    assert n[('stalls_driver', 'A')] == 1 and any(l.endswith('| sync |') for l in lines), '\n'.join(lines)
    print('selftest OK')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('net', nargs='*', help='A_net.csv B_net.csv')
    ap.add_argument('--frames', nargs=2, metavar='FRAMES.csv:LOG', help='frame log and stderr log, A then B')
    ap.add_argument('--driver', help="build/infra/two.py's driver.csv")
    ap.add_argument('--md', help='also write the report here')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if len(args.net) != 2:
        ap.error('two network logs (A then B)')
    net = {'A': read_net(args.net[0]), 'B': read_net(args.net[1])}
    frames = {s: read_frames(f) for s, f in zip('AB', args.frames)} if args.frames else None
    lines, verdicts, _ = report(net, frames, read_driver(args.driver) if args.driver else None)
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if args.md:
        open(args.md, 'w', encoding='utf-8').write(text)


if __name__ == '__main__':
    main()
