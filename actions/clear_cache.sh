#!/bin/sh
# clear_cache.sh
# Flushes Linux page cache, dentries and inodes.
# Requires root privileges.

# Flush file system buffers first so no dirty data is lost
sync

# 3 = drop page cache + dentries + inodes
echo 3 > /proc/sys/vm/drop_caches

echo "Page/slab caches dropped"
exit 0
