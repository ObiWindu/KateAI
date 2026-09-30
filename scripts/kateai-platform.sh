# Shared platform helpers for install.sh / uninstall.sh / build.sh.
# Not meant to be run directly.

kateai_uname() {
    uname -s 2>/dev/null || echo unknown
}

kateai_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

kateai_plugin_name() {
    case "$(kateai_uname)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT) echo kateai.dll ;;
        *) echo kateai.so ;;
    esac
}

kateai_user_plugin_dir() {
    case "$(kateai_uname)" in
        Darwin)
            echo "${HOME}/Library/Application Support/kate/lib/qt6/plugins"
            ;;
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            echo "${LOCALAPPDATA:-${HOME}/AppData/Local}/KateAI/plugins"
            ;;
        *)
            echo "${HOME}/.local/lib/qt6/plugins"
            ;;
    esac
}

kateai_os_release_id() {
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        echo "${ID:-}"
    fi
}

kateai_os_release_like() {
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        echo "${ID_LIKE:-} ${ID:-} ${VARIANT_ID:-} ${VARIANT:-}"
    fi
}

kateai_family() {
    case "$(kateai_uname)" in
        Darwin) echo macos; return ;;
        MINGW*|MSYS*|CYGWIN*|Windows_NT) echo windows; return ;;
    esac
    local like
    like="$(kateai_os_release_like | tr '[:upper:]' '[:lower:]')"
    case " ${like} " in
        *" arch "*|*" cachyos "*|*" manjaro "*|*" endeavouros "*) echo arch ;;
        *" fedora "*|*" asahi "*|*" rhel "*|*" centos "*) echo fedora ;;
        *" debian "*|*" ubuntu "*) echo debian ;;
        *) echo linux ;;
    esac
}

kateai_ubuntu_ok() {
    if [[ ! -r /etc/os-release ]]; then
        return 1
    fi
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" == "debian" ]]; then
        return 0
    fi
    if [[ "${ID:-}" != "ubuntu" && "${ID_LIKE:-}" != *ubuntu* ]]; then
        return 1
    fi
    local ver="${VERSION_ID:-0}"
    local major minor
    major="${ver%%.*}"
    minor="${ver#*.}"
    minor="${minor%%.*}"
    if [[ "${major}" -gt 25 ]]; then
        return 0
    fi
    if [[ "${major}" -eq 25 && "${minor:-0}" -ge 4 ]]; then
        return 0
    fi
    return 1
}

# Arch / CachyOS / Manjaro package names.
kateai_arch_packages() {
    printf '%s\n' \
        kate extra-cmake-modules cmake ninja gcc \
        qt6-base ktexteditor kcoreaddons ki18n \
        kxmlgui kconfigwidgets kconfig kwidgetsaddons \
        bubblewrap
}

kateai_fedora_packages() {
    printf '%s\n' \
        kate extra-cmake-modules cmake gcc-c++ ninja-build \
        qt6-qtbase-devel kf6-ktexteditor-devel kf6-kcoreaddons-devel \
        kf6-ki18n-devel kf6-kxmlgui-devel kf6-kconfigwidgets-devel \
        kf6-kconfig-devel kf6-kwidgetsaddons-devel \
        bubblewrap
}

kateai_debian_packages() {
    printf '%s\n' \
        kate cmake extra-cmake-modules ninja-build g++ pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-tools-dev \
        libkf6texteditor-dev libkf6coreaddons-dev libkf6i18n-dev \
        libkf6xmlgui-dev libkf6configwidgets-dev libkf6config-dev \
        libkf6widgetaddons-dev \
        bubblewrap
}

kateai_macos_brew_packages() {
    printf '%s\n' \
        cmake extra-cmake-modules ninja qtbase \
        kcoreaddons ki18n kconfig kconfigwidgets kxmlgui kwidgetsaddons
}

kateai_priv() {
    if [[ "$(id -u)" -eq 0 ]]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        echo "Need root to run: $*" >&2
        return 1
    fi
}

