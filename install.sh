#!/usr/bin/env bash
# Default: install for this user only (~/.local, mode 700, no sudo).
# Use --system for a machine-wide copy (needs sudo, mode 755).
set -euo pipefail
cd "$(dirname "$0")"

MODE="user"
BUILD_TYPE="Release"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user) MODE="user"; shift ;;
        --system) MODE="system"; shift ;;
        --debug) BUILD_TYPE="Debug"; shift ;;
        --release) BUILD_TYPE="Release"; shift ;;
        -h|--help)
            echo "Usage: $0 [--user|--system] [--debug|--release]"
            echo "  --user     install to ~/.local (default, mode 700, no sudo)"
            echo "  --system   install to /usr/lib/qt6/plugins (mode 755, sudo)"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

JOBS="$(command -v nproc >/dev/null && nproc || echo 4)"
USER_PLUGIN_DIR="${HOME}/.local/lib/qt6/plugins"
USER_PLUGIN_SO="${USER_PLUGIN_DIR}/kf6/ktexteditor/kateai.so"

# shellcheck source=scripts/kateai-build-dir.sh
source "$(dirname "$0")/scripts/kateai-build-dir.sh"
BUILD_DIR="$(kateai_resolve_build_dir)"

if [[ "${MODE}" == "user" ]]; then
    echo "==> Configuring user install (${HOME}/.local, ${BUILD_TYPE}, ${BUILD_DIR})"
    cmake -S . -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DKATEAI_USER_INSTALL=ON \
        -DCMAKE_INSTALL_PREFIX="${HOME}/.local" \
        -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF \
        -DKDE_INSTALL_QTPLUGINDIR=lib/qt6/plugins
else
    echo "==> Configuring system install (/usr, ${BUILD_TYPE}, ${BUILD_DIR})"
    cmake -S . -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DKATEAI_USER_INSTALL=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DKDE_INSTALL_USE_QT_SYS_PATHS=ON
fi

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

if [[ "${SKIP_TESTS:-0}" != "1" ]]; then
    echo "==> Tests"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if [[ "${MODE}" == "user" ]]; then
    echo "==> Installing for ${USER} only (${HOME}/.local)"
    BUILT_SO="${BUILD_DIR}/bin/kf6/ktexteditor/kateai.so"
    if [[ ! -f "${BUILT_SO}" ]]; then
        BUILT_SO="$(find "${BUILD_DIR}" -name kateai.so -print -quit)"
    fi
    if [[ -z "${BUILT_SO}" || ! -f "${BUILT_SO}" ]]; then
        echo "Build did not produce kateai.so under ${BUILD_DIR}" >&2
        exit 1
    fi
    mkdir -p "${USER_PLUGIN_DIR}/kf6/ktexteditor"
    install -m 700 "${BUILT_SO}" "${USER_PLUGIN_SO}"
    chmod 700 "${USER_PLUGIN_DIR}/kf6/ktexteditor" "${USER_PLUGIN_SO}"

    mkdir -p "${HOME}/.config/plasma-workspace/env" "${HOME}/.config/environment.d"
    cp -f scripts/kate-ai-user-env.sh "${HOME}/.config/plasma-workspace/env/kate-ai.sh"
    chmod 600 "${HOME}/.config/plasma-workspace/env/kate-ai.sh"
    cp -f scripts/50-kate-ai.conf "${HOME}/.config/environment.d/50-kate-ai.conf"
    chmod 600 "${HOME}/.config/environment.d/50-kate-ai.conf"

    echo
    echo "Installed: ${USER_PLUGIN_SO}"
    ls -l "${USER_PLUGIN_SO}"
    echo
    echo "Kate does not search ~/.local by itself. This install added:"
    echo "  ~/.config/plasma-workspace/env/kate-ai.sh"
    echo "  ~/.config/environment.d/50-kate-ai.conf"
    echo
    echo "To use it in this session (no logout):"
    echo "  export QT_PLUGIN_PATH=\"${USER_PLUGIN_DIR}\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}\""
    echo "  kate"
    echo
    echo "From the application menu, log out and back in once (or reboot) so the"
    echo "session picks up QT_PLUGIN_PATH. Then:"
    echo "  1. Settings → Configure Kate → Plugins → enable Kate AI"
    echo "  2. Settings → Configure Kate → Kate AI  (paste API keys)"
    echo "  3. Ctrl+Alt+A opens the panel"
    exit 0
fi

echo "==> Installing system-wide (sudo)"
if [[ "$(id -u)" -eq 0 ]]; then
    cmake --install "${BUILD_DIR}"
else
    sudo cmake --install "${BUILD_DIR}"
fi

qtpaths_bin="$(command -v qtpaths6 || command -v qtpaths || true)"
if [[ -z "${qtpaths_bin}" && -x /usr/lib/qt6/bin/qtpaths ]]; then
    qtpaths_bin=/usr/lib/qt6/bin/qtpaths
fi
if [[ -n "${qtpaths_bin}" ]]; then
    PLUGIN_SO="$("${qtpaths_bin}" --plugin-dir)/kf6/ktexteditor/kateai.so"
else
    PLUGIN_SO="/usr/lib/qt6/plugins/kf6/ktexteditor/kateai.so"
fi

if [[ -f "${PLUGIN_SO}" ]]; then
    if [[ -w "${PLUGIN_SO}" ]]; then
        chmod 755 "${PLUGIN_SO}"
    else
        sudo chmod 755 "${PLUGIN_SO}"
    fi
    echo
    echo "Installed: ${PLUGIN_SO}"
    ls -l "${PLUGIN_SO}"
fi

echo
echo "Next: fully quit Kate, enable Kate AI in Settings → Plugins, add API keys."
