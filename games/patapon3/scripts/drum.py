# Play Patapon 3 drum commands locked to the game's beat over the debug socket.
"""drum.py <seq> [repeats] [offset_ms] [shift_beats]: play drum commands locked to the game's beat.

Syncs on the beat outline the game flashes at x=4 (debug-socket screenshots,
~32 ms each), fits period + phase, finds the measure downbeat (its flash is
the brightest), then presses buttons on predicted beats. One char per beat,
starting on a downbeat: P=pata(SQUARE) O=pon(CIRCLE) C=chaka(TRIANGLE)
D=don(CROSS) -=rest. Example: tutorial "---O"; march "PPPO----" (command +
4 beats while the army sings). offset_ms shifts the presses (default -20).
Screenshots go to $P3_SHOT, or `_beat.tga` in the current directory.
"""
import os, socket, struct, sys, time

SHOT = os.environ.get('P3_SHOT', '_beat.tga')
KEYS = {'P': 0x8000, 'O': 0x2000, 'C': 0x1000, 'D': 0x4000}


def send(line, tries=4):
    # Loading hitches can stall the socket for seconds: retry instead of dying.
    for i in range(tries):
        try:
            s = socket.create_connection(('127.0.0.1', 9999), timeout=20)
            s.sendall((line + '\n').encode())
            r = s.makefile('rb').readline()
            s.close()
            return r
        except OSError:
            if i == tries - 1:
                raise
            time.sleep(2)


def outline_level():
    send('S ' + SHOT)
    d = open(SHOT, 'rb').read()
    w, h = struct.unpack('<HH', d[12:16])
    bp, desc = d[16] // 8, d[17]
    n = 0
    for y in range(40, 240, 20):
        yy = y if desc & 0x20 else h - 1 - y
        o = 18 + (yy * w + 4) * bp
        if (d[o] + d[o + 1] + d[o + 2]) // 3 > 140:
            n += 1
    return n


def sync(seconds=4.0):
    """Return (period, t_downbeat) in perf_counter time, or None."""
    onsets, prev, last = [], 0, None
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        t = time.perf_counter()
        v = outline_level()
        # An onset right after a sampling stall (the game hitches ~250 ms
        # now and then) is late by an unknown amount: skip it.
        if v >= 5 and prev < 5 and last is not None and t - last < 0.06:
            onsets.append((t, v))
        prev, last = v, t
    if len(onsets) < 4:
        return None
    ts = [t for t, _ in onsets]
    # Robust phase on the nominal 0.5 s beat (120 BPM): late samples would
    # bend a least-squares period, so take the median residual and refit
    # the period from the inliers only.
    period = 0.5
    res = sorted(((t - ts[0] + period / 2) % period) - period / 2 for t in ts)
    med = res[len(res) // 2]
    a0 = ts[0] + med
    inl = [(round((t - a0) / period), t) for t in ts
           if abs(((t - a0 + period / 2) % period) - period / 2) < 0.06]
    if len(inl) >= 4:
        n = len(inl)
        mi = sum(i for i, _ in inl) / n
        mt = sum(t for _, t in inl) / n
        var = sum((i - mi) ** 2 for i, _ in inl)
        p2 = sum((i - mi) * (t - mt) for i, t in inl) / var if var else period
        if abs(p2 - 0.5) < 0.003:
            period = p2
        a = mt - period * mi
    else:
        a = a0
    idx = [round((t - a) / period) for t in ts]
    # Downbeat: the beat class (index mod 4) with the brightest flashes.
    score = [0.0] * 4
    cnt = [0] * 4
    for i, (_, v) in zip(idx, onsets):
        score[i % 4] += v
        cnt[i % 4] += 1
    best = max(range(4), key=lambda c: score[c] / cnt[c] if cnt[c] else 0)
    return period, a + best * period


def main():
    seq = sys.argv[1]
    reps = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    offset = (int(sys.argv[3]) if len(sys.argv) > 3 else -20) / 1000.0
    shift = int(sys.argv[4]) if len(sys.argv) > 4 else 0  # beats to add to the downbeat guess
    r = sync()
    if not r:
        print('no beat found')
        sys.exit(1)
    period, down = r
    print(f'period={period:.4f}')
    now = time.perf_counter()
    k = int((now - down) / period) + 1
    while (k - shift) % 4:
        k += 1  # start on the next downbeat
    for j, ch in enumerate(seq * reps):
        if ch not in KEYS:
            continue
        t = down + (k + j) * period + offset
        while time.perf_counter() < t:
            pass
        send(f'B {KEYS[ch]:X} 60')


if __name__ == '__main__':
    main()
