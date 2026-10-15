#!/bin/bash
# qwz-step.sh <stage>: rerun the bring-up up to <stage> without rebooting and
# print the result (needs the driver loaded and idle; sudo for the sysctls).
# Stages: 1 task queue, 2 gate, 3 attach, 4 config/BAR, 5 MSI, 6 D0+regs, 7 DMA,
# 8 attach done, 9 chip reset, 10 MHI, 11 fw download, 12 MHI channels,
# 13 powered up, 1000 = everything. Stages 14-19 are inside the firmware init
# and PARK instead of cleaning up (firmware left running, chip interrupts off):
# 14 QMI done, 15 CE/DP alloc, 16 WMI ready, 17 core started, 18 pdev+mac,
# 19 init done. After a parked run, reboot before the next attempt.
set -u
N=${1:?usage: $0 <stage>}
sudo sysctl debug.qwz.stage=$N >/dev/null && sudo sysctl debug.qwz.start=1 >/dev/null || exit 1
for i in $(seq 1 60); do
  sleep 1
  st=$(ioreg -l -w0 -r -k QwzMacStage | sed -n 's/.*"QwzMacStage" = "\(.*\)"/\1/p' | head -1)
  case "$st" in *idle*|*running*|*failed*) break ;; esac
done
echo "stage: $st"
echo "--- log tail"
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | tail -25
