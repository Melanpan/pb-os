#!/bin/bash
# One scenario with one implementation, inside the rootfs (see run.sh).
set -u
scen=$1 impl=$2
export XDG_RUNTIME_DIR=/tmp/rt XDG_CONFIG_HOME=/tmp/cfg HOME=/tmp/home
export PULSE_SERVER=unix:/tmp/rt/pulse/native
mkdir -p -m 0777 /tmp/rt /tmp/home "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
cat >"$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/50-test-sink.conf" <<'CONF'
context.objects = [
  { factory = adapter
    args = { factory.name = support.null-audio-sink node.name = "testsink"
             media.class = "Audio/Sink" audio.position = [ FL FR ] priority.session = 1000 } }
]
CONF
pipewire >/tmp/pw.log 2>&1 &
sleep 0.5
pipewire-pulse >/tmp/pwp.log 2>&1 &
for _ in $(seq 50); do [[ -S /tmp/rt/pulse/native ]] && break; sleep 0.1; done
chmod 0777 /tmp/rt /tmp/rt/pulse 2>/dev/null; chmod 0666 /tmp/rt/pulse/native
pactl info | grep -i 'default sink' >&2
pactl set-sink-volume @DEFAULT_SINK@ 30000 20000
pactl set-sink-mute @DEFAULT_SINK@ 0
sleep 0.3
/tmp/t/sinkrec >"/tmp/out/$scen.$impl.trace" &
rec=$!

mkfifo /tmp/ui
python3 /tmp/t/uinput.py </tmp/ui >"/tmp/ui.log" 2>&1 &
exec 3>/tmp/ui
ui() { echo "$*" >&3; sleep 0.05; }
node_of() {  # the /dev/input/eventN of the device named $1
  for d in /sys/class/input/event*; do
    [[ "$(cat "$d/device/name" 2>/dev/null)" == "$1" ]] && { echo "/dev/input/${d##*/}"; return; }
  done
}
tap() { ui press "$1" "$2"; ui release "$1" "$2"; sleep 0.4; }
hold() { ui press "$1" "$2"; sleep "$3"; ui release "$1" "$2"; sleep 0.5; }

daemon_env=()
user=()
start() {
  if [[ $impl == python ]]; then cmd=(python3 /tmp/t/sm8550-volume-keys)
  else cmd=(/tmp/t/pbos-platformd --user --only volume-keys); fi
  env "${daemon_env[@]}" "${user[@]}" "${cmd[@]}" >"/tmp/out/$scen.$impl.log" 2>&1 &
  dpid=$!
  sleep 1.5
}

case $scen in
s1)  # physical keys present from the start; mute set elsewhere; holds
  ui create g gpio-keys 115; ui create r pmic_resin 114; sleep 0.3
  start
  tap g 115; tap g 115; tap g 115
  tap r 114; tap r 114
  pactl set-sink-mute @DEFAULT_SINK@ 1; sleep 0.3
  tap g 115
  hold g 115 1.95
  hold r 114 1.05
  tap r 114
  ;;
s2)  # sm8550: InputPlumber's keyboard comes late, as a user (udev hand-over), goes while held, comes back
  user=(setpriv --reuid=1000 --regid=1000 --clear-groups)
  ui create g gpio-keys 115; sleep 0.3; chmod 0666 "$(node_of gpio-keys)"
  start
  ui create ip InputPlumber+Keyboard 115,114,113,30
  sleep 1; chmod 0666 "$(node_of 'InputPlumber Keyboard')"
  sleep 4  # the Python looked every 3 s
  tap ip 115   # physical gpio-keys has it: ignored here
  tap ip 114
  tap ip 113
  tap g 115    # unmutes
  hold ip 114 1.05
  ui press ip 114; sleep 0.65; ui destroy ip; sleep 1
  ui create ip InputPlumber+Keyboard 115,114,113,30
  sleep 0.3; chmod 0666 "$(node_of 'InputPlumber Keyboard')"
  sleep 4
  tap ip 114; tap ip 113; tap ip 113
  ;;
s3)  # the knobs: step, limit, hold timing
  daemon_env=(SM8550_VOLUME_STEP=3% SM8550_VOLUME_LIMIT=0.8 SM8550_VOLUME_HOLD_DELAY=0.3 SM8550_VOLUME_HOLD_INTERVAL=0.15)
  ui create g gpio-keys 115; ui create r pmic_resin 114; sleep 0.3
  start
  tap g 115
  hold g 115 2.05
  hold r 114 1.0
  tap r 114
  ;;
s4)  # odd knobs: a raw step (pactl's "-2000" is raw units), limit 0.29 → 28 %
  daemon_env=(SM8550_VOLUME_STEP=2000 SM8550_VOLUME_LIMIT=0.29)
  ui create g gpio-keys 115; ui create r pmic_resin 114; sleep 0.3
  start
  tap r 114; tap r 114; tap r 114
  tap g 115
  hold r 114 1.05
  ;;
s5)  # (expected to differ) a key 0.3 s after udev hands the keyboard over:
     # volume-keys opens it on that change, the Python only on its next 3 s look
  user=(setpriv --reuid=1000 --regid=1000 --clear-groups)
  ui create g gpio-keys 115; sleep 0.3; chmod 0666 "$(node_of gpio-keys)"
  start
  ui create ip InputPlumber+Keyboard 115,114,113,30
  sleep 1; chmod 0666 "$(node_of 'InputPlumber Keyboard')"
  sleep 0.3
  tap ip 114
  ;;
esac
sleep 0.5
kill "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
exec 3>&-
sleep 0.3
kill "$rec" 2>/dev/null
kill %1 %2 2>/dev/null
cat /tmp/ui.log >&2
wait 2>/dev/null
true
