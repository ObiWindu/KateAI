#!/usr/bin/env bash
# Create kateai-<version>.tar.gz from the current tree for distro packagers.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.1.0}"
DIST="${ROOT}/packaging/dist"
NAME="kateai-${VERSION}"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

mkdir -p "${STAGE}/${NAME}" "${DIST}"
# Copy the sources packagers need; skip build trees and local caches.
tar -C "${ROOT}" \
    --exclude='./build' --exclude='./build-*' --exclude='./packaging/dist' \
    --exclude='./.git' --exclude='./lib' \
    --exclude='./build.log' --exclude='./*.o' --exclude='./src/*.o' \
    --exclude='./src/_probe.txt' \
    -cf - . | tar -C "${STAGE}/${NAME}" -xf -

# Debian helpers live under packaging/debian; native packages expect debian/.
rm -rf "${STAGE}/${NAME}/debian"
cp -a "${ROOT}/packaging/debian" "${STAGE}/${NAME}/debian"
chmod +x "${STAGE}/${NAME}/debian/rules"

OUT="${DIST}/${NAME}.tar.gz"
tar -C "${STAGE}" -czf "${OUT}" "${NAME}"
printf '%s\n' "${OUT}"
