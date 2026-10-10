# Adaptive Patapon 3 prologue driver: drum sequences on the beat, CROSS in dialogue.
"""autoplay.py <shots dir> <seq> <minutes> [reps] [seq2]: adaptive prologue driver.

Loop until <minutes> pass: when the beat outline flashes (drum.sync), play <seq>
(drum.py syntax) <reps> times locked to it; otherwise the game is in a dialogue,
so press CROSS. Saves a screenshot A<n>.tga after each round. With <seq2>, switch to it
once a mostly-blue screen shows up (the drum-gift button diagram ends the Hatapon tutorial).
"""
import os, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drum

D = sys.argv[1].rstrip('/') + '/'
seq, minutes = sys.argv[2], float(sys.argv[3])
reps = int(sys.argv[4]) if len(sys.argv) > 4 else 5
seq2 = sys.argv[5] if len(sys.argv) > 5 else None
from PIL import Image
end = time.time() + minutes * 60
n = 0
while time.time() < end:
    r = drum.sync(3.0)
    if r:
        period, down = r
        now = time.perf_counter()
        k = int((now - down) / period) + 1
        while k % 4:
            k += 1
        for j, ch in enumerate(seq * reps):
            if ch not in drum.KEYS:
                continue
            t = down + (k + j) * period - 0.02
            while time.perf_counter() < t:
                pass
            drum.send(f'B {drum.KEYS[ch]:X} 60')
        kind = 'drum'
    else:
        drum.send('B 4000 150')
        time.sleep(2.0)
        kind = 'cross'
    n += 1
    drum.send(f'S {D}A{n:03d}.tga')
    if seq2:
        im = Image.open(f'{D}A{n:03d}.tga').convert('RGB').resize((48, 27))
        px = list(im.get_flattened_data()) if hasattr(im, "get_flattened_data") else list(im.getdata())
        if sum(1 for r, g, b in px if b > 120 and b > r + 60) > len(px) // 2:
            seq, seq2 = seq2, None
            kind += ' -> switch to ' + seq
    print(n, kind, flush=True)
