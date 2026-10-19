#!/bin/bash
# Reboot the console onto the build on the medium (xnet mode), wait for XBDM on the
# card's address, and start the plugin's TCP throughput server.
set -e
here=$(dirname "$0")
host=${XE_HOST:-192.168.1.210}
xr=${XERUNNER:-$here/../../xerunner/.venv/bin/xerunner}
"$xr" console reboot --host "$host" --wait 5 --json >/dev/null 2>&1 || true
sleep 20
i=0
until timeout 1 bash -c "exec 3<>/dev/tcp/$host/730" 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 60 ] && { echo "console never came back on $host"; exit 1; }
    sleep 2
done
XE_HOST=$host "$here/xectl.py" cmd 14
