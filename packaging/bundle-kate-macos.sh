#!/usr/bin/env bash
# Bundle the latest Kate (built via Craft) with this plugin on macOS.
#
# Result in packaging/dist/:
#   kate-<ver>-macos-<arch>.dmg      # latest Kate installer built by Craft
#   kateai-<ver>-macos-<arch>.zip    # this plugin (Craft/Clang ABI)
#
# Install: open the .dmg to install Kate, then run ./install.sh (it copies
# kateai.so into Kate.app and sets up the LaunchAgent).
#
# Usage: packaging/bundle-kate-macos.sh [--out-dir packaging/dist] [--skip-kate]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"

CRAFT_ROOT="${CRAFT_ROOT:-${HOME}/CraftRoot}"
OUT_DIR="${ROOT}/packaging/dist"
SKIP_KATE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --skip-kate) SKIP_KATE=1; shift ;;
        --craft-root) CRAFT_ROOT="$2"; shift 2 ;;
        -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.1.0}"
ARCH="$(uname -m)"

if [[ ! -f "${CRAFT_ROOT}/craft/craftenv.sh" ]]; then
    # Non-interactive: --use-defaults answers the setup wizard prompts.
    echo "==> Bootstrapping KDE Craft into ${CRAFT_ROOT}"
    curl https://raw.githubusercontent.com/KDE/craft/master/setup/CraftBootstrap.py -o /tmp/CraftBootstrap.py
    python3 /tmp/CraftBootstrap.py --prefix "${CRAFT_ROOT}" --use-defaults
fi
if [[ ! -f "${CRAFT_ROOT}/craft/craftenv.sh" ]]; then
    echo "KDE Craft bootstrap failed for ${CRAFT_ROOT}." >&2
    echo "See https://community.kde.org/Get_Involved/development/Mac" >&2
    exit 1
fi
# shellcheck source=/dev/null
source "${CRAFT_ROOT}/craft/craftenv.sh"

if [[ "${SKIP_KATE}" -eq 0 ]]; then
    echo "==> Building latest Kate via Craft"
    craft kate
fi

echo "==> Building plugin against ${CRAFT_ROOT}"
/usr/bin/env bash "${ROOT}/packaging/ci/macos-build.sh" --build-type Release --artifacts-dir "${ROOT}/artifacts-macos-tmp"

echo "==> Packaging Kate installer (.dmg) via Craft"
craft --package kate

mkdir -p "${OUT_DIR}"
DMG="$(find "${CRAFT_ROOT}/tmp" -maxdepth 1 -name 'kate-*.dmg' -print 2>/dev/null | sort | tail -1 || true)"
if [[ -n "${DMG}" && -f "${DMG}" ]]; then
    cp -f "${DMG}" "${OUT_DIR}/kate-${VERSION}-macos-${ARCH}.dmg"
else
    echo "WARNING: no kate-*.dmg found under ${CRAFT_ROOT}/tmp; shipping plugin only." >&2
fi
rm -f "${OUT_DIR}/kateai-${VERSION}-macos-${ARCH}.zip"
(cd "${ROOT}/artifacts-macos-tmp" && zip -j "${OUT_DIR}/kateai-${VERSION}-macos-${ARCH}.zip" kateai.so)
rm -rf "${ROOT}/artifacts-macos-tmp"
ls -l "${OUT_DIR}"/kate-*-macos-*.dmg "${OUT_DIR}"/kateai-*-macos-*.zip 2>/dev/null || true
echo "Bundle ready in ${OUT_DIR}"
