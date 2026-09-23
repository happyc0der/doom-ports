#!/bin/sh
# Build, (re)start the simulator, push the app, and show its console.
# Usage: ./run.sh [seconds-to-wait]   (default 25)
#
# The simulator is restarted every time: monkeydo hangs silently if an app is
# already running in it.  The SDK Manager's copy of the simulator is used -
# the one the Homebrew cask puts in /Applications cannot find the SDK's
# version.txt and shows an error dialog on every launch.
set -e
cd "$(dirname "$0")"
WAIT="${1:-25}"
./build.sh
SDK="$(cat "$HOME/Library/Application Support/Garmin/ConnectIQ/current-sdk.cfg")"
pkill -f MonkeyDoDeux 2>/dev/null || true
pkill -f "ConnectIQ.app/Contents/MacOS/simulator" 2>/dev/null || true
sleep 1
nohup "${SDK}bin/ConnectIQ.app/Contents/MacOS/simulator" > bin/simulator.log 2>&1 &
sleep 6
nohup "${SDK}bin/monkeydo" bin/Trenchfire-venux1.prg venux1 > bin/sim.log 2>&1 &
sleep "$WAIT"
tail -4 bin/sim.log
