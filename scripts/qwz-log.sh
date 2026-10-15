#!/bin/bash
# qwz-log.sh [n]: republish the driver's log ring and print its last n lines
# (default 40). Lines from interrupt context are only here, not in the file.
N=${1:-40}
sudo sysctl debug.qwz.publish=1 >/dev/null || exit 1
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | tail -n "$N"
