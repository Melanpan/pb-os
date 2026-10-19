#!/bin/bash
# One implementation's run, inside the rootfs (see run.sh).
set -u
impl=$1
SINK=alsa_output.platform-sound.HiFi__Speaker__sink
export XDG_RUNTIME_DIR=/tmp/rt XDG_CONFIG_HOME=/tmp/cfg HOME=/tmp/home
export PULSE_SERVER=unix:/tmp/rt/pulse/native
mkdir -p -m 0700 /tmp/rt /tmp/home "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
cat >"$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/50-test-sink.conf" <<CONF
context.objects = [
  { factory = adapter
    args = { factory.name = support.null-audio-sink node.name = "$SINK"
             media.class = "Audio/Sink" audio.position = [ FL FR ] priority.session = 1000 } }
]
CONF
pw_up() {
  pipewire >>/tmp/pw.log 2>&1 &
  pw=$!
  sleep 0.5
  pipewire-pulse >>/tmp/pwp.log 2>&1 &
  pwp=$!
  for _ in $(seq 50); do pactl info >/dev/null 2>&1 && break; sleep 0.1; done
}
pw_up
/tmp/t/mkctl 0 81
ctl() { amixer -c 0 -q cset "name=$1" "$2"; }
ctl 'WSA_RX0 Digital Volume' 81; ctl 'WSA_RX1 Digital Volume' 81; ctl 'Speakers Volume' 12,12
pactl set-sink-volume $SINK 30%; pactl set-sink-mute $SINK 0
sleep 0.3

val() { amixer -c 0 cget "name=$1" | sed -n 's/^  : values=//p'; }
nid() { pw-cli ls Node | awk -v n="node.name = \"$SINK\"" '/^\s*id [0-9]+,/{ id=$2; sub(",","",id) } index($0, n) { print id; exit }'; }
soft() {
  pw-dump "$(nid)" | python3 -c '
import json, sys
for o in json.load(sys.stdin):
    for p in (o.get("info", {}).get("params", {}).get("Props") or []):
        if p.get("softVolumes"):
            print(",".join("%.3f" % v for v in p["softVolumes"])); sys.exit()
print("-")'
}
snap() {
  local vol mute
  vol=$(pactl get-sink-volume $SINK | grep -o '[0-9]*%' | head -1)
  mute=$(pactl get-sink-mute $SINK | awk '{print $2}')
  echo "$1 bar=$vol mute=$mute pa=$(val 'Speakers Volume') d0=$(val 'WSA_RX0 Digital Volume') d1=$(val 'WSA_RX1 Digital Volume') soft=$(soft)" >>"/tmp/out/$impl.snap"
}
settle() { sleep 3.5; snap "$1"; }  # the Python's 3 s look, and a bit
fast() { sleep 0.3; snap "fast $1"; sleep 3.2; snap "$1"; }

# Every control event on the card, to count writes.
stdbuf -oL amixer -c 0 events >/tmp/events.log 2>&1 &
ev=$!
: >"/tmp/out/$impl.snap"
if [[ $impl == python ]]; then python3 /tmp/t/konkr-volume >"/tmp/out/$impl.log" 2>&1 &
else /tmp/t/pbos-platformd --user --only konkr-volume >"/tmp/out/$impl.log" 2>&1 & fi
dpid=$!
settle start
for v in 1 5 10 20 30 50 75 100 0 40 150 37; do
  pactl set-sink-volume $SINK "$v%"; settle "vol$v"
done
pactl set-sink-mute $SINK 1; settle muted
pactl set-sink-volume $SINK 60%; settle muted-vol60
pactl set-sink-mute $SINK 0; settle unmuted
ctl 'Speakers Volume' 12,12; fast pa-reset
ctl 'WSA_RX0 Digital Volume' 81; fast digital-reset          # (the Python never sees this one)
ctl 'Speakers Volume' 12,12; fast pa-reset-again               # PA fixed → Python re-forces both stages
ctl 'WSA_RX0 Digital Volume' 81; ctl 'WSA_RX1 Digital Volume' 81; ctl 'Speakers Volume' 12,12; fast all-reset
pw-cli set-param "$(nid)" Props '{ softVolumes: [ 0.5, 0.5 ] }' >/dev/null; fast soft-drift
kill $pwp; wait $pwp 2>/dev/null; sleep 0.5
pipewire-pulse >>/tmp/pwp.log 2>&1 & pwp=$!
sleep 2.5; settle pulse-restart
pactl set-sink-volume $SINK 25%; settle pulse-restart-vol25
kill $pwp $pw; wait $pwp $pw 2>/dev/null; sleep 0.5
ctl 'Speakers Volume' 12,12; ctl 'WSA_RX0 Digital Volume' 81; ctl 'WSA_RX1 Digital Volume' 81  # as a UCM restore would
pw_up
pactl set-sink-volume $SINK 25%
sleep 2.5; settle pipewire-restart
pactl set-sink-volume $SINK 45%; settle pipewire-restart-vol45
# A fight: something resets the PA every 50 ms for 5 s.
before=$(grep -c "Speakers Volume" /tmp/events.log)
for _ in $(seq 100); do ctl 'Speakers Volume' 12,12; sleep 0.05; done
after=$(grep -c "Speakers Volume" /tmp/events.log)
settle after-fight
fight=$(( after - before ))
# Idle CPU over 15 s (utime+stime+children, clock ticks).
cpu() { awk '{ print $14 + $15 + $16 + $17 }' /proc/$dpid/stat; }
c0=$(cpu); sleep 15; c1=$(cpu)
alive=$(kill -0 $dpid 2>/dev/null && echo alive || echo DEAD)
kill $dpid; wait $dpid 2>/dev/null; code=$?
echo "$impl: daemon $alive; PA events during the 5 s fight: $fight (100 resets, both writes counted); idle CPU 15 s: $(( c1 - c0 )) ticks; exit on SIGTERM: $code" >"/tmp/out/$impl.stats"
kill $ev $pwp $pw 2>/dev/null
cp /tmp/events.log "/tmp/out/$impl.events"
wait 2>/dev/null
true
