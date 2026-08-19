#!/usr/bin/env bash
#
# Build the acceptance suite for iOS, install it on a connected iPad, and stream its output back.
#
# WHY A SCRIPT AND NOT A README PARAGRAPH
#
# The device round trip is five tools deep — vcpkg for the dependencies, CMake's iOS toolchain,
# xcodebuild for signing, devicectl to install, devicectl again to launch and capture stdout — and
# getting any one of them wrong produces a failure that looks like the code's fault rather than the
# pipeline's. A script is the difference between "the core passes on iPad" being a claim someone
# reproduces and a claim someone remembers making.
#
# WHAT IT PROVES, AND WHAT IT DOES NOT
#
# It runs the SAME sources as the desktop suite, unmodified. What it cannot run, it skips out loud:
# a sandboxed app may not fork, may not dlopen code it did not ship, and cannot see this Mac's
# filesystem where the fixture corpus lives. Those print as skips with reasons — see
# tests/acceptance/Platform.h for why they are skips rather than #ifs.
#
# Usage:  tools/ios-test.sh [device-udid]
#         With no argument it picks the first available iPad.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/Developer/vcpkg}"
BUILD_DIR="$ROOT/build-ios"
BUNDLE_ID="dev.vcad.tests"

# The signing team, read from the keychain rather than hard-coded: a checkout on another machine
# has a different one, and a wrong team fails deep inside xcodebuild with an unhelpful message.
TEAM="${CAD_IOS_TEAM:-$(security find-identity -v -p codesigning \
    | sed -n 's/.*Apple Development: .*(\([A-Z0-9]*\)).*/\1/p' | head -1)}"
if [[ -z "$TEAM" ]]; then
    echo "No Apple Development signing identity found. Open Xcode and add your Apple ID." >&2
    exit 1
fi

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    DEVICE="$(xcrun devicectl list devices 2>/dev/null \
        | awk '/iPad/ && /available/ {print $(NF-3); exit}')"
fi
if [[ -z "$DEVICE" ]]; then
    echo "No available iPad found. Connect one and trust this Mac." >&2
    exit 1
fi
# shaderc is a HOST tool. A cross build's host triplet does not necessarily install it, so reuse the
# desktop build's if one is there — the alternative is failing at the shader step for a tool that
# has nothing to do with the target.
SHADERC="$(find "$ROOT/build" -name shaderc -type f -perm -u+x 2>/dev/null | head -1)"
if [[ -n "$SHADERC" ]]; then
    SHADERC_ARG="-DBGFX_SHADERC=$SHADERC"
    echo "shaderc: $SHADERC"
fi

echo "device:  $DEVICE"
echo "team:    $TEAM"

# ── Configure ────────────────────────────────────────────────────────────────────────────
#
# Xcode generator, not Ninja: signing an app bundle is an Xcode build-setting concern, and the
# generator is what makes those settings mean anything.
#
# The renderer is ON because the acceptance suite exercises it through NullBackend, which needs no
# Metal surface — the point is to prove the SCENE logic ports, not to draw a frame. The Qt shell and
# the spikes are off; neither exists on this platform.
cmake -S "$ROOT" -B "$BUILD_DIR" -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=arm64-ios \
    -DVCPKG_INSTALLED_DIR="$BUILD_DIR/vcpkg_installed" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCAD_BUILD_RENDER=ON \
    -DCAD_BUILD_TESTS=ON \
    -DCAD_BUILD_SPIKES=OFF \
    -DCAD_BUILD_SHELL_QT=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    ${SHADERC_ARG:-}

# ── Build and sign ───────────────────────────────────────────────────────────────────────
#
# -allowProvisioningUpdates lets Xcode mint a development profile on first run; without it a machine
# that has never built for this device fails with a provisioning error and no way forward.
xcodebuild -project "$BUILD_DIR/CAD.xcodeproj" \
    -target cad_tests \
    -configuration Release \
    -destination "generic/platform=iOS" \
    -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$TEAM" \
    CODE_SIGN_STYLE=Automatic \
    build

APP="$(find "$BUILD_DIR" -name 'cad_tests.app' -type d | head -1)"
if [[ -z "$APP" ]]; then
    echo "Build produced no app bundle." >&2
    exit 1
fi
echo "bundle:  $APP"

# ── Install and run ──────────────────────────────────────────────────────────────────────
xcrun devicectl device install app --device "$DEVICE" "$APP"

# --console streams stdout back over the wire, which is the only way to read Catch2's report from a
# device. The suite's exit code comes back through devicectl.
xcrun devicectl device process launch \
    --device "$DEVICE" \
    --console \
    --terminate-existing \
    "$BUNDLE_ID"
