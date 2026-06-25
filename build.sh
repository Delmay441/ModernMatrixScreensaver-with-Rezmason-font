#!/bin/bash
# Build script for the Matrix Rain screensaver + dev harness.
# No .xcodeproj: compiles Swift with swiftc, shaders with the Metal toolchain,
# assembles a .saver bundle and a .app harness, and ad-hoc codesigns both.
#
# Usage:
#   ./build.sh            # build everything (debug)
#   ./build.sh harness    # just the dev-harness app
#   ./build.sh saver      # just the .saver bundle
#   ./build.sh run        # build harness and launch it
#   ./build.sh snapshot   # build harness and render a frame to build/snapshot.png
#   ./build.sh install    # build saver (release) and install to ~/Library/Screen Savers
#   ./build.sh clean
#
# Env: CONFIG=release for optimized builds (default: debug).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
SDK="$(xcrun --show-sdk-path)"
DEPLOY_TARGET="arm64-apple-macosx26.0"
CONFIG="${CONFIG:-debug}"

APP="$BUILD/Modern Matrix.app"
SAVER="$BUILD/Modern Matrix.saver"
METALLIB="$BUILD/default.metallib"

CORE_SOURCES=( "$ROOT"/Sources/Core/*.swift )

# Shared C engine (mmcore) — compiled with clang, bridged into Swift via the header
# below, and linked into both the .app and the .saver. The SAME .c files compile on
# Windows for the .scr port, so rain behaviour is edited once (see PORTING.md).
CORE_C_SOURCES=( "$ROOT"/core/*.c )
BRIDGING_HEADER="$ROOT/core/mmcore.h"

if [[ "$CONFIG" == "release" ]]; then
  SWIFT_OPT=( -O -wmo )
  METAL_OPT=( -O )
  CC_OPT=( -O2 )
else
  SWIFT_OPT=( -Onone -g )
  METAL_OPT=( )
  CC_OPT=( -O0 -g )
fi

SWIFTC=( swiftc -sdk "$SDK" -target "$DEPLOY_TARGET" -swift-version 5 "${SWIFT_OPT[@]}" )

log() { printf '\033[1;32m▸ %s\033[0m\n' "$*"; }

compile_metal() {
  log "Compiling Metal shaders → default.metallib"
  mkdir -p "$BUILD"
  xcrun -sdk macosx metal "${METAL_OPT[@]+"${METAL_OPT[@]}"}" -frecord-sources \
    -c "$ROOT/Resources/Shaders.metal" -o "$BUILD/Shaders.air"
  xcrun -sdk macosx metallib "$BUILD/Shaders.air" -o "$METALLIB"
}

compile_core() {
  log "Compiling shared C core → mmcore"
  mkdir -p "$BUILD"
  MMCORE_OBJS=()
  local src obj
  for src in "${CORE_C_SOURCES[@]}"; do
    obj="$BUILD/$(basename "${src%.c}").o"
    clang -std=c99 -Wall -c -arch arm64 -isysroot "$SDK" -target "$DEPLOY_TARGET" \
      "${CC_OPT[@]}" "$src" -o "$obj"
    MMCORE_OBJS+=( "$obj" )
  done
}

build_harness() {
  compile_metal
  compile_core
  log "Building app → $(basename "$APP")"
  rm -rf "$APP"
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  "${SWIFTC[@]}" \
    -module-name MatrixRainHarness \
    -import-objc-header "$BRIDGING_HEADER" \
    "${CORE_SOURCES[@]}" "$ROOT"/Sources/Harness/*.swift \
    "${MMCORE_OBJS[@]+"${MMCORE_OBJS[@]}"}" \
    -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore \
    -framework CoreText -framework CoreGraphics -framework CoreImage \
    -framework SwiftUI -framework ScreenSaver \
    -o "$APP/Contents/MacOS/MatrixRainHarness"
  rm -rf "$APP/Contents/MacOS/MatrixRainHarness.dSYM"
  cp "$ROOT/Sources/Harness/Info.plist" "$APP/Contents/Info.plist"
  cp "$METALLIB" "$APP/Contents/Resources/default.metallib"
  codesign --force --sign - "$APP" >/dev/null 2>&1 || true
  log "App ready: $APP"
}

build_saver() {
  compile_metal
  compile_core
  log "Building screensaver → $(basename "$SAVER")"
  rm -rf "$SAVER"
  mkdir -p "$SAVER/Contents/MacOS" "$SAVER/Contents/Resources"
  # A .saver executable must be a Mach-O bundle (MH_BUNDLE): -Xlinker -bundle.
  "${SWIFTC[@]}" \
    -module-name MatrixRain \
    -emit-executable -Xlinker -bundle \
    -import-objc-header "$BRIDGING_HEADER" \
    "${CORE_SOURCES[@]}" "$ROOT"/Sources/Saver/*.swift \
    "${MMCORE_OBJS[@]+"${MMCORE_OBJS[@]}"}" \
    -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore \
    -framework CoreText -framework CoreGraphics -framework CoreImage \
    -framework ScreenSaver -framework SwiftUI \
    -o "$SAVER/Contents/MacOS/MatrixRain"
  rm -rf "$SAVER/Contents/MacOS/MatrixRain.dSYM"
  cp "$ROOT/Sources/Saver/Info.plist" "$SAVER/Contents/Info.plist"
  cp "$METALLIB" "$SAVER/Contents/Resources/default.metallib"
  # Picker thumbnail (the wallpaper agent shows this in the grid; same convention as Apple savers).
  cp "$ROOT/Sources/Saver/thumbnail.png" "$SAVER/Contents/Resources/thumbnail.png"
  cp "$ROOT/Sources/Saver/thumbnail@2x.png" "$SAVER/Contents/Resources/thumbnail@2x.png"
  codesign --force --sign - "$SAVER" >/dev/null 2>&1 || true
  log "Mach-O type: $(otool -hv "$SAVER/Contents/MacOS/MatrixRain" | awk 'NR==4{print $5}')"
  log "Screensaver ready: $SAVER"
}

case "${1:-all}" in
  metal)    compile_metal ;;
  harness)  build_harness ;;
  saver)    build_saver ;;
  run)      build_harness; open "$APP" ;;
  snapshot) build_harness; shift; "$APP/Contents/MacOS/MatrixRainHarness" --snapshot "${1:-$BUILD/snapshot.png}" ;;
  install)
            CONFIG=release build_saver
            CONFIG=release build_harness
            DEST="$HOME/Library/Screen Savers"
            mkdir -p "$DEST"
            rm -rf "$DEST/MatrixRain.saver" "$DEST/Modern Matrix.saver"
            cp -R "$SAVER" "$DEST/"
            log "Installed screensaver → $DEST/$(basename "$SAVER")"
            rm -rf "/Applications/Matrix Rain.app" "/Applications/Modern Matrix.app" 2>/dev/null
            if cp -R "$APP" "/Applications/" 2>/dev/null; then
              log "Installed app → /Applications/$(basename "$APP")"
            else
              mkdir -p "$HOME/Applications"
              rm -rf "$HOME/Applications/Matrix Rain.app" "$HOME/Applications/Modern Matrix.app"
              cp -R "$APP" "$HOME/Applications/"
              log "Installed app → $HOME/Applications/$(basename "$APP")"
            fi
            ;;
  clean)    rm -rf "$BUILD"; log "Cleaned." ;;
  all|*)    build_harness; build_saver ;;
esac
