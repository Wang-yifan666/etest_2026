#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-5000}"
OUTPUT_DIR="${HOME}/ball_records"

mkdir -p "${OUTPUT_DIR}"

OUTPUT="${OUTPUT_DIR}/test_$(date +%Y%m%d_%H%M%S).mkv"

echo "Recording to: ${OUTPUT}"
echo "Press Ctrl+C to finish normally."

exec gst-launch-1.0 -e \
    udpsrc \
        port="${PORT}" \
        buffer-size=1048576 \
        caps='application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000' \
    ! rtpjpegdepay \
    ! jpegdec \
    ! videoconvert \
    ! tee name=t \
    \
    t. ! queue \
        leaky=downstream \
        max-size-buffers=1 \
        max-size-bytes=0 \
        max-size-time=0 \
      ! autovideosink sync=false \
    \
    t. ! queue \
        max-size-buffers=30 \
        max-size-bytes=0 \
        max-size-time=0 \
      ! x264enc \
        tune=zerolatency \
        speed-preset=ultrafast \
        bitrate=3000 \
        key-int-max=30 \
        bframes=0 \
      ! h264parse \
      ! matroskamux \
      ! filesink location="${OUTPUT}"