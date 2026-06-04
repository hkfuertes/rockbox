#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-240p}"
IMAGE="${ROCKBOX_LOCAL_IMAGE:-rockbox-y1-local:amd64}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! docker info >/dev/null 2>&1; then
  if command -v colima >/dev/null 2>&1; then
    PROFILE="${COLIMA_PROFILE:-rockbox-rosetta}"
    echo "Docker is not running. Starting Colima profile '$PROFILE' with Rosetta..."
    colima start --profile "$PROFILE" \
      --arch aarch64 \
      --vm-type=vz \
      --vz-rosetta \
      --cpu "${COLIMA_CPU:-4}" \
      --memory "${COLIMA_MEMORY:-8}" \
      --disk "${COLIMA_DISK:-80}"
    docker context use "colima-$PROFILE" >/dev/null
  else
    echo "Docker is not running, and colima was not found." >&2
    exit 1
  fi
fi

arch="$(docker info --format '{{.Architecture}}' 2>/dev/null || true)"
if [[ "$arch" != "x86_64" && "$arch" != "amd64" ]]; then
  echo "Warning: Docker server arch is '$arch'. This build image runs linux/amd64; Docker may use emulation." >&2
fi

docker build --platform linux/amd64 -f "$ROOT/Dockerfile.local" -t "$IMAGE" "$ROOT"

mkdir -p "$ROOT/.docker-home" "$ROOT/android/build"

docker run --rm \
  --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -e HOME=/work/.docker-home \
  -e ANDROID_SDK_PATH=/opt/android/android-sdk \
  -e ANDROID_NDK_PATH=/opt/android/android-ndk-r10e \
  -e ANDROID_NDK_ROOT=/opt/android/android-ndk-r10e \
  -v "$ROOT:/work" \
  -w /work \
  "$IMAGE" \
  bash tools/local-build-y1.sh "$MODE"
