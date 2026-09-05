#!/usr/bin/env bash
set -euo pipefail

qtpaths_bin="$(command -v qtpaths6 || command -v qtpaths || true)"
if [[ -z "${qtpaths_bin}" && -x /usr/lib/qt6/bin/qtpaths ]]; then
    qtpaths_bin=/usr/lib/qt6/bin/qtpaths
fi

candidates=()
if [[ -n "${qtpaths_bin}" ]]; then
    candidates+=("$("${qtpaths_bin}" --plugin-dir)/kf6/ktexteditor/kateai.so")
fi
candidates+=(
    "${HOME}/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so"
    /usr/lib/qt6/plugins/kf6/ktexteditor/kateai.so
    /usr/lib64/qt6/plugins/kf6/ktexteditor/kateai.so
    /usr/lib/x86_64-linux-gnu/qt6/plugins/kf6/ktexteditor/kateai.so
)

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
    "${HOME}/.config/environment.d/50-kate-ai.conf"; do
    if [[ -e "${extra}" ]]; then
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
