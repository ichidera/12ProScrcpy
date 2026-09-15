#!/usr/bin/env bash
# Cross-compiles qtscrcpy_raw_input_daemon for the device (aarch64 Android),
# statically linked, per docs/daemon-implementation-plan.md §1.3 step 2 and
# the "Cross-compile toolchain" open question in
# docs/raw-input-daemon-design.md.
#
# Requires the Android NDK (ANDROID_NDK_HOME or --ndk <path>). Not run as
# part of the normal CMake build - the daemon binary is a build artifact you
# produce once and drop into
# ../third_party/raw_input_daemon/qtscrcpy_raw_input_daemon, same as the
# adb/scrcpy-server binaries already bundled there.
#
# Usage:
#   ./build_raw_input_daemon.sh [--ndk /path/to/android-ndk] [--api 26]
#
set -euo pipefail

API_LEVEL=26
NDK_PATH="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ndk)
            NDK_PATH="$2"
            shift 2
            ;;
        --api)
            API_LEVEL="$2"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$NDK_PATH" ]]; then
    echo "error: Android NDK not found. Set ANDROID_NDK_HOME or pass --ndk <path>." >&2
    exit 1
fi

HOST_TAG="linux-x86_64"
case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
esac

TOOLCHAIN_BIN="${NDK_PATH}/toolchains/llvm/prebuilt/${HOST_TAG}/bin"
CLANG="${TOOLCHAIN_BIN}/aarch64-linux-android${API_LEVEL}-clang"

if [[ ! -x "$CLANG" ]]; then
    echo "error: expected compiler not found at $CLANG" >&2
    echo "       check --api/--ndk and that the NDK matches HOST_TAG=$HOST_TAG" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/../third_party/raw_input_daemon"
mkdir -p "$OUT_DIR"

"$CLANG" \
    -static \
    -O2 \
    -Wall -Wextra \
    -o "${OUT_DIR}/qtscrcpy_raw_input_daemon" \
    "${SCRIPT_DIR}/raw_input_daemon.c" \
    -lpthread

echo "built: ${OUT_DIR}/qtscrcpy_raw_input_daemon"
file "${OUT_DIR}/qtscrcpy_raw_input_daemon" 2>/dev/null || true
