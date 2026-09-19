#!/bin/bash
# Stop all services started by start.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -f "$SCRIPT_DIR/mediamtx.pid" ]; then
    PID=$(cat "$SCRIPT_DIR/mediamtx.pid")
    kill $PID 2>/dev/null && echo "Stopped Mediamtx (PID: $PID)"
    rm -f "$SCRIPT_DIR/mediamtx.pid"
fi

if [ -f "$SCRIPT_DIR/server.pid" ]; then
    PID=$(cat "$SCRIPT_DIR/server.pid")
    kill $PID 2>/dev/null && echo "Stopped HTTP server (PID: $PID)"
    rm -f "$SCRIPT_DIR/server.pid"
fi

echo "All services stopped."
