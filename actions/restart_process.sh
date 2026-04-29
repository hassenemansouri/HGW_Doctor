#!/bin/sh
# restart_process.sh <process_name>
# Restarts a service using its procd init script

PROC="$1"

if [ -z "$PROC" ]; then
    echo "Usage: $0 <process_name>" >&2
    exit 1
fi

# Get the base name without extension for the init script
INIT_NAME=$(basename "$PROC" .sh)

# Try using init script first
if [ -f "/etc/init.d/$INIT_NAME" ]; then
    logger -t hgw-doctor "Restarting service $INIT_NAME via init script"
    "/etc/init.d/$INIT_NAME" restart
    exit $?
fi

# Fall back to kill + let procd respawn
if pkill -TERM -x "$PROC" 2>/dev/null; then
    logger -t hgw-doctor "Sent SIGTERM to $PROC - procd will respawn"
    exit 0
fi

echo "Process '$PROC' not found" >&2
exit 1
