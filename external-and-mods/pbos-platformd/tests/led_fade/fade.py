# sm8550-sleep's python fade as it was (the reference for run.sh), taken
# verbatim from the heredoc: python3 fade.py out|in SAVE.
import math, sys, time
way, save = sys.argv[1], sys.argv[2]

def lowest(d):
    try:
        ch = [int(c) for c in open(d + '/multi_intensity').read().split()]
        top = int(open(d + '/max_brightness').read())
    except (OSError, ValueError):
        return 0
    main = [c for c in ch if c and c >= max(ch) / 4]
    return math.ceil(top / min(main)) if main else 0

leds = [(open(d + '/brightness', 'w'), int(b), lowest(d))
        for d, b in (l.split() for l in open(save) if l.strip())]
T, start, last = 0.3, time.monotonic(), {}
while True:
    x = min((time.monotonic() - start) / T, 1.0)
    k = (1 - x) ** 2.2 if way == 'out' else x ** 2.2
    for f, b, low in leds:
        v = round(b * k)
        if v < low:
            v = 0
        if last.get(f) != v:
            f.write(str(v)); f.flush(); last[f] = v
    if x >= 1: break
    time.sleep(0.01)
