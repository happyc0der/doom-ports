#!/bin/sh
# Pull the on-device log (GARMIN/Apps/LOGS/Trenchfire.TXT) over MTP and print the
# frame profile lines.   Usage: tools/pull_log.sh [out-file]
#
# The watch only writes the log if that file already exists; create it once:
#   : > /tmp/Trenchfire.TXT && tools/mtp_push /tmp/Trenchfire.TXT Trenchfire.TXT GARMIN/Apps/LOGS
# and build with PROFILE = true in Engine.mc.  The log is appended to across runs.
set -e
export LANG="${LANG:-en_US.UTF-8}"
OUT="${1:-/tmp/Trenchfire_device.TXT}"
i=0
until mtp-detect 2>/dev/null | grep -q "Garmin"; do
    i=$((i + 1)); [ "$i" -gt 12 ] && { echo "no watch on USB after 60 s" >&2; exit 1; }
    sleep 5
done
ID="$(mtp-files 2>/dev/null | grep -B1 "Filename: Trenchfire.TXT" | grep "File ID" | awk '{print $3}')"
[ -n "$ID" ] || { echo "no GARMIN/Apps/LOGS/Trenchfire.TXT on the watch (see the comment above)" >&2; exit 1; }
mtp-getfile "$ID" "$OUT" > /dev/null 2>&1
echo "saved $OUT ($(wc -l < "$OUT" | tr -d ' ') lines); frame lines:"
grep "fps=" "$OUT" | tail -20
grep "fps=" "$OUT" | awk '{split($1,f,"="); split($4,c,"="); if (c[2]+0 >= 6) {s++; sf+=f[2]} else {m++; mf+=f[2]}} END {if (s+m) printf "still: %d samples avg %.1f fps | moving: %d samples avg %.1f fps\n", s, (s?sf/s:0), m, (m?mf/m:0)}'
