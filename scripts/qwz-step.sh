#!/bin/bash
# qwz-step.sh <stage>: rerun the bring-up up to <stage> without rebooting and
# print the result (needs the driver loaded and idle; sudo for the sysctls).
# Stages: 1 thread test, 2 task queue, 3 gate, 4 attach, 5 config/BAR, 6 MSI,
# 7 D0+regs, 8 DMA, 9 attach done, 10 chip reset, 11 MHI, 12 fw download,
# 13 MHI channels, 14 powered up, 1000 = everything.
# Stages 15-20 are inside the firmware init and PARK instead of cleaning up
# (firmware left running, chip interrupts off, nothing freed): 15 QMI done,
# 16 CE/DP alloc, 17 WMI ready, 18 core started, 19 pdev+mac, 20 init done.
# After a parked run, reboot before the next attempt.
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
