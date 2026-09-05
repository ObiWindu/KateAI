#!/usr/bin/env bash
# User-local copy. Prefer ../install.sh for a system-wide install from git.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
so="${root}/build/bin/kf6/ktexteditor/kateai.so"
if [[ ! -f "${so}" ]]; then
    echo "Build the plugin first: cmake --build build" >&2
    exit 1
fi

qtpaths_bin="$(command -v qtpaths6 || command -v qtpaths || true)"
if [[ -z "${qtpaths_bin}" && -x /usr/lib/qt6/bin/qtpaths ]]; then
    qtpaths_bin=/usr/lib/qt6/bin/qtpaths
fi
plugin_root="$("${qtpaths_bin}" --plugin-dir 2>/dev/null || echo /usr/lib/qt6/plugins)"
system_dir="${plugin_root}/kf6/ktexteditor"
user_dir="${HOME}/.local/lib/qt6/plugins/kf6/ktexteditor"

install_one() {
    local dest_dir="$1"
    mkdir -p "${dest_dir}"
    install -m 755 "${so}" "${dest_dir}/kateai.so"
    echo "Installed ${dest_dir}/kateai.so"
}

if [[ -d "${system_dir}" ]]; then
    if [[ -w "${system_dir}" ]]; then
        install_one "${system_dir}"
    else
        echo "Installing to ${system_dir} (needs sudo so Kate can read the plugin)..."
        sudo install -m 755 "${so}" "${system_dir}/kateai.so"
        echo "Installed ${system_dir}/kateai.so"
    fi
else
    install_one "${user_dir}"
    echo "Start Kate with: QT_PLUGIN_PATH=\"${HOME}/.local/lib/qt6/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}\" kate"
fi

echo "Fully quit Kate, reopen it, then enable Settings → Configure Kate → Plugins → Kate AI"
