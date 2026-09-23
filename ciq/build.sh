#!/bin/sh
# Build TRENCHFIRE for a Connect IQ device.   Usage: ./build.sh [device]   (default venux1)
#
# Uses the SDK the Connect IQ SDK Manager marked current (it also installs the
# device definitions), falling back to a monkeyc on PATH.  Needs a JDK.
set -e
cd "$(dirname "$0")"
DEV="${1:-venux1}"

CFG="$HOME/Library/Application Support/Garmin/ConnectIQ/current-sdk.cfg"
if [ -f "$CFG" ] && [ -x "$(cat "$CFG")bin/monkeyc" ]; then
    MONKEYC="$(cat "$CFG")bin/monkeyc"
else
    MONKEYC="$(command -v monkeyc || true)"
fi
if [ -z "$MONKEYC" ]; then
    echo "monkeyc not found: install the Connect IQ SDK Manager, sign in, download an SDK" >&2
    exit 1
fi
if [ ! -d "$HOME/Library/Application Support/Garmin/ConnectIQ/Devices/$DEV" ]; then
    echo "device '$DEV' not installed: SDK Manager -> Devices -> download it" >&2
    exit 1
fi

if [ ! -f developer_key ]; then
    echo "generating developer key"
    openssl genrsa -out developer_key.pem 4096 2>/dev/null
    openssl pkcs8 -topk8 -inform PEM -outform DER -in developer_key.pem -out developer_key -nocrypt
fi
mkdir -p bin
"$MONKEYC" -o "bin/Trenchfire-$DEV.prg" -f monkey.jungle -y developer_key -d "$DEV" -l 0 2>&1 | grep -v WARNING || true
[ -f "bin/Trenchfire-$DEV.prg" ] && [ -z "$(find source -newer "bin/Trenchfire-$DEV.prg")" ] || { echo "BUILD FAILED" >&2; exit 1; }
echo "built bin/Trenchfire-$DEV.prg"
