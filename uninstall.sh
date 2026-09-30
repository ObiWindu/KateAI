#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=scripts/kateai-platform.sh
source "$(dirname "$0")/scripts/kateai-platform.sh"

PLUGIN_NAME="$(kateai_plugin_name)"
USER_PLUGIN_DIR="$(kateai_user_plugin_dir)"

qtpaths_bin="$(command -v qtpaths6 || command -v qtpaths || true)"
if [[ -z "${qtpaths_bin}" && -x /usr/lib/qt6/bin/qtpaths ]]; then
    qtpaths_bin=/usr/lib/qt6/bin/qtpaths
fi

candidates=()
if [[ -n "${qtpaths_bin}" ]]; then
    candidates+=("$("${qtpaths_bin}" --plugin-dir)/kf6/ktexteditor/${PLUGIN_NAME}")
fi
candidates+=(
    "${USER_PLUGIN_DIR}/kf6/ktexteditor/${PLUGIN_NAME}"
    "${HOME}/.local/lib/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}"
    /usr/lib/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}
    /usr/lib64/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}
    /usr/lib/x86_64-linux-gnu/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}
    /usr/lib/aarch64-linux-gnu/qt6/plugins/kf6/ktexteditor/${PLUGIN_NAME}
)

if [[ "$(kateai_family)" == "macos" ]]; then
    while IFS= read -r kdir; do
        [[ -n "${kdir}" ]] && candidates+=("${kdir}/${PLUGIN_NAME}")
    done < <(kateai_macos_kate_plugin_dirs)
fi

removed=0
for so in "${candidates[@]}"; do
    if [[ -e "${so}" ]]; then
        if [[ -w "${so}" || -w "$(dirname "${so}")" ]]; then
            rm -f "${so}"
        else
            sudo rm -f "${so}"
        fi
        echo "Removed ${so}"
        removed=1
    fi
done

for extra in \
    "${HOME}/.config/plasma-workspace/env/kate-ai.sh" \
    "${HOME}/.config/environment.d/50-kate-ai.conf" \
    "${HOME}/Library/LaunchAgents/org.kateai.qtpluginpath.plist"; do
    if [[ -e "${extra}" ]]; then
        if [[ "${extra}" == *LaunchAgents* ]]; then
            launchctl unload "${extra}" 2>/dev/null || true
        fi
        rm -f "${extra}"
        echo "Removed ${extra}"
        removed=1
    fi
done

if [[ "${removed}" -eq 0 ]]; then
    echo "Kate AI plugin not found in the usual plugin directories."
    exit 1
fi

echo "Fully quit Kate and reopen it so the plugin list refreshes."
