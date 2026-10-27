#!/usr/bin/env bash
# Build a distro package from this tree (Arch pkg.tar.zst, Fedora rpm, Debian deb).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"

FAMILY="${1:-$(kateai_family)}"
TARBALL="$("${ROOT}/packaging/mk-source-tarball.sh")"
VERSION="$(basename "${TARBALL}" .tar.gz)"
VERSION="${VERSION#kateai-}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

case "${FAMILY}" in
    arch)
        cp -a "${ROOT}/packaging/arch/." "${WORKDIR}/"
        cp -f "${TARBALL}" "${WORKDIR}/kateai-${VERSION}.tar.gz"
        (cd "${WORKDIR}" && makepkg -f --noconfirm)
        mkdir -p "${ROOT}/packaging/dist"
        cp -f "${WORKDIR}"/*.pkg.tar.* "${ROOT}/packaging/dist/"
        echo "Arch package(s) in ${ROOT}/packaging/dist/"
        ls -l "${ROOT}/packaging/dist"/*.pkg.tar.*
        ;;
    fedora)
        mkdir -p "${WORKDIR}/SOURCES" "${WORKDIR}/SPECS" "${WORKDIR}/BUILD" "${WORKDIR}/RPMS" "${WORKDIR}/SRPMS"
        cp -f "${TARBALL}" "${WORKDIR}/SOURCES/"
        cp -f "${ROOT}/packaging/fedora/kateai.spec" "${WORKDIR}/SPECS/"
        rpmbuild \
            --define "_topdir ${WORKDIR}" \
            -ba "${WORKDIR}/SPECS/kateai.spec"
        mkdir -p "${ROOT}/packaging/dist"
        find "${WORKDIR}/RPMS" "${WORKDIR}/SRPMS" -type f -name '*.rpm' -exec cp -f {} "${ROOT}/packaging/dist/" \;
        echo "RPM package(s) in ${ROOT}/packaging/dist/"
        ls -l "${ROOT}/packaging/dist"/*.rpm
        ;;
    debian)
        tar -C "${WORKDIR}" -xzf "${TARBALL}"
        SRC="${WORKDIR}/kateai-${VERSION}"
        (cd "${SRC}" && dpkg-buildpackage -us -uc -b)
        mkdir -p "${ROOT}/packaging/dist"
        cp -f "${WORKDIR}"/*.deb "${ROOT}/packaging/dist/" 2>/dev/null || true
        echo "Debian package(s) in ${ROOT}/packaging/dist/"
        ls -l "${ROOT}/packaging/dist"/*.deb
        ;;
    macos)
        # Plugin zip for macOS: Homebrew Kate + KF6 build (see packaging/ci/macos-build.sh).
        VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
        VERSION="${VERSION:-0.1.0}"
        "${ROOT}/packaging/ci/macos-build.sh" --build-type Release --artifacts-dir "${ROOT}/artifacts-macos-tmp"
        mkdir -p "${ROOT}/packaging/dist"
        OUT="${ROOT}/packaging/dist/kateai-${VERSION}-macos-$(uname -m).zip"
        rm -f "${OUT}"
        (cd "${ROOT}/artifacts-macos-tmp" && zip -j "${OUT}" kateai.so)
        rm -rf "${ROOT}/artifacts-macos-tmp"
        echo "macOS package: ${OUT}"
        ls -l "${OUT}"
        ;;
    windows)
        echo "On Windows, build with KDE Craft (matching MSVC ABI):" >&2
        echo "  powershell -ExecutionPolicy Bypass -File packaging\\ci\\windows-build.ps1" >&2
        echo "Bundle latest Kate + plugin with:" >&2
        echo "  powershell -ExecutionPolicy Bypass -File packaging\\bundle-kate-windows.ps1" >&2
        exit 1
        ;;
    *)
        echo "No package recipe for family '${FAMILY}'." >&2
        echo "Supported: arch (CachyOS, Manjaro), fedora (Asahi), debian (Ubuntu 25.04+)." >&2
        exit 1
        ;;
esac
