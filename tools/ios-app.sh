#!/usr/bin/env bash
#
# Build the vCAD iPad app, install it on a connected iPad, and launch it.
#
# The sibling of tools/ios-test.sh, and deliberately the same shape: same build directory, same
# team discovery, same devicectl round trip. Sharing build-ios/ is not laziness — it is what lets
# the app reuse the vcpkg tree the test bundle already built, which is the multi-hour part.
#
# Usage:  tools/ios-app.sh [device-udid]
#         With no argument it picks the first available iPad.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/Developer/vcpkg}"
BUILD_DIR="$ROOT/build-ios"
BUNDLE_ID="dev.vcad.app"

# The team is the certificate's ORGANISATIONAL UNIT, not the parenthesised string in its common
# name. See the long comment in ios-test.sh for why that distinction has its own paragraph.
TEAM="${CAD_IOS_TEAM:-$(security find-certificate -c "Apple Development" -p 2>/dev/null \
    | openssl x509 -noout -subject 2>/dev/null \
    | sed -n 's/.*OU=\([A-Z0-9]*\).*/\1/p' | head -1)}"
if [[ -z "$TEAM" ]]; then
    echo "No Apple Development signing identity found. Open Xcode and add your Apple ID." >&2
    exit 1
fi

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    # Matched by SHAPE, not by column position. `devicectl list devices` is a fixed-width table
    # whose Name and Model columns both contain spaces ("Vg's iPad", "iPad Pro 11-inch (M5)"), so
    # counting fields from either end picks a different token per device — it picked the word "Pro"
    # here, and the error that produced named a device called "Pro".
    DEVICE="$(xcrun devicectl list devices 2>/dev/null \
        | grep iPad | grep 'available' \
        | grep -oE '[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}' | head -1)"
fi
if [[ -z "$DEVICE" ]]; then
    echo "No available iPad found. Connect one and trust this Mac." >&2
    exit 1
fi

SHADERC="$(find "$ROOT/build" -name shaderc -type f -perm -u+x 2>/dev/null | head -1)"
if [[ -n "$SHADERC" ]]; then
    SHADERC_ARG="-DBGFX_SHADERC=$SHADERC"
fi

echo "device:  $DEVICE"
echo "team:    $TEAM"

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
    -DCAD_BUILD_SHELL_IOS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    ${SHADERC_ARG:-}

xcodebuild -project "$BUILD_DIR/CAD.xcodeproj" \
    -target vcad_ios \
    -configuration Release \
    -destination "generic/platform=iOS" \
    -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$TEAM" \
    CODE_SIGN_STYLE=Automatic \
    build

APP="$(find "$BUILD_DIR" -name 'vCAD.app' -type d | head -1)"
if [[ -z "$APP" ]]; then
    echo "Build produced no app bundle." >&2
    exit 1
fi
echo "bundle:  $APP"

xcrun devicectl device install app --device "$DEVICE" "$APP"

# No --console: unlike the test bundle, this app is interactive and does not exit. Launching it
# and returning is the whole point; the user drives it from here.
xcrun devicectl device process launch \
    --device "$DEVICE" \
    --terminate-existing \
    "$BUNDLE_ID"
