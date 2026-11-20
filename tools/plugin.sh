#!/bin/zsh
# The Shift Light app in My Vehicle (step 6) on the head unit, in /var/opt/hmi/data/ID5/Plugins/ShiftLight:
#   libShiftLight.so, ShiftLight.manifest, ShiftLight.xml   the menu (an iDrive plugin, loaded at every iDrive start)
#   shiftlight-worker.so                                    out/libshiftlight.so, run by the plugin in SL_MODE=service
#   on, sport-only, manual-only, shift-rpm, car-map         the switches (written by the menu; car-map by `map`)
#
#   tools/plugin.sh check          read-only: the frameworks have what the plugin uses, the folder and id 230 are free
#   tools/plugin.sh install        stage in /var/opt/shiftlight-dev/stage-ShiftLight, check every hash, then move the
#                                  folder into Plugins. Then restart iDrive (parked): hold the volume knob until it goes dark
#   tools/plugin.sh update-worker  replace only shiftlight-worker.so (used from the next worker start: untick and tick)
#   tools/plugin.sh update-plugin  replace libShiftLight.so and ShiftLight.xml (loaded at the next iDrive start)
#   tools/plugin.sh status         the switches, the car data, the worker, the end of both logs
#   tools/plugin.sh log            the logs to logs/<date>-plugin.txt, -service.txt and -events.txt
#   tools/plugin.sh map 'mode35up 3 4' 'gear 5 6'   write car-map (which raw values are sport / manual)
#   tools/plugin.sh uninstall      stop the worker (it clears the HUD), move the folder to
#                                  /var/opt/hmi/data/ID5/PluginBackups/ShiftLight-<time>. Then restart iDrive
set -e
cd "$(dirname "$0")/.."
P=/var/opt/hmi/data/ID5/Plugins/ShiftLight
STAGE=/var/opt/shiftlight-dev/stage-ShiftLight
SSH=(ssh -o BatchMode=yes hu)
quiet() { grep -v -e post-quantum -e 'store now' -e 'upgraded. See' -e 'cannot bind' || true; }
r() { "${SSH[@]}" "$@" 2> >(quiet >&2); }
typeset -A SRC
SRC=(libShiftLight.so out/plugin/libShiftLight.so ShiftLight.manifest out/plugin/ShiftLight.manifest ShiftLight.xml out/plugin/ShiftLight.xml
     shiftlight-worker.so out/libshiftlight.so)

backup() { # backup <file in $P>: the head unit's copy into out/archive/<name>-<hash8> before it is replaced
  local tmp=$(mktemp) h
  r "cat $P/$1" > $tmp
  if [ -s $tmp ]; then
    h=$(shasum -a 256 < $tmp | cut -c1-8)
    mkdir -p out/archive && mv $tmp "out/archive/car-${1%.*}-$h.${1##*.}"
    echo "  backed up the car's $1 ($h) to out/archive/car-${1%.*}-$h.${1##*.}"
  else
    rm -f $tmp
  fi
}

verify() { # verify <remote dir>: every file's hash against the Mac's
  local bad=0 f l rr
  for f in ${(k)SRC}; do
    l=$(shasum -a 256 < ${SRC[$f]} | cut -c1-64)
    rr=$(r "cat $1/$f" | shasum -a 256 | cut -c1-64)
    if [ "$l" = "$rr" ]; then echo "  ok  $f ${l:0:16}"; else echo "  BAD $f: Mac ${l:0:16}, head unit ${rr:0:16}"; bad=1; fi
  done
  return $bad
}

case "$1" in
check)
  tmp=$(mktemp -d)
  for f in libAppFramework.A.so libCoreFramework.A.so; do r "cat /opt/hmi/ID5/lib/$f" > $tmp/$f; done
  echo "frameworks: $(wc -c < $tmp/libAppFramework.A.so) and $(wc -c < $tmp/libCoreFramework.A.so) bytes"
  for s in $(nm -D -u out/plugin/libShiftLight.so | awk '{print $NF}' | grep '^_ZN3bmw') \
           _ZN3bmw9PluginApp4postEPNS_8RunnableEj \
           _ZN3bmw9PluginApp33cdsAddPropertyChangedEventHandlerERNS_11BasicStringIcEEjRKNS_8DelegateIbRKNS1_ItEEvvvEE \
           _ZTVN3bmw8RunnableE _ZTVN3bmw9PluginAppE; do
    w=$(nm -D -S --defined-only $tmp/*.so 2>/dev/null | grep " $s\$" | awk '{print "size 0x"$2}' | sed 's/0x0*/0x/' | uniq)
    printf '  %-7s %s %s\n' "${${w:+found}:-MISSING}" "${s:0:70}" "$w"
  done
  echo "  (PluginApp vtable: 0xac = 41 slots, as st1ng's plugin expects; post, cdsAdd... and the Runnable vtable are"
  echo "   optional: without them the menu still loads, only the drive mode and gear are missing)"
  rm -rf $tmp
  r "ls /var/opt/hmi/data/ID5/Plugins; for m in /var/opt/hmi/data/ID5/Plugins/*/*.manifest; do grep '<id>' \$m; done; \
     [ -e $P ] && echo 'ShiftLight: already installed' || echo 'ShiftLight: not installed'; df -k /var/opt"
  ;;
install)
  for f in ${(k)SRC}; do [ -f ${SRC[$f]} ] || { echo "missing ${SRC[$f]}: run ./build.sh"; exit 1; }; done
  if r "[ -e $P ]"; then echo "$P exists: uninstall first"; exit 1; fi
  r "rm -rf $STAGE && mkdir -p $STAGE"
  for f in ${(k)SRC}; do r "cat > $STAGE/$f" < ${SRC[$f]}; done
  r "chmod 755 $STAGE/libShiftLight.so $STAGE/shiftlight-worker.so"
  echo "staged in $STAGE:"; verify $STAGE || { echo "not installed (staging left in $STAGE)"; exit 1; }
  r "mv $STAGE $P"
  echo "installed in $P:"; verify $P
  r "ls -l $P"
  echo "--- now restart iDrive (parked): hold the volume knob until the screen goes dark. Undo: tools/plugin.sh uninstall"
  ;;
