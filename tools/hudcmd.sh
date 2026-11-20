#!/bin/zsh
# Sends one batch of HUDTerminal calls (FBlock 82/00) through the CarPlay plugin's test channel on the head unit, the way
# st1ng's tools/hud_probe.sh does: the batch goes to /dev/shmem/carplaymenu-hudcmd (a RAM file), the plugin's MOST client
# runs it (20 ms between calls) and deletes the file. On this car's HUD-only install that client lives in mm-ipod, which
# runs only during a CarPlay session: the iPhone must be on CarPlay. The cluster's answers go to /var/iap/rghook.log.
#
#   hudcmd.sh < batch.txt      lines "<fkt hex> <payload hex>", e.g. "204 000101" = SetVisibility(1, on); '#' comments
#
# Exit 0 once the plugin has taken the batch, 1 if it is busy or nothing took it within 8 s (the file is then removed).
set -e
CMD=/dev/shmem/carplaymenu-hudcmd
SSH=(ssh -o BatchMode=yes hu)
batch=$(cat)
"${SSH[@]}" "if [ -f $CMD ]; then echo 'test channel busy ($CMD still there)'; exit 1; fi; cat > $CMD.tmp && mv $CMD.tmp $CMD" <<< "$batch" 2>/dev/null
for i in {1..16}; do
  sleep 0.5
  "${SSH[@]}" "[ -f $CMD ]" 2>/dev/null || exit 0
done
"${SSH[@]}" "rm -f $CMD" 2>/dev/null
echo "nothing took the calls: is the iPhone on CarPlay (mm-ipod running)?" >&2
exit 1
