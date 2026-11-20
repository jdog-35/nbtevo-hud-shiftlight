#!/bin/zsh
# Runs /var/opt/shiftlight-dev/libshiftlight.so on the head unit (over the shared ssh connection, Host hu), with the HMI's library
# path and DSI address (an SSH shell has neither). Arguments are SL_* settings; see the top of src/shiftlight.c.
#
#   tools/run.sh                          the bar at 3,000 rpm for 120 s (Enter stops it early)
#   tools/run.sh SL_ON=3000 SL_TOGGLE=buffer
#   tools/run.sh SL_MODE=trace SL_SECONDS=60
#   tools/run.sh SL_MODE=clear            the undo: the HUD as the BMW navigation leaves it after a route
set -e
HMI_ENV='LD_LIBRARY_PATH=/opt/hmi/lib:/opt/hmi/otf:/boot/lib:/lib:/usr/lib:/opt/sys/lib:/opt/car/lib DSI_IP_ADDRESS=160.48.199.254 DSI_APS_PARTITION_NAME=interaction'
for a in "$@"; do
  [[ "$a" =~ '^SL_[A-Z0-9]+=[A-Za-z0-9]+$' ]] || { echo "not an SL_ setting: $a"; exit 2; }
done
exec ssh -o BatchMode=yes hu "$HMI_ENV $* LD_PRELOAD=/var/opt/shiftlight-dev/libshiftlight.so /bin/cat"