update-plugin)
  # replace libShiftLight.so and ShiftLight.xml (the menu); loaded at the next iDrive start. Keep the old ones in
  # out/archive first.
  for f in libShiftLight.so ShiftLight.xml; do
    backup $f
    r "cat > $P/$f.new" < out/plugin/$f
    l=$(shasum -a 256 < out/plugin/$f | cut -c1-64); rr=$(r "cat $P/$f.new" | shasum -a 256 | cut -c1-64)
    [ "$l" = "$rr" ] || { echo "$f: upload differs (${rr:0:16}), left as $f.new"; exit 1; }
  done
  r "chmod 755 $P/libShiftLight.so.new && mv $P/libShiftLight.so.new $P/libShiftLight.so && mv $P/ShiftLight.xml.new $P/ShiftLight.xml"
  echo "plugin $(shasum -a 256 < out/plugin/libShiftLight.so | cut -c1-16) and menu $(shasum -a 256 < out/plugin/ShiftLight.xml | cut -c1-16) replaced; restart iDrive (volume knob) to load them"
  ;;
update-worker)
  backup shiftlight-worker.so
  r "cat > $P/shiftlight-worker.so.new" < out/libshiftlight.so
  l=$(shasum -a 256 < out/libshiftlight.so | cut -c1-64); rr=$(r "cat $P/shiftlight-worker.so.new" | shasum -a 256 | cut -c1-64)
  [ "$l" = "$rr" ] || { echo "upload differs (${rr:0:16}), left as .new"; exit 1; }
  r "chmod 755 $P/shiftlight-worker.so.new && mv $P/shiftlight-worker.so.new $P/shiftlight-worker.so"
  echo "worker replaced (${l:0:16}); it is used from the next worker start (untick and tick Shift light)"
  ;;
status)
  r "cd $P 2>/dev/null || { echo 'not installed'; exit 0; }; for f in on sport-only manual-only shift-rpm car-map disabled boot_strikes; do \
       [ -f \$f ] && echo \"\$f: \$(cat \$f)\"; done; echo '--- car data'; cat /dev/shmem/shiftlight-car 2>/dev/null; \
       p=\$(cat /dev/shmem/shiftlight.pid 2>/dev/null); if [ -n \"\$p\" ] && kill -0 \$p 2>/dev/null; then echo \"--- worker running (pid \$p)\"; \
       else echo '--- worker not running'; fi; [ -f /dev/shmem/shiftlight-aside ] && echo '(stepped aside for the BMW navigation)'; \
       echo '--- plugin log'; cat /dev/shmem/shiftlight-plugin.log 2>/dev/null" | tail -40
  echo '--- worker log'; r "cat /dev/shmem/shiftlight.log 2>/dev/null" | tail -15
  echo '--- events.log (kept across reboots)'; r "cat $P/events.log 2>/dev/null" | tail -15
  ;;
log)
  d=$(date +%Y-%m-%d-%H%M)
  mkdir -p logs  # local only: .gitignore keeps logs/ out of the repository (traces can hold the car's position)
  r "cat /dev/shmem/shiftlight-plugin.log" > logs/$d-plugin.txt; r "cat /dev/shmem/shiftlight.log" > logs/$d-service.txt
  r "cat $P/events.log 2>/dev/null" > logs/$d-events.txt
  wc -l logs/$d-plugin.txt logs/$d-service.txt logs/$d-events.txt
  ;;
map)
  shift
  [ $# -gt 0 ] || { echo "usage: tools/plugin.sh map 'mode35up 3 4' 'gear 5 6'"; exit 2; }
  for a in "$@"; do [[ "$a" =~ '^(mode|mode35up|gear|shift)( -?[0-9]+)+$' ]] || { echo "not a map line: $a"; exit 2; }; done
  r "cat > $P/car-map" <<< "${(F)@}"
  r "cat $P/car-map"
  ;;
uninstall)
  r "p=\$(cat /dev/shmem/shiftlight.pid 2>/dev/null); [ -n \"\$p\" ] && kill -TERM \$p 2>/dev/null && echo 'worker stopped' || true"
  sleep 4
  ts=$(date +%Y%m%d-%H%M%S)
  r "mkdir -p /var/opt/hmi/data/ID5/PluginBackups && mv $P /var/opt/hmi/data/ID5/PluginBackups/ShiftLight-$ts && \
     echo moved to /var/opt/hmi/data/ID5/PluginBackups/ShiftLight-$ts"
  echo "--- now restart iDrive (parked) to drop the menu. To restore: move the folder back to $P and restart iDrive"
  ;;
*)
  sed -n '2,18p' "$0"
  exit 2
  ;;
esac
