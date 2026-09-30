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
    macos|windows)
        echo "Use ./install.sh (macOS) or .\\install.ps1 (Windows). Distro packages are Linux-only." >&2
        exit 1
        ;;
    *)
        echo "No package recipe for family '${FAMILY}'." >&2
        echo "Supported: arch (CachyOS, Manjaro), fedora (Asahi), debian (Ubuntu 25.04+)." >&2
        exit 1
        ;;
esac
