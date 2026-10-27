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
# shellcheck source=/dev/null
source "${CRAFT_ROOT}/craft/craftenv.sh"

echo "==> Installing latest Kate + deps via Craft (binary cache first; long on first run)"
craft kate

echo "==> Configuring plugin (${BUILD_TYPE}) against ${CRAFT_ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"
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
