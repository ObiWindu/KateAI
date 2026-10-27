#!/usr/bin/env bash
# macOS CI/local plugin build with KDE Craft (matching Kate ABI).
#
# There is no Homebrew ktexteditor formula, so a brew-only build cannot find
# KTextEditor (see scripts/kateai-platform.sh and install.sh, which point at
# Craft for the same reason). This script follows
# https://community.kde.org/Get_Involved/development/Mac :
#   1. Bootstrap Craft if missing (non-interactive, ~/CraftRoot).
#   2. `craft kate`              -> latest Kate + Qt/KF6 stack.
#   3. cmake build of this repo against ~/CraftRoot + ctest.
#   4. Stage artifacts/kateai.so.
#
# Usage: packaging/ci/macos-build.sh [--build-type Release] [--artifacts-dir artifacts]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT}"

# GitHub macOS images export ANDROID_SDK_ROOT/ANDROID_NDK, which makes
# CraftBootstrap.isAndroid() true and produces an android-clang-arm64 ABI.
# KateAI does not build for Android: drop those markers for everything
# Craft-related in this script.
unset ANDROID_SDK_ROOT ANDROID_NDK

export HOMEBREW_NO_AUTO_UPDATE=1
# cmake/ninja are not guaranteed on the runner image (and nothing else
# installs them since the brew-deps approach was replaced by Craft).
# Idempotent: a no-op when already present.
for tool in cmake ninja git pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || brew install "$tool"
done

BUILD_TYPE="Release"
ARTIFACTS="artifacts"
CRAFT_ROOT="${CRAFT_ROOT:-${HOME}/CraftRoot}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --artifacts-dir) ARTIFACTS="$2"; shift 2 ;;
        --craft-root) CRAFT_ROOT="$2"; shift 2 ;;
        -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ ! -f "${CRAFT_ROOT}/craft/craftenv.sh" ]]; then
    # Non-interactive: --use-defaults answers the setup wizard prompts
    # (target arch auto-detected, e.g. arm64 on Apple Silicon runners).
    echo "==> Bootstrapping KDE Craft into ${CRAFT_ROOT}"
    curl https://raw.githubusercontent.com/KDE/craft/master/setup/CraftBootstrap.py -o /tmp/CraftBootstrap.py
    python3 /tmp/CraftBootstrap.py --prefix "${CRAFT_ROOT}" --use-defaults
fi

echo "==> Loading Craft environment"
# craftenv.sh is not written with `set -u` in mind (unbound
# CRAFT_PYTHON_BIN aborts the source); relax nounset across the source.
set +u
# shellcheck source=/dev/null
source "${CRAFT_ROOT}/craft/craftenv.sh"
set -u

echo "==> Installing latest Kate + deps via Craft (binary cache first; long on first run)"
craft kate

echo "==> Configuring plugin (${BUILD_TYPE}) against ${CRAFT_ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"
# shellcheck source=scripts/kateai-build-dir.sh
source "${ROOT}/scripts/kateai-build-dir.sh"
BUILD_DIR="$(kateai_resolve_build_dir)"
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH="${CRAFT_ROOT}" \
    -DKATEAI_USER_INSTALL=ON

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"$(kateai_jobs)"

echo "==> Tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "==> Staging artifacts"
mkdir -p "${ARTIFACTS}"
BUILT="$(kateai_find_built_plugin "${BUILD_DIR}")" || {
    echo "Build did not produce the plugin library under ${BUILD_DIR}" >&2
    exit 1
}
cp -f "${BUILT}" "${ARTIFACTS}/kateai.so"
ls -l "${ARTIFACTS}/kateai.so"
