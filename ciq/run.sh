#!/bin/sh
# Build, (re)start the simulator, push the app, and tail its console.
# monkeydo hangs if the simulator is already running an app, so always
# restart it.  Usage: ./run.sh [seconds-to-wait]
cd "$(dirname "$0")"
WAIT="${1:-25}"
./build.sh 2>&1 | grep -E 'ERROR|error:|BUILD' || exit 1
pkill -f MonkeyDoDeux; pkill -f "ConnectIQ.app/Contents/MacOS/simulator"; sleep 1
# Use the SDK Manager's copy of the simulator: the Homebrew cask moves the app
# to /Applications, where it can't find the SDK's version.txt and complains.
SDK_BIN="$(cat "$HOME/Library/Application Support/Garmin/ConnectIQ/current-sdk.cfg")bin"
nohup "$SDK_BIN/ConnectIQ.app/Contents/MacOS/simulator" > bin/simulator.log 2>&1 &
sleep 6
nohup monkeydo bin/DoomCE-venux1.prg venux1 > bin/sim.log 2>&1 &
sleep "$WAIT"
tail -4 bin/sim.log
