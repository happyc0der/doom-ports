#!/bin/sh
# Build and install TRENCHFIRE on a Venu X1 over USB (MTP).   Usage: ./install.sh
#
# Needs libmtp (brew install libmtp).  Plug the watch in, unlocked; it can take
# 10-20 s to appear on the bus.  Developer mode must be on for the watch to
# run a sideloaded app: Settings > System > About, tap the serial number
# seven times.
set -e
cd "$(dirname "$0")"
./build.sh
make -s -C tools mtp_push
export LANG="${LANG:-en_US.UTF-8}"
i=0
until mtp-detect 2>/dev/null | grep -q "Garmin"; do
    i=$((i + 1)); [ "$i" -gt 12 ] && { echo "no watch on USB after 60 s" >&2; exit 1; }
    sleep 5
done
tools/mtp_push bin/Trenchfire-venux1.prg Trenchfire.prg
echo "installed: unplug the watch and open TRENCHFIRE from the app list"
