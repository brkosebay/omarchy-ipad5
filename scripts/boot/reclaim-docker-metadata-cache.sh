#!/usr/bin/env bash
# Host Docker VM only. Reclaim unused inode/dentry cache after global OOM
# caused by fragmented RAM and multi-GiB SReclaimable. No files are removed,
# no persistent VM settings change, and no service/process is stopped.
set -euo pipefail
docker run --rm --privileged linux-a9-kbuild sh -euc '
sync
echo 2 > /proc/sys/vm/drop_caches
grep -E "MemFree|MemAvailable|Slab|SReclaimable" /proc/meminfo
cat /proc/buddyinfo
'
