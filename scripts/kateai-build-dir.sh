# Shared by install.sh and build.sh. Not meant to be run directly.
# Picks a build/ tree that is not a CMake cache from another machine or user.

kateai_resolve_build_dir() {
    local src cached def
    src="$(pwd -P)"
    def="${KATEAI_BUILD_DIR:-build}"

    if [[ -f "${def}/CMakeCache.txt" ]]; then
        cached="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${def}/CMakeCache.txt" | head -1)"
        if [[ -n "${cached}" && "${cached}" != "${src}" ]]; then
            if [[ -w "${def}" ]]; then
                echo "==> Removing stale ${def}/ (configured for ${cached})" >&2
                rm -rf "${def}"
            else
                def="build-$(id -un)"
                echo "==> build/ was configured for ${cached} and is not writable; using ${def}" >&2
            fi
        elif [[ ! -w "${def}" || ! -w "${def}/CMakeCache.txt" ]]; then
            def="build-$(id -un)"
            echo "==> build/ is not writable; using ${def}" >&2
        fi
    elif [[ -d "${def}" && ! -w "${def}" ]]; then
        def="build-$(id -un)"
        echo "==> build/ is not writable; using ${def}" >&2
    fi

    printf '%s\n' "${def}"
}
