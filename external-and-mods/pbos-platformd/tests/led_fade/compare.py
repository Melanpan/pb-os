# Compares the Python and C++ runs of each case (see run.sh).
# virt (virtual clock): the same writes in the same order with the same
# values at the same (virtual) times, the same exit status and end state.
# real: each value an LED takes in both comes at the same time into the
# fade (from the first write) within TOL ms; prints each one's start-up
# (launch to first write) and run time.
#
# python3 compare.py OUTDIR CASE...
import json, sys

TOL = 15.0  # ms: frames are 10 ms apart, plus a busy VM's scheduling

def wlog(path):
    try:
        lines = open(path).read().splitlines()
    except FileNotFoundError:
        return []
    return [(int(t), p, v) for t, p, v in (l.split('\t', 2) for l in lines)]

def load(out, c, mode):
    return {i: json.load(open(f'{out}/{c}.{mode}.{i}.json')) for i in ('py', 'cpp')}

out, cases = sys.argv[1], sys.argv[2:]
bad = 0
for c in cases:
    v, r = load(out, c, 'virt'), load(out, c, 'real')
    for s in range(len(v['py'])):
        msgs = []
        # virt: everything exact.
        py, cpp = v['py'][s], v['cpp'][s]
        w = {i: wlog(f'{out}/{c}.virt.{i}.{s}.wlog') for i in ('py', 'cpp')}
        if w['py'] != w['cpp']:
            msgs.append('virt: writes differ')
        if py['rc'] != cpp['rc']:
            msgs.append(f"virt: rc py {py['rc']} cpp {cpp['rc']}")
        if py['final'] != cpp['final'] or py['save'] != cpp['save']:
            msgs.append('virt: end state differs')
        last = {}
        for _, p, val in w['cpp']:
            last[p.split('/')[-2]] = val
        nvirt = len(w['py'])
        # real: the same rc and last values; when each value came.
        rp, rc = r['py'][s], r['cpp'][s]
        rw = {i: wlog(f'{out}/{c}.real.{i}.{s}.wlog') for i in ('py', 'cpp')}
        if rp['rc'] != rc['rc']:
            msgs.append(f"real: rc py {rp['rc']} cpp {rc['rc']}")
        rlast = [{p: val for _, p, val in rw[i]} for i in ('py', 'cpp')]
        if rlast[0] != rlast[1]:
            msgs.append('real: last values differ')
        dt, common, span = 0.0, 0, {}
        if rw['py'] and rw['cpp']:
            when = []
            for i in ('py', 'cpp'):
                t0, seen = rw[i][0][0], {}
                for t, p, val in rw[i]:
                    seen.setdefault((p, val), (t - t0) / 1e6)
                when.append(seen)
                span[i] = (rw[i][-1][0] - t0) / 1e6
            keys = when[0].keys() & when[1].keys()
            common = len(keys)
            dt = max(abs(when[0][k] - when[1][k]) for k in keys)
            if dt > TOL:
                msgs.append(f'real: timing off by {dt:.1f} ms')
        def times(x, ws, i):
            first = f'{(ws[0][0] - x["start_ns"]) / 1e6:5.1f} ms' if ws else '   -    '
            fade = f', fade {span[i]:5.1f} ms' if i in span else ''
            return f"launch to first write {first}{fade}, run {(x['end_ns'] - x['start_ns']) / 1e6:5.1f} ms"
        print(f"{c}[{s}] {py['way']}: virt {nvirt}/{len(w['cpp'])} writes, rc {py['rc']}/{cpp['rc']}; "
              f"real {len(rw['py'])}/{len(rw['cpp'])} writes, {common} values in both, max diff {dt:.1f} ms "
              f"-> {'PASS' if not msgs else 'FAIL: ' + ', '.join(msgs)}")
        print(f"    py:  {times(rp, rw['py'], 'py')}\n    cpp: {times(rc, rw['cpp'], 'cpp')}")
        if last:
            print(f"    last: {last}")
        bad += bool(msgs)
print('ALL PASS' if not bad else f'{bad} FAILED')
sys.exit(1 if bad else 0)
