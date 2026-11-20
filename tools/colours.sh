#!/bin/zsh
# HUD palette probe, after st1ng's "hud_probe.sh colours": for each fill colour index, a filled square in picture buffer 6
# at (40,20)-(80,63) (inside this car's visible box (36,16)-(89,69)) and the index as digits in the distance text (3).
# Run hud-clear.txt afterwards (hudcmd.sh < hud-clear.txt) to leave the HUD as the BMW navigation does after a route.
#
#   colours.sh [first=0] [last=31] [secs=3]
set -e
cd "$(dirname "$0")"
first=${1:-0}; last=${2:-31}; secs=${3:-3}
B=0006
SQ="21d ${B}0000030400280014005000140050003f0028003f"   # AddPolygon(6, item 0, filled, 4 points)
./hudcmd.sh <<EOF
204 000101
204 ${B}01
204 000301
EOF
for c in {$first..$last}; do
  T=""
  for ch in ${(s::)c}; do T+=$(printf '00%02x' $(( ch + 48 ))); done   # UTF-16BE digits
  print "colour $c  $(date +%T)"
  ./hudcmd.sh <<EOF
220 ${B}
21b ${B}$(printf '%02x' $c)
219 ${B}00
${SQ}
210 000300${T}0000
EOF
  sleep $secs
done
print "done: run  ./hudcmd.sh < hud-clear.txt"
