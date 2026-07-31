#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-5000}"

exec gst-launch-1.0 -q \
    udpsrc \
        port="${PORT}" \
        buffer-size=1048576 \
        caps='application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000' \
    ! rtpjpegdepay \
    ! jpegdec \
    ! queue \
        leaky=downstream \
        max-size-buffers=1 \
        max-size-bytes=0 \
        max-size-time=0 \
    ! videoconvert \
    ! autovideosink sync=false