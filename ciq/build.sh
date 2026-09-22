#!/bin/sh
# Build DOOMCE for a Connect IQ device.  Usage: ./build.sh [device]  (default venux1)
# Needs the Connect IQ SDK on PATH (brew install --cask connectiq) and the
# device definition installed via the SDK Manager (brew install --cask
# connectiq-sdk-manager, sign in, Devices tab, download "Venu X1").
set -e
cd "$(dirname "$0")"
DEV="${1:-venux1}"
if [ ! -f developer_key ]; then
    echo "generating developer key"
    openssl genrsa -out developer_key.pem 4096 2>/dev/null
    openssl pkcs8 -topk8 -inform PEM -outform DER -in developer_key.pem -out developer_key -nocrypt
fi
mkdir -p bin
monkeyc -o "bin/DoomCE-$DEV.prg" -f monkey.jungle -y developer_key -d "$DEV" -l 0
echo "built bin/DoomCE-$DEV.prg"
