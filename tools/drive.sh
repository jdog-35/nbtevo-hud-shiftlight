#!/bin/zsh
# A drive run of libshiftlight.so (stages): started parked over SSH, it keeps going after the ENET cable is pulled
# (SL_DETACH=1), writes its log on the head unit, and ends by itself after SL_SECONDS (default here 1200 = 20 min), when a
# route starts on the HUD, when the head unit shuts down, or on `drive.sh stop` (it then clears the HUD).
#
#   tools/drive.sh start SL_SHIFT=5500 [SL_VERBOSE=1] [SL_SECONDS=1200] [other SL_ settings]
#   tools/drive.sh status        running or not, and the end of the log
#   tools/drive.sh stop          SIGTERM: it clears the HUD and exits
#   tools/drive.sh log [file]    the log to the Mac (default logs/<date>-drive.txt)
#
# On the head unit, in /var/opt/shiftlight-dev: drive.log (the run before kept as drive.prev.log) and drive.pid.
set -e
cd "$(dirname "$0")/.."
D=/var/opt/shiftlight-dev
HMI_ENV='LD_LIBRARY_PATH=/opt/hmi/lib:/opt/hmi/otf:/boot/lib:/lib:/usr/lib:/opt/sys/lib:/opt/car/lib DSI_IP_ADDRESS=160.48.199.254 DSI_APS_PARTITION_NAME=interaction'
SSH=(ssh -o BatchMode=yes hu)
RUNNING="[ -f $D/drive.pid ] && pidin -p \$(cat $D/drive.pid) -F '%N' 2>/dev/null | grep -q cat"

case "$1" in
start)
  shift
  secs=1200
  args=()
  for a in "$@"; do
    [[ "$a" =~ '^SL_[A-Z0-9]+=[A-Za-z0-9]+$' ]] || { echo "not an SL_ setting: $a"; exit 2; }
    case "$a" in
    SL_SECONDS=*) secs=${a#SL_SECONDS=} ;;
    SL_DETACH=* | SL_MODE=*) echo "drive.sh sets ${a%%=*} itself"; exit 2 ;;
    *) args+=("$a") ;;
    esac
  done
  "${SSH[@]}" "if $RUNNING; then echo 'a drive run is already going (tools/drive.sh stop)'; exit 1; fi
    if [ -f $D/drive.log ]; then mv $D/drive.log $D/drive.prev.log; fi
    $HMI_ENV SL_MODE=stages SL_DETACH=1 SL_SECONDS=$secs ${args[*]} LD_PRELOAD=$D/libshiftlight.so /bin/cat </dev/null >$D/drive.log 2>&1 &
    echo \$! > $D/drive.pid
    sleep 7
    cat $D/drive.log"
  echo "--- started for $secs s. Unplug the cable when you like; it keeps running."
  ;;
status)
  if "${SSH[@]}" "$RUNNING"; then echo "running (pid $("${SSH[@]}" "cat $D/drive.pid"))"; else echo "not running"; fi
  "${SSH[@]}" "cat $D/drive.log 2>/dev/null" | tail -15
  ;;
stop)
  "${SSH[@]}" "if $RUNNING; then kill -TERM \$(cat $D/drive.pid) && echo 'SIGTERM sent'; sleep 6; else echo 'not running'; fi"
  "${SSH[@]}" "cat $D/drive.log 2>/dev/null" | tail -12
  ;;
log)
  mkdir -p logs
  out=${2:-logs/$(date +%Y-%m-%d-%H%M)-drive.txt}
  "${SSH[@]}" "cat $D/drive.log" > "$out"
  echo "$out: $(wc -l < "$out") lines"
  ;;
*)
  sed -n '2,11p' "$0"
  exit 2
  ;;
esac
