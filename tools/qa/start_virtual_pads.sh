#!/usr/bin/env bash
# QA: start the virtual pads for testing pad-driven peek.
#
# A physical controller cannot be pressed from a script, so the test uses
# `virtual_pads`, which needs root only because /dev/uinput is root-only. It runs
# detached and is driven through the control FIFO:
#
#   bash tools/qa/start_virtual_pads.sh          # verified by the user's sudo
#   echo "0 ld" > /tmp/pads.ctl                  # pad 0 (P1) left shoulder down
#   echo "0 lu" > /tmp/pads.ctl                  # ... and up
#   echo quit > /tmp/pads.ctl                    # tear both pads down
#
# Nothing on the machine is reconfigured: the pads are uinput devices that exist
# only until `quit`, and the only permission touched is 0666 on the two event
# nodes they create, so SDL (running as the user) can read them.
set -eu
cd "$(dirname "$0")/../.."
ctl=${1:-/tmp/pads.ctl}
count=${2:-2}
rm -f "$ctl"
mkfifo "$ctl"
chmod 666 "$ctl"
setsid nohup ./tools/qa/virtual_pads "$ctl" "$count" >/tmp/pads.log 2>&1 </dev/null &
sleep 1
cat /tmp/pads.log
