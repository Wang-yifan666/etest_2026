#!/bin/bash
set -euo pipefail

SERVICE_NAME="etest_2026"
SERVICE_FILE="scripts/etest_2026.service"
SYSTEMD_DIR="/etc/systemd/system"

echo "[install_service] checking prerequisites..."

if [ ! -f "$SERVICE_FILE" ]; then
    echo "ERROR: service file not found: $SERVICE_FILE" >&2
    exit 1
fi

if [ ! -f "build/etest_2026" ]; then
    echo "ERROR: executable not found: build/etest_2026" >&2
    echo "Please run: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
fi

if [ ! -d "config" ]; then
    echo "ERROR: config directory not found" >&2
    exit 1
fi

WORKDIR=$(pwd)
EXECUTABLE="$WORKDIR/build/etest_2026"
CONFIGDIR="$WORKDIR/config"
USER=$(whoami)

echo "[install_service] workdir: $WORKDIR"
echo "[install_service] executable: $EXECUTABLE"
echo "[install_service] config: $CONFIGDIR"
echo "[install_service] user: $USER"

TEMP_SERVICE=$(mktemp)
cp "$SERVICE_FILE" "$TEMP_SERVICE"

sed -i "s|REPLACE_WITH_USER|$USER|g" "$TEMP_SERVICE"
sed -i "s|REPLACE_WITH_WORKDIR|$WORKDIR|g" "$TEMP_SERVICE"
sed -i "s|REPLACE_WITH_EXECUTABLE|$EXECUTABLE|g" "$TEMP_SERVICE"
sed -i "s|REPLACE_WITH_CONFIGDIR|$CONFIGDIR|g" "$TEMP_SERVICE"

DEST="$SYSTEMD_DIR/${SERVICE_NAME}.service"
echo "[install_service] installing to $DEST"

sudo cp "$TEMP_SERVICE" "$DEST"
rm -f "$TEMP_SERVICE"

sudo systemctl daemon-reload
echo "[install_service] daemon reloaded"

sudo systemctl enable "$SERVICE_NAME"
echo "[install_service] enabled $SERVICE_NAME"

echo "[install_service] done. Start with: sudo systemctl start $SERVICE_NAME"
echo "[install_service] Status: sudo systemctl status $SERVICE_NAME"