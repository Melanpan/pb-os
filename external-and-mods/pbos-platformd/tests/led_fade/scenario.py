# One led-fade case in a throwaway /sys (bwrap, see run.sh): builds the LED
# devices, writes the save file as sm8550-sleep's pre does, runs one
# implementation with the write logger, and saves the log and results.
#
# python3 scenario.py --list
# python3 scenario.py py|cpp CASE OUTDIR virt|real
# virt: the virtual clock (vclock.so), for exact values; real: real time.
import json, os, subprocess, sys, time

# name: (driver, max_brightness, multi_intensity or None, brightness)
RP6 = {
    'l1': ('leds_group_multicolor', '255', '255 105 180', '255'),   # pink: low 3
    'l2': ('leds_group_multicolor', '255', '255 105 180', '128'),
    'l3': ('leds_group_multicolor', '255', '0 0 255', '200'),       # blue: low 1
    'l4': ('leds_group_multicolor', '255', '10 3 0', '255'),        # dim: low 85
    'r1': ('leds_group_multicolor', '255', '10 3 0', '50'),         # under its low: stays 0
    'r2': ('leds_group_multicolor', '255', '255 255 255', '17'),
    'r3': ('leds_group_multicolor', '255', None, '99'),             # no color file: low 0
    'r4': ('leds_group_multicolor', 'junk', '1 2 3', '77'),         # bad max: low 0
}
ODIN2 = {
    'left-joystick': ('leds_pwm_multicolor', '255', '255 0 0', '255'),
    'left-side': ('leds_pwm_multicolor', '255', '0 255 64', '180'),
    'right-joystick': ('leds_pwm_multicolor', '255', '128 0 255', '255'),
    'right-side': ('leds_pwm_multicolor', '255', '40 40 0', '3'),
    'charge': ('leds_qcom_lpg', '255', None, '255'),               # not a stick LED
}
OFF = {k: (v[0], v[1], v[2], '0') for k, v in RP6.items()}

CASES = {
    # case: (leds, steps); a step is (way, save) with save 'hook' (pre's
    # list), 'missing' (no file), 'gone' (lists an LED that is not there),
    # 'bad' (a line with one field) or 'keep' (the file the last step used)
    'rp6-out': (RP6, [('out', 'hook')]),
    'rp6-out-in': (RP6, [('out', 'hook'), ('in', 'keep')]),
    'odin2-out-in': (ODIN2, [('out', 'hook'), ('in', 'keep')]),
    'no-leds': ({}, [('out', 'hook'), ('in', 'keep')]),
    'leds-off': (OFF, [('out', 'hook'), ('in', 'keep')]),
    'missing-save': (RP6, [('in', 'missing')]),
    'gone': (RP6, [('out', 'gone')]),
    'bad-line': (RP6, [('in', 'bad')]),
}

def build(leds):
    os.makedirs('/sys/class/leds', exist_ok=True)
    os.makedirs('/sys/bus/drivers', exist_ok=True)
    for name, (drv, top, mi, b) in leds.items():
        dev = f'/sys/devices/platform/{name}-dev'
        d = f'{dev}/leds/{name}'
        os.makedirs(d)
        os.makedirs(f'/sys/bus/drivers/{drv}', exist_ok=True)
        os.symlink(f'/sys/bus/drivers/{drv}', f'{dev}/driver')
        os.symlink(dev, f'{d}/device')
        os.symlink(d, f'/sys/class/leds/{name}')
        open(f'{d}/max_brightness', 'w').write(top + '\n')
        if mi is not None:
            open(f'{d}/multi_intensity', 'w').write(mi + '\n')
        open(f'{d}/brightness', 'w').write(b + '\n')

def hook_save(path):
    # sm8550-sleep's leds() and pre, in sh as the hook runs them.
    sh = r'''
leds() {
    for d in /sys/class/leds/*; do
        case "$(basename "$(readlink "$d/device/driver" 2>/dev/null)")" in
            leds_group_multicolor|leds_pwm_multicolor) echo "$d" ;;
        esac
    done
}
: > "$1"
for d in $(leds); do
    b=$(cat "$d/brightness") && [ "$b" != 0 ] && echo "$d $b" >> "$1"
done
'''
    subprocess.run(['sh', '-c', sh, 'sh', path])  # status: the last LED's test, as in the hook

def main():
    if sys.argv[1] == '--list':
        print(' '.join(CASES))
        return
    impl, case, out, mode = sys.argv[1:5]
    leds, steps = CASES[case]
    build(leds)
    save = '/run/sm8550-sleep-leds'
    results = []
    for i, (way, how) in enumerate(steps):
        if how == 'hook':
            hook_save(save)
        elif how == 'missing':
            save = '/run/nothing-here'
        elif how == 'gone':
            hook_save(save)
            with open(save, 'a') as f:
                f.write('/sys/class/leds/l9 200\n')
        elif how == 'bad':
            hook_save(save)
            with open(save, 'a') as f:
                f.write('/sys/class/leds/l1\n')
        cmd = (['python3', '/tmp/t/fade.py'] if impl == 'py' else ['/tmp/t/pbos-platformd', 'led-fade']) + [way, save]
        log = f'{out}/{case}.{mode}.{impl}.{i}.wlog'
        env = dict(os.environ, LD_PRELOAD='/tmp/t/vclock.so /tmp/t/wlog.so' if mode == 'virt' else '/tmp/t/wlog.so', LED_WLOG=log)
        t0 = time.monotonic_ns()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        t1 = time.monotonic_ns()
        final = {n: open(f'/sys/class/leds/{n}/brightness').read() for n in sorted(leds)}
        results.append({'way': way, 'rc': p.returncode, 'start_ns': t0, 'end_ns': t1,
                        'stderr_lines': len(p.stderr.splitlines()), 'final': final,
                        'save': open(save).read() if os.path.exists(save) else None})
    json.dump(results, open(f'{out}/{case}.{mode}.{impl}.json', 'w'), indent=1)

main()
