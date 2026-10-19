#!/bin/bash
# FTP to and from the console's hard disk, what installing a game does: upload a
# file from the PC's RAM to Hdd1, then download it back to /dev/null. The rate is
# curl's own average over the whole transfer, so it includes the disk.
#
#   tests/ftpbench.sh [up|down|both] [MB]       default both, 256
#
# Environment: XE_HOST (192.168.1.210), XE_FTP (xboxftp:xboxftp), XE_DIR (Hdd1/xedrivers).
set -e
host=${XE_HOST:-192.168.1.210}
cred=${XE_FTP:-xboxftp:xboxftp}
dir=${XE_DIR:-Hdd1/xedrivers}
which=${1:-both}
mb=${2:-256}
src=/dev/shm/xedrivers/t$mb.bin
mkdir -p /dev/shm/xedrivers
[ -f "$src" ] || head -c $((mb << 20)) /dev/urandom > "$src"
url="ftp://$cred@$host/$dir/t$mb.bin"
rate() { awk -v b="$1" 'BEGIN { printf "%.1f MB/s", b / 1e6 }'; }
if [ "$which" != down ]; then
    s=$(curl -s --ftp-create-dirs --max-time 600 -T "$src" -w '%{speed_upload}' "$url")
    echo "up   PC->console disk  $mb MB  $(LC_ALL=C rate "$s")"
fi
if [ "$which" != up ]; then
    s=$(curl -s --max-time 600 -o /dev/null -w '%{speed_download}' "$url")
    echo "down console disk->PC  $mb MB  $(LC_ALL=C rate "$s")"
fi
