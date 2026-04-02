#!/bin/sh
# restart_process.sh <process_name>
# Sends SIGHUP first (graceful reload), falls back to SIGTERM.

PROC="$1"

if [ -z "$PROC" ]; then
    echo "Usage: $0 <process_name>" >&2
    exit 1
fi

# Try graceful reload first
if pkill -HUP -x "$PROC" 2>/dev/null; then
    echo "Sent SIGHUP to $PROC"
    exit 0
fi

# Fall back to SIGTERM
if pkill -TERM -x "$PROC" 2>/dev/null; then
    echo "Sent SIGTERM to $PROC"
    exit 0
fi

echo "Process '$PROC' not found or could not be signalled" >&2
exit 1
