#!/usr/bin/env bash
# Bundle the latest Kate.app (Homebrew cask) with this plugin on macOS.
#
# Result: packaging/dist/kate-bundle-<ver>-macos-<arch>.tar.gz containing:
#   Kate.app/                      # latest Kate from `brew install --cask kate`
#     Contents/PlugIns/kf6/ktexteditor/kateai.so
#   install.sh                     # installs the plugin into an existing Kate.app
#
# Usage: packaging/bundle-kate-macos.sh [--out <file>] [--skip-kate-app]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"

export HOMEBREW_NO_AUTO_UPDATE=1
OUT=""
SKIP_KATE_APP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --skip-kate-app) SKIP_KATE_APP=1; shift ;;
        -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.1.0}"
ARCH="$(uname -m)"
if [[ -z "${OUT}" ]]; then
    OUT="${ROOT}/packaging/dist/kate-bundle-${VERSION}-macos-${ARCH}.tar.gz"
fi

if [[ "${SKIP_KATE_APP}" -eq 0 ]]; then
    echo "==> Installing latest Kate (Homebrew cask)"
    brew install --cask kate 2>/dev/null || brew upgrade --cask kate 2>/dev/null || true
fi

APP=""
for candidate in "/Applications/Kate.app" "${HOME}/Applications/Kate.app"; do
    if [[ -d "${candidate}" ]]; then APP="${candidate}"; break; fi
done
if [[ -z "${APP}" ]]; then
    echo "Kate.app not found; install it with: brew install --cask kate" >&2
    exit 1
fi
echo "Using Kate.app at ${APP}"

/usr/bin/env bash "${ROOT}/packaging/ci/macos-build.sh" --build-type Release --artifacts-dir "${ROOT}/artifacts-macos-tmp"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT
STAGE="${WORKDIR}/kate-macos-bundle"
mkdir -p "${STAGE}"
echo "==> Copying Kate.app into bundle staging"
cp -R "${APP}" "${STAGE}/Kate.app"
echo "==> Injecting plugin into bundled Kate.app"
PLUGIN_DEST="${STAGE}/Kate.app/Contents/PlugIns/kf6/ktexteditor"
mkdir -p "${PLUGIN_DEST}"
cp -f "${ROOT}/artifacts-macos-tmp/kateai.so" "${PLUGIN_DEST}/kateai.so"
cp -f "${ROOT}/scripts/install-user.sh" "${STAGE}/install.sh" 2>/dev/null || cp -f "${ROOT}/install.sh" "${STAGE}/install.sh"
ls -l "${PLUGIN_DEST}/kateai.so"

echo "==> Packing ${OUT}"
COPYFILE_DISABLE=1 tar -C "${WORKDIR}" -czf "${OUT}" kate-macos-bundle
ls -l "${OUT}"
rm -rf "${ROOT}/artifacts-macos-tmp"
echo "Bundle ready: ${OUT}"
