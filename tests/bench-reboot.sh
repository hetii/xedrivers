#!/bin/sh
# Reboot the console onto whatever build is on the medium, wait for the plugin to
# bring the card up, then run tests/speedtest.py with the arguments given.
# XE_HOST is where XBDM answers (the onboard port in standalone mode).
set -e
here=$(dirname "$0")
xr=${XERUNNER:-$here/../../xerunner/.venv/bin/xerunner}
"$xr" console reboot --host "$XE_HOST" --wait 120 --json >/dev/null
i=0
until "$here/xectl.py" status 2>/dev/null | grep -q "link=1"; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "card never got a link"; exit 1; }
    sleep 2
done
"$here/xectl.py" status | sed -n 4p
exec "$here/speedtest.py" "$@"
