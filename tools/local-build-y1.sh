#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/local-build-y1.sh [240p|360p|all|update]

Builds Rockbox Y1 locally inside the prepared Docker image/container.
Outputs:
  android/build/rockbox_240p.apk
  android/build/rockbox_360p.apk
  android/build/update_240p.zip      (for all/update)
  android/build/update_360p.zip      (for all/update)
EOF
}

MODE="${1:-240p}"
case "$MODE" in
  240p|360p|all|update) ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export ANDROID_SDK_PATH="${ANDROID_SDK_PATH:-/opt/android/android-sdk}"
export ANDROID_NDK_PATH="${ANDROID_NDK_PATH:-/opt/android/android-ndk-r10e}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_NDK_PATH}"
export PATH="$ANDROID_SDK_PATH/build-tools/35.0.0:$ANDROID_SDK_PATH/cmdline-tools/latest/bin:$PATH"

mkdir -p android/build
cd android/build

build_go_tools() {
  if [[ -x gocurl/gocurl && -x download-podcast/poddl ]]; then
    return
  fi

  export GOPATH="$PWD/go"
  export CGO_ENABLED=0
  export GOOS=linux
  export GOARCH=arm
  export GOARM=6

  if [[ ! -d gocurl ]]; then
    git clone https://github.com/rockbox-y1/gocurl.git
  fi
  git -C gocurl fetch --depth=1 origin json-data
  git -C gocurl checkout json-data
  (cd gocurl && go build -ldflags '-s -w' -o gocurl)

  if [[ ! -d download-podcast ]]; then
    git clone https://codeberg.org/rockbox-y1/download-podcast.git
  fi
  git -C download-podcast fetch --depth=1 origin patches
  git -C download-podcast checkout patches
  if [[ ! -d download-podcast/id3-go ]]; then
    git clone https://github.com/rockbox-y1/id3-go.git download-podcast/id3-go
  fi
  git -C download-podcast/id3-go fetch --depth=1 origin patches
  git -C download-podcast/id3-go checkout patches
  (cd download-podcast && go build -o poddl)
}

sign_apk() {
  local inout="$1"
  local apksigner=""
  if command -v apksigner >/dev/null 2>&1; then
    apksigner="apksigner"
  elif [[ -x "$ANDROID_SDK_PATH/build-tools/35.0.0/apksigner" ]]; then
    apksigner="$ANDROID_SDK_PATH/build-tools/35.0.0/apksigner"
  else
    echo "apksigner not found" >&2
    return 1
  fi
  "$apksigner" sign --key ../platform.pk8 --cert ../platform.x509.pem "$inout"
}

build_target() {
  local name="$1" width="$2" height="$3"
  echo "==> Building $name (${width}x${height})"
  ../../tools/configure --target=201 --lcdwidth="$width" --lcdheight="$height" --type=n
  make clean
  make
  make classes
  make zip
  make unsigned-apk
  sign_apk rockbox_unsigned.apk
  cp rockbox_unsigned.apk "rockbox_${name}.apk"
  rm -rf "libs_${name}"
  cp -r libs "libs_${name}"
}

bundle_updates() {
  echo "==> Bundling update zips"
  [[ -d libs_360p && -d libs_240p ]] || { echo "Need libs_360p and libs_240p; run all first" >&2; exit 1; }
  [[ -x gocurl/gocurl && -x download-podcast/poddl ]] || build_go_tools

  if [[ ! -f com.innioasis.y1_2.7.2.apk ]]; then
    wget https://github.com/rockbox-y1/rockbox/releases/download/stock-menu/com.innioasis.y1_2.7.2.apk
  fi

  for name in 360p 240p; do
    rm -rf "update_${name}" update
    mkdir "update_${name}"
    cp -r "libs_${name}" "update_${name}/libs"
    cp "rockbox_${name}.apk" "update_${name}/rockbox.apk"
    cp com.innioasis*.apk "update_${name}/."
    cp ../scripts/install-recovery.sh "update_${name}/."
    cp ../scripts/99Y1ButtonScript "update_${name}/."
    cp ../scripts/switch-to-stock.sh "update_${name}/."
    cp ../scripts/update.sh "update_${name}/."
    cp ../scripts/Rockbox.kl "update_${name}/."
    cp ../scripts/Stock.kl "update_${name}/."
    cp gocurl/gocurl "update_${name}/."
    cp download-podcast/poddl "update_${name}/."
    mv "update_${name}" update
    zip -r "update_${name}.zip" update/*
    rm -rf update
  done
}

case "$MODE" in
  240p) build_target 240p 320 240 ;;
  360p) build_target 360p 480 360 ;;
  all) build_go_tools; build_target 360p 480 360; build_target 240p 320 240; bundle_updates ;;
  update) build_go_tools; bundle_updates ;;
esac
