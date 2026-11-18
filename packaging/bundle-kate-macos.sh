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
# GitHub macOS images export ANDROID_SDK_ROOT/ANDROID_NDK, which makes
# CraftBootstrap.isAndroid() true and produces an android-clang-arm64 ABI.
# KateAI does not build for Android: drop those markers for everything
# Craft-related in this script (also inherited by macos-build.sh below).
unset ANDROID_SDK_ROOT ANDROID_NDK
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"

CRAFT_ROOT="${CRAFT_ROOT:-${HOME}/CraftRoot}"
OUT_DIR="${ROOT}/packaging/dist"
SKIP_KATE=0
PLUGIN_SO=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --skip-kate) SKIP_KATE=1; shift ;;
        --craft-root) CRAFT_ROOT="$2"; shift 2 ;;
        --plugin-so) PLUGIN_SO="$2"; shift 2 ;;
        -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.5.1}"
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
echo "==> Loading Craft environment"
# craftenv.sh is not written with `set -u` in mind (unbound
# CRAFT_PYTHON_BIN aborts the source); relax nounset across the source.
set +u
# shellcheck source=/dev/null
source "${CRAFT_ROOT}/craft/craftenv.sh"
set -u

if [[ "${SKIP_KATE}" -eq 0 ]]; then
    echo "==> Building latest Kate via Craft"
    craft kate
fi

echo "==> Building plugin against ${CRAFT_ROOT}"
mkdir -p "${ROOT}/artifacts-macos-tmp"
if [[ -n "${PLUGIN_SO}" && ! -f "${PLUGIN_SO}" ]]; then
    # actions/download-artifact has changed its directory layout across
    # versions. Accept the expected file plus a nested/renamed extraction
    # instead of failing the bundle because of one extra path component.
    PLUGIN_SO="$(find "${ROOT}/artifacts-macos-tmp" -type f \
        \( -name 'kateai-macos.so' -o -name 'kateai.so' \) -print 2>/dev/null | head -1 || true)"
fi
if [[ -n "${PLUGIN_SO}" && ! -f "${PLUGIN_SO}" ]]; then
    echo "WARNING: prebuilt plugin was not found under ${ROOT}/artifacts-macos-tmp; rebuilding it." >&2
    find "${ROOT}/artifacts-macos-tmp" -maxdepth 4 -type f -print >&2 2>/dev/null || true
    PLUGIN_SO=""
fi
if [[ -n "${PLUGIN_SO}" ]]; then
    cp -f "${PLUGIN_SO}" "${ROOT}/artifacts-macos-tmp/kateai.so"
    echo "==> Reusing prebuilt plugin ${PLUGIN_SO}"
else
    /usr/bin/env bash "${ROOT}/packaging/ci/macos-build.sh" --build-type Release --artifacts-dir "${ROOT}/artifacts-macos-tmp"
fi

# Locate Craft's Kate.app. Craft installs macOS applications under
# CraftRoot/Applications (often Applications/KDE/kate.app), but the exact
# layout has changed between Craft releases, so check the usual locations and
# then fall back to a bounded search.
APP=""
for candidate in \
    "${CRAFT_ROOT}/Applications/KDE/kate.app" \
    "${CRAFT_ROOT}/Applications/Kate.app" \
    "${CRAFT_ROOT}/Applications/kate.app" \
    "${CRAFT_ROOT}/bin/kate.app"; do
    if [[ -d "${candidate}" ]]; then
        APP="${candidate}"
        break
    fi
done
if [[ -z "${APP}" && -d "${CRAFT_ROOT}/Applications" ]]; then
    APP="$(find "${CRAFT_ROOT}/Applications" -type d -iname 'kate.app' -print 2>/dev/null | head -1 || true)"
fi
if [[ -n "${APP}" ]]; then
    # Mirror the installer's discovery: use the ktexteditor directory that
    # actually exists in the bundle, and create the conventional location only
    # when the app has none yet.
    PLUGIN_DIRS=()
    while IFS= read -r dir; do
        [[ -n "${dir}" ]] && PLUGIN_DIRS+=("${dir}")
    done < <(find "${APP}/Contents" -type d -name ktexteditor -print 2>/dev/null || true)
    if [[ ${#PLUGIN_DIRS[@]} -eq 0 ]]; then
        PLUGIN_DIRS=("${APP}/Contents/PlugIns/kf6/ktexteditor")
    fi
    COPIED=0
    for plugin_dir in "${PLUGIN_DIRS[@]}"; do
        mkdir -p "${plugin_dir}"
        if install -m 755 "${ROOT}/artifacts-macos-tmp/kateai.so" "${plugin_dir}/kateai.so"; then
            COPIED=1
        fi
    done
    if [[ "${COPIED}" -eq 1 ]]; then
        echo "==> Installed plugin into ${APP}"
    else
        echo "WARNING: could not install the plugin into ${APP}; shipping the plugin zip only." >&2
    fi
else
    echo "WARNING: no Craft Kate.app found under ${CRAFT_ROOT}; cannot produce a preinstalled app bundle." >&2
fi

echo "==> Packaging Kate installer (.dmg) via Craft"
craft --package kate

# Craft's packaging step may rebuild/refresh the app bundle. Re-apply the
# plugin before zipping so the shipped Kate.app always contains it.
if [[ -n "${APP}" ]]; then
    for plugin_dir in "${PLUGIN_DIRS[@]}"; do
        mkdir -p "${plugin_dir}"
        install -m 755 "${ROOT}/artifacts-macos-tmp/kateai.so" "${plugin_dir}/kateai.so" 2>/dev/null || true
    done
fi

mkdir -p "${OUT_DIR}"
DMG="$(find "${CRAFT_ROOT}/tmp" -type f -iname 'kate-*.dmg' -print 2>/dev/null | sort | tail -1 || true)"
if [[ -n "${DMG}" && -f "${DMG}" ]]; then
    cp -f "${DMG}" "${OUT_DIR}/kate-${VERSION}-macos-${ARCH}.dmg"
else
    echo "WARNING: no kate-*.dmg found under ${CRAFT_ROOT}/tmp; shipping the app/plugin bundle only." >&2
fi
rm -f "${OUT_DIR}/kateai-${VERSION}-macos-${ARCH}.zip"
(cd "${ROOT}/artifacts-macos-tmp" && zip -j "${OUT_DIR}/kateai-${VERSION}-macos-${ARCH}.zip" kateai.so)

# A DMG generated by Craft does not know about this plugin. The app zip is the
# actual "Kate with the plugin already installed" artifact: keep every bundle
# symlink and resource fork intact with ditto, which is what a .app needs.
if [[ -n "${APP}" && -d "${APP}" ]]; then
    APP_ZIP="${OUT_DIR}/kate-bundle-${VERSION}-macos-${ARCH}.zip"
    rm -f "${APP_ZIP}"
    ditto -c -k --sequesterRsrc --keepParent "${APP}" "${APP_ZIP}"
    echo "Kate.app + plugin bundle: ${APP_ZIP}"
fi
rm -rf "${ROOT}/artifacts-macos-tmp"
ls -l "${OUT_DIR}"/kate-*-macos-*.dmg "${OUT_DIR}"/kate-*-macos-*.zip "${OUT_DIR}"/kateai-*-macos-*.zip 2>/dev/null || true
echo "Bundle ready in ${OUT_DIR}"
