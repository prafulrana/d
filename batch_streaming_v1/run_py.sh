#!/bin/bash
set -euo pipefail

RTSP_PORT="${RTSP_PORT:-8554}"
PUBLIC_HOST="${PUBLIC_HOST:-127.0.0.1}"
BASE_UDP_PORT="${BASE_UDP_PORT:-5000}"
ENGINE_DIR="${ENGINE_DIR:-$PWD/models}"

mkdir -p "$ENGINE_DIR"

echo "Building Python DeepStream RTSP server image..."
docker build -f Dockerfile.python -t batch_streaming_py:latest .

echo "Running Python server..."
docker run --rm \
  --gpus all \
  --network host \
  -e RTSP_PORT="$RTSP_PORT" \
  -e PUBLIC_HOST="$PUBLIC_HOST" \
  -e BASE_UDP_PORT="$BASE_UDP_PORT" \
  -v "$ENGINE_DIR":/models \
  batch_streaming_py:latest

