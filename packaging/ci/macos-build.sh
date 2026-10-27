#!/usr/bin/env bash
# macOS CI/local plugin build: Homebrew deps, cmake build, ctest, stage artifacts.
#
# Usage: packaging/ci/macos-build.sh [--build-type Release] [--artifacts-dir artifacts]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT}"

BUILD_TYPE="Release"
ARTIFACTS="artifacts"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --artifacts-dir) ARTIFACTS="$2"; shift 2 ;;
        -h|--help) sed -n '1,8p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

export HOMEBREW_NO_AUTO_UPDATE=1
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required on macOS (https://brew.sh)." >&2
    exit 1
fi

echo "==> Installing build dependencies (Homebrew)"
# ktexteditor headers: prefer the Homebrew formula when available; otherwise
# fall back to KDE Craft for a matching Kate ABI (see packaging/craft/kateai.py).
brew install cmake extra-cmake-modules ninja qtbase \
    kcoreaddons ki18n kconfig kconfigwidgets kxmlgui kwidgetsaddons 2>/dev/null || true
brew install ktexteditor 2>/dev/null || {
    echo "WARNING: no Homebrew ktexteditor formula; building against whatever kateai-platform.sh provides." >&2
    echo "If cmake cannot find KTextEditor, build with KDE Craft instead:" >&2
    echo "  https://develop.kde.org/docs/getting-started/building/craft/" >&2
}
if ! brew list --cask kate >/dev/null 2>&1; then
    brew install --cask kate 2>/dev/null || echo "WARNING: Kate.app cask not installed; continuing without it." >&2
fi

PREFIX_ARGS=""
for f in qtbase kcoreaddons ki18n kconfig kconfigwidgets kxmlgui kwidgetsaddons ktexteditor extra-cmake-modules; do
    if brew --prefix "$f" >/dev/null 2>&1; then
        PREFIX_ARGS="${PREFIX_ARGS:+${PREFIX_ARGS};}$(brew --prefix "$f")"
    fi
done

echo "==> Configuring (${BUILD_TYPE})"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"
BUILD_DIR="$(kateai_resolve_build_dir)"
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    ${PREFIX_ARGS:+-DCMAKE_PREFIX_PATH="${PREFIX_ARGS}"} \
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
