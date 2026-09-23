#!/bin/sh
# Build the store package: bin/Trenchfire.iq, a release (debug-stripped) build for
# every product in manifest.xml, signed with developer_key.  Upload this file
# at https://apps.garmin.com/developer/ (Add an App).
set -e
cd "$(dirname "$0")"
[ -f developer_key ] || ./build.sh
SDK="$(cat "$HOME/Library/Application Support/Garmin/ConnectIQ/current-sdk.cfg")"
mkdir -p bin
"${SDK}bin/monkeyc" -e -r -o bin/Trenchfire.iq -f monkey.jungle -y developer_key 2>&1 | grep -v WARNING || true
[ -f bin/Trenchfire.iq ] || { echo "export failed" >&2; exit 1; }
echo "store package: bin/Trenchfire.iq ($(wc -c < bin/Trenchfire.iq | tr -d ' ') bytes)"
