#!/usr/bin/env bash
# Default: install for this user only (no sudo).
# Use --system for a machine-wide copy (needs sudo, mode 755).
# Use --deps to install distro build/runtime packages first.
# Use --package to build an Arch / Fedora / Debian package.
set -euo pipefail
cd "$(dirname "$0")"

# shellcheck source=scripts/kateai-build-dir.sh
source "$(dirname "$0")/scripts/kateai-build-dir.sh"
# shellcheck source=scripts/kateai-platform.sh
source "$(dirname "$0")/scripts/kateai-platform.sh"

MODE="user"
BUILD_TYPE="Release"
DO_DEPS=0
DO_PACKAGE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user) MODE="user"; shift ;;
        --system) MODE="system"; shift ;;
        --debug) BUILD_TYPE="Debug"; shift ;;
        --release) BUILD_TYPE="Release"; shift ;;
        --deps) DO_DEPS=1; shift ;;
        --package) DO_PACKAGE=1; shift ;;
        --skip-tests) SKIP_TESTS=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--user|--system] [--debug|--release] [--deps] [--package] [--skip-tests]"
            echo "  --user      install for this account (default)"
            echo "  --system    install into Kate's Qt plugin dir (sudo on Unix)"
            echo "  --deps      install distro/Homebrew packages first"
            echo "  --package   build an Arch, Fedora/Asahi, or Debian/Ubuntu package"
            echo "  --skip-tests  skip ctest after the build"
            echo
            echo "Windows: powershell -ExecutionPolicy Bypass -File install.ps1"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

FAMILY="$(kateai_family)"
if [[ "${FAMILY}" == "windows" ]]; then
    echo "Use install.ps1 on Windows." >&2
    exit 1
fi

if [[ "${DO_DEPS}" -eq 1 ]]; then
    echo "==> Installing build and runtime packages (${FAMILY})"
    kateai_install_deps
fi

if [[ "${DO_PACKAGE}" -eq 1 ]]; then
    exec "$(pwd)/packaging/build-package.sh" "${FAMILY}"
fi

JOBS="$(kateai_jobs)"
PLUGIN_NAME="$(kateai_plugin_name)"
USER_PLUGIN_DIR="$(kateai_user_plugin_dir)"
USER_PLUGIN_SO="${USER_PLUGIN_DIR}/kf6/ktexteditor/${PLUGIN_NAME}"
BUILD_DIR="$(kateai_resolve_build_dir)"

if [[ "${MODE}" == "user" ]]; then
    echo "==> Configuring user install (${USER_PLUGIN_DIR}, ${BUILD_TYPE}, ${BUILD_DIR})"
    cmake -S . -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DKATEAI_USER_INSTALL=ON \
        -DCMAKE_INSTALL_PREFIX="${HOME}/.local" \
        -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF \
        -DKDE_INSTALL_QTPLUGINDIR=lib/qt6/plugins
else
    echo "==> Configuring system install (${BUILD_TYPE}, ${BUILD_DIR})"
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

install_user_plugin() {
    local built dest_dir dest
    built="$(kateai_find_built_plugin "${BUILD_DIR}")" || {
        echo "Build did not produce ${PLUGIN_NAME} under ${BUILD_DIR}" >&2
        exit 1
    }
    dest_dir="${USER_PLUGIN_DIR}/kf6/ktexteditor"
    dest="${dest_dir}/${PLUGIN_NAME}"
    mkdir -p "${dest_dir}"
    install -m 700 "${built}" "${dest}"
    chmod 700 "${dest_dir}" "${dest}" 2>/dev/null || true
    echo "${dest}"
}

if [[ "${MODE}" == "user" ]]; then
    echo "==> Installing for ${USER} only"
    INSTALLED="$(install_user_plugin)"
    echo "Installed: ${INSTALLED}"
    ls -l "${INSTALLED}"

    if [[ "${FAMILY}" == "macos" ]]; then
        copied=0
        while IFS= read -r kdir; do
            [[ -z "${kdir}" ]] && continue
            mkdir -p "${kdir}" 2>/dev/null || true
            if [[ -w "${kdir}" ]] || mkdir -p "${kdir}" 2>/dev/null; then
                if cp -f "${INSTALLED}" "${kdir}/${PLUGIN_NAME}" 2>/dev/null; then
                    echo "Also copied into ${kdir}/${PLUGIN_NAME}"
                    copied=1
                fi
            fi
        done < <(kateai_macos_kate_plugin_dirs | awk 'NF && !seen[$0]++')

        AGENT="$(kateai_write_macos_launch_agent "${USER_PLUGIN_DIR}")"
        echo "Wrote ${AGENT} so GUI Kate sees QT_PLUGIN_PATH."
        if [[ "${copied}" -eq 0 ]]; then
            echo "If the plugin is missing in Kate.app, copy it next to the other ktexteditor plugins inside the app bundle."
        fi
    else
        mkdir -p "${HOME}/.config/plasma-workspace/env" "${HOME}/.config/environment.d"
        cp -f scripts/kate-ai-user-env.sh "${HOME}/.config/plasma-workspace/env/kate-ai.sh"
        chmod 600 "${HOME}/.config/plasma-workspace/env/kate-ai.sh"
        cp -f scripts/50-kate-ai.conf "${HOME}/.config/environment.d/50-kate-ai.conf"
        chmod 600 "${HOME}/.config/environment.d/50-kate-ai.conf"
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
        echo "session picks up QT_PLUGIN_PATH."
    fi

    echo
    echo "Then:"
    echo "  1. Settings → Configure Kate → Plugins → enable Kate AI"
    echo "  2. Settings → Configure Kate → Kate AI  (paste API keys)"
    echo "  3. Ctrl+Alt+A opens the panel"
    exit 0
fi

echo "==> Installing system-wide"
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
    PLUGIN_SO="$("${qtpaths_bin}" --plugin-dir)/kf6/ktexteditor/${PLUGIN_NAME}"
else
    PLUGIN_SO="/usr/lib/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}"
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
