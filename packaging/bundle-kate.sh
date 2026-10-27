#!/usr/bin/env bash
# Build the latest Kate editor from source and bundle it with this plugin.
#
# Result is a self-contained prefix:
#   <prefix>/bin/kate            # Kate built from KATE_REF (default: master)
#   <prefix>/lib/qt6/plugins/kf6/ktexteditor/kateai.so
#   <prefix>/bin/kate-ai         # wrapper that launches the bundled Kate
# plus a tarball in packaging/dist/ for CI releases.
#
# Usage:
#   packaging/bundle-kate.sh [--kate-ref master] [--prefix /opt/kate-ai-bundle]
#                            [--out packaging/dist/kate-bundle-<ver>.tar.gz]
#                            [--kate-dir <existing-checkout>] [--skip-kate]
#
# --skip-kate reuses an already-built prefix (only rebuilds the plugin).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
# shellcheck source=scripts/kateai-platform.sh
source "${ROOT}/scripts/kateai-platform.sh"

KATE_GIT_URL="${KATE_GIT_URL:-https://invent.kde.org/utilities/kate.git}"
KATE_REF="${KATE_REF:-master}"
PREFIX="/opt/kate-ai-bundle"
OUT=""
KATE_DIR=""
SKIP_KATE=0
BUILD_TYPE="RelWithDebInfo"
JOBS="$(kateai_jobs)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --kate-ref) KATE_REF="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --kate-dir) KATE_DIR="$2"; shift 2 ;;
        --skip-kate) SKIP_KATE=1; shift ;;
        --release) BUILD_TYPE="Release"; shift ;;
        --debug) BUILD_TYPE="Debug"; shift ;;
        -h|--help)
            sed -n '1,20p' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

VERSION="$(sed -n 's/^project(kateai VERSION \([^ ]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.1.0}"
KATE_SHORT="$(echo "${KATE_REF}" | tr '/ ' '__')"
if [[ -z "${OUT}" ]]; then
    OUT="${ROOT}/packaging/dist/kate-bundle-${VERSION}-kate-${KATE_SHORT}-$(uname -m).tar.gz"
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

if [[ "${SKIP_KATE}" -eq 0 ]]; then
    if [[ -z "${KATE_DIR}" ]]; then
        KATE_DIR="${WORKDIR}/kate"
        echo "==> Cloning Kate (${KATE_REF}) from ${KATE_GIT_URL}"
        git clone --depth 1 --branch "${KATE_REF}" "${KATE_GIT_URL}" "${KATE_DIR}" 2>/dev/null \
        || git clone --depth 1 "${KATE_GIT_URL}" "${KATE_DIR}"
        if [[ "${KATE_REF}" != "master" ]]; then
            (cd "${KATE_DIR}" && git fetch --depth 1 origin "${KATE_REF}" 2>/dev/null && git checkout FETCH_HEAD) || true
        fi
    fi
    echo "==> Building Kate (${KATE_REF}) into ${PREFIX}"
    cmake -S "${KATE_DIR}" -B "${WORKDIR}/build-kate" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DBUILD_TESTING=OFF \
        -G Ninja
    cmake --build "${WORKDIR}/build-kate" -j"${JOBS}"
    if [[ "$(id -u)" -eq 0 ]]; then
        cmake --install "${WORKDIR}/build-kate"
    else
        sudo cmake --install "${WORKDIR}/build-kate" 2>/dev/null \
        || cmake --install "${WORKDIR}/build-kate" --prefix "${PREFIX}"
    fi
else
    echo "==> Reusing existing Kate prefix at ${PREFIX}"
fi

echo "==> Building Kate AI plugin against ${PREFIX}"
# USE_QT_SYS_PATHS=OFF: the plugin must land under the bundle prefix.
# ON would resolve the *system* Qt plugin dir (/usr/lib64/...) and the
# bundle check below would fail on exactly that installed-to-system file.
cmake -S "${ROOT}" -B "${WORKDIR}/build-kateai" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DKATEAI_USER_INSTALL=OFF \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF \
    -DBUILD_TESTING=OFF \
    -G Ninja
cmake --build "${WORKDIR}/build-kateai" -j"${JOBS}"
if [[ "$(id -u)" -eq 0 ]]; then
    DESTDIR="" cmake --install "${WORKDIR}/build-kateai"
else
    sudo cmake --install "${WORKDIR}/build-kateai" 2>/dev/null \
    || (echo "sudo failed, installing with user permissions into ${PREFIX}" >&2; cmake --install "${WORKDIR}/build-kateai")
fi

PLUGIN_SO="$(find "${PREFIX}" -path '*kf6/ktexteditor/kateai.so' 2>/dev/null | head -1 || true)"
if [[ -z "${PLUGIN_SO}" ]]; then
    echo "Bundle failed: kateai.so not found under ${PREFIX}" >&2
    exit 1
fi
echo "Bundled plugin: ${PLUGIN_SO}"
ls -l "${PLUGIN_SO}"

# Wrapper so the bundle runs without env setup: the plugin already lives in
# the prefix's Qt plugin dir, so plain `kate` is enough. The plugin dir is
# derived from where the .so actually landed (lib vs lib64 varies per
# distro) and stored relative to the prefix, so the tarball stays
# relocatable instead of hard-coding /opt or lib/.
PLUGIN_BASE="$(dirname "$(dirname "$(dirname "${PLUGIN_SO}")")")"
PLUGIN_RELPATH="$(realpath --relative-to="${PREFIX}" "${PLUGIN_BASE}")"
cat > "${PREFIX}/bin/kate-ai" <<EOF
#!/usr/bin/env bash
set -euo pipefail
HERE="\$(cd "\$(dirname "\$0")" && pwd)"
PREFIX="\$(cd "\${HERE}/.." && pwd)"
export QT_PLUGIN_PATH="\${PREFIX}/${PLUGIN_RELPATH}\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
exec "\${PREFIX}/bin/kate" "\$@"
EOF
chmod +x "${PREFIX}/bin/kate-ai"

mkdir -p "$(dirname "${OUT}")"
echo "==> Packing ${PREFIX} -> ${OUT}"
tar -C "$(dirname "${PREFIX}")" -czf "${OUT}" "$(basename "${PREFIX}")"
ls -l "${OUT}"
echo "Bundle ready: ${OUT}"
echo "Run it: tar -xzf ${OUT} -C / && /opt/kate-ai-bundle/bin/kate-ai"
