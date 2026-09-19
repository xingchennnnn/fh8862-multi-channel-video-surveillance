#!/bin/bash
#
# start.sh - Launch RTSP-to-WebRTC relay + frontend server
#
# Usage:
#   ./start.sh [board_ip]
#
# Architecture:
#   Board (live555 RTSP) → Mediamtx (RTSP→WebRTC) → Browser (WHEP)
#
# Requirements: bash, wget, tar, node or python3
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MEDIAMTX_VERSION="1.11.3"
MEDIAMTX_DIR="$SCRIPT_DIR/mediamtx"
MEDIAMTX_BIN="$MEDIAMTX_DIR/mediamtx"
MEDIAMTX_CONFIG="$SCRIPT_DIR/mediamtx.yml"

# Get Ubuntu machine IP on the LAN
UBUNTU_IP=$(ip addr show | grep -E "inet " | grep -v 127.0.0.1 | head -1 | awk '{print $2}' | cut -d/ -f1)
BOARD_IP="${1:-192.168.1.3}"

echo "============================================"
echo " RTSP Stream Player - WebRTC Relay Setup"
echo "============================================"
echo ""

# --- Detect Ubuntu IP ---
if [ -z "$UBUNTU_IP" ]; then
    echo "ERROR: Cannot detect LAN IP. Please set it manually."
    exit 1
fi
echo "[INFO] Ubuntu LAN IP: $UBUNTU_IP"

# --- Ask for board IP if not provided ---
if [ -z "$BOARD_IP" ]; then
    read -p "Enter board IP address (RTSP source): " BOARD_IP
fi
if [ -z "$BOARD_IP" ]; then
    echo "ERROR: Board IP is required."
    exit 1
fi
echo "[INFO] Board IP: $BOARD_IP"

# --- Download and setup Mediamtx ---
if [ ! -f "$MEDIAMTX_BIN" ]; then
    echo "[INFO] Downloading Mediamtx v${MEDIAMTX_VERSION}..."
    mkdir -p "$MEDIAMTX_DIR"

    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64)  MTX_ARCH="amd64" ;;
        aarch64) MTX_ARCH="arm64v8" ;;
        armv7l)  MTX_ARCH="armv7" ;;
        *)       echo "ERROR: Unsupported architecture: $ARCH"; exit 1 ;;
    esac

    MTX_URL="https://github.com/bluenviron/mediamtx/releases/download/v${MEDIAMTX_VERSION}/mediamtx_v${MEDIAMTX_VERSION}_linux_${MTX_ARCH}.tar.gz"
    MTX_TGZ="$MEDIAMTX_DIR/mediamtx.tar.gz"

    wget -q --show-progress -O "$MTX_TGZ" "$MTX_URL" || {
        echo "ERROR: Failed to download Mediamtx. Check network or try manually."
        echo "URL: $MTX_URL"
        exit 1
    }

    tar -xzf "$MTX_TGZ" -C "$MEDIAMTX_DIR"
    rm -f "$MTX_TGZ"
    chmod +x "$MEDIAMTX_BIN"
    echo "[INFO] Mediamtx installed to $MEDIAMTX_DIR"
else
    echo "[INFO] Mediamtx already installed: $MEDIAMTX_BIN"
fi

# --- Generate Mediamtx config with actual IPs ---
echo "[INFO] Generating Mediamtx configuration..."
sed -e "s/PLACEHOLDER_BOARD_IP/$BOARD_IP/g" \
    -e "s/PLACEHOLDER_UBUNTU_IP/$UBUNTU_IP/g" \
    "$MEDIAMTX_CONFIG" > "$MEDIAMTX_DIR/mediamtx.yml"

echo "[INFO] Config written to $MEDIAMTX_DIR/mediamtx.yml"

# --- Kill any existing processes on our ports ---
echo "[INFO] Checking for existing processes..."
EXISTING_MTX=$(lsof -ti:8889 2>/dev/null || true)
EXISTING_WEB=$(lsof -ti:8080 2>/dev/null || true)
[ -n "$EXISTING_MTX" ] && kill $EXISTING_MTX 2>/dev/null && echo "[WARN] Killed existing process on port 8889"
[ -n "$EXISTING_WEB" ] && kill $EXISTING_WEB 2>/dev/null && echo "[WARN] Killed existing process on port 8080"
sleep 1

# --- Start Mediamtx ---
echo "[INFO] Starting Mediamtx (RTSP→WebRTC relay)..."
cd "$MEDIAMTX_DIR"
nohup "$MEDIAMTX_BIN" > "$SCRIPT_DIR/mediamtx.log" 2>&1 &
MTX_PID=$!
echo $MTX_PID > "$SCRIPT_DIR/mediamtx.pid"
echo "[INFO] Mediamtx started (PID: $MTX_PID)"

# Wait for Mediamtx to be ready
echo "[INFO] Waiting for Mediamtx to start..."
for i in $(seq 1 15); do
    if wget -q -O /dev/null http://127.0.0.1:9997/v3/paths/list 2>/dev/null; then
        echo "[INFO] Mediamtx is ready"
        break
    fi
    if [ $i -eq 15 ]; then
        echo "[WARN] Mediamtx API not responding, but continuing..."
    fi
    sleep 1
done

# --- Start HTTP server for frontend ---
echo "[INFO] Starting frontend HTTP server on port 8080..."

if command -v node &> /dev/null; then
    echo "[INFO] Using Node.js server..."
    nohup node "$SCRIPT_DIR/server.js" > "$SCRIPT_DIR/server.log" 2>&1 &
    WEB_PID=$!
elif command -v python3 &> /dev/null; then
    echo "[INFO] Using Python HTTP server..."
    nohup python3 -m http.server 8080 --bind 0.0.0.0 > "$SCRIPT_DIR/server.log" 2>&1 &
    WEB_PID=$!
else
    echo "ERROR: Neither node nor python3 found."
    kill $MTX_PID 2>/dev/null
    exit 1
fi

echo $WEB_PID > "$SCRIPT_DIR/server.pid"
echo "[INFO] HTTP server started (PID: $WEB_PID)"

# --- Print access info ---
echo ""
echo "============================================"
echo " Setup Complete!"
echo "============================================"
echo ""
echo "  Frontend:  http://${UBUNTU_IP}:8080"
echo "  WHEP API:  http://${UBUNTU_IP}:8889/main/whep"
echo "  WHEP API:  http://${UBUNTU_IP}:8889/sub/whep"
echo ""
echo "  Mediamtx API: http://${UBUNTU_IP}:9997"
echo ""
echo "  Logs:"
echo "    Mediamtx: $SCRIPT_DIR/mediamtx.log"
echo "    Server:   $SCRIPT_DIR/server.log"
echo ""
echo "  To stop: kill \$(cat $SCRIPT_DIR/mediamtx.pid) \$(cat $SCRIPT_DIR/server.pid)"
echo ""
echo "  Open the Frontend URL in your Windows browser"
echo "  Enter board IP: $BOARD_IP, then click '连接'"
echo "============================================"

cd "$SCRIPT_DIR"
