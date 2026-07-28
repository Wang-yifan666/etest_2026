#!/usr/bin/env bash
set -u

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

cd "$PROJECT_DIR" || {
    echo "[ERROR] cannot enter project directory: $PROJECT_DIR" >&2
    exit 1
}

exec ./build/etest_2026 --config-dir "$PROJECT_DIR/config"