kateai_install_deps() {
    local family
    family="$(kateai_family)"
    case "${family}" in
        arch)
            kateai_priv pacman -S --needed --noconfirm $(kateai_arch_packages)
            ;;
        fedora)
            if command -v dnf5 >/dev/null 2>&1; then
                kateai_priv dnf5 install -y $(kateai_fedora_packages)
            elif command -v dnf >/dev/null 2>&1; then
                kateai_priv dnf install -y $(kateai_fedora_packages)
            else
                echo "dnf is required on Fedora / Asahi." >&2
                return 1
            fi
            ;;
        debian)
            if ! kateai_ubuntu_ok; then
                echo "Debian/Ubuntu packages target Ubuntu 25.04+ (KF6). This release may be too old." >&2
            fi
            kateai_priv apt-get update
            kateai_priv apt-get install -y $(kateai_debian_packages)
            ;;
        macos)
            if ! command -v brew >/dev/null 2>&1; then
                echo "Homebrew is required on macOS. Install it from https://brew.sh" >&2
                return 1
            fi
            brew install $(kateai_macos_brew_packages)
            if ! brew list --cask kate >/dev/null 2>&1; then
                brew install --cask kate
            fi
            echo
            echo "ktexteditor headers are not in Homebrew. Building against Homebrew KF6"
            echo "may not load in the Kate.app cask. Prefer KDE Craft for a matching ABI:"
            echo "  https://develop.kde.org/docs/getting-started/building/craft/"
            echo "The installer will still copy a built plugin into Kate.app when possible."
            ;;
        windows)
            echo "On Windows, build with KDE Craft so the plugin matches Kate's ABI." >&2
            echo "See packaging/windows/install.ps1" >&2
            return 1
            ;;
        *)
            echo "No packaged dependencies for this OS. Install Kate, Qt 6.5+, KF6, CMake 3.25+, and bubblewrap (Linux)." >&2
            return 1
            ;;
    esac
}

kateai_find_built_plugin() {
    local build_dir="$1"
    local name
    name="$(kateai_plugin_name)"
    if [[ -f "${build_dir}/bin/kf6/ktexteditor/${name}" ]]; then
        printf '%s\n' "${build_dir}/bin/kf6/ktexteditor/${name}"
        return 0
    fi
    local found
    found="$(find "${build_dir}" \( -name kateai.so -o -name kateai.dll -o -name kateai.dylib \) -print -quit 2>/dev/null || true)"
    if [[ -n "${found}" && -f "${found}" ]]; then
        printf '%s\n' "${found}"
        return 0
    fi
    return 1
}

kateai_macos_kate_plugin_dirs() {
    local app dir
    for app in \
        "/Applications/Kate.app" \
        "/Applications/kate.app" \
        "${HOME}/Applications/Kate.app" \
        "${HOME}/Applications/kate.app"; do
        if [[ -d "${app}" ]]; then
            while IFS= read -r dir; do
                printf '%s\n' "${dir}"
            done < <(find "${app}/Contents" -type d -name ktexteditor 2>/dev/null || true)
            printf '%s\n' "${app}/Contents/PlugIns/kf6/ktexteditor"
            printf '%s\n' "${app}/Contents/lib/plugins/kf6/ktexteditor"
        fi
    done
}

kateai_write_macos_launch_agent() {
    local plugin_dir="$1"
    local dest="${HOME}/Library/LaunchAgents/org.kateai.qtpluginpath.plist"
    mkdir -p "${HOME}/Library/LaunchAgents"
    cat > "${dest}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>org.kateai.qtpluginpath</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/launchctl</string>
        <string>setenv</string>
        <string>QT_PLUGIN_PATH</string>
        <string>${plugin_dir}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
EOF
    chmod 600 "${dest}"
    launchctl unload "${dest}" 2>/dev/null || true
    launchctl load "${dest}" 2>/dev/null || true
    launchctl setenv QT_PLUGIN_PATH "${plugin_dir}" 2>/dev/null || true
    echo "${dest}"
}
