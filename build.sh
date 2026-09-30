#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=scripts/kateai-build-dir.sh
source "$(dirname "$0")/scripts/kateai-build-dir.sh"
# shellcheck source=scripts/kateai-platform.sh
source "$(dirname "$0")/scripts/kateai-platform.sh"
BUILD_DIR="$(kateai_resolve_build_dir)"

rm -f "${BUILD_DIR}/prefix.sh" "${BUILD_DIR}/prefix.sh.fish" \
      "${BUILD_DIR}/ecm_uninstall.cmake" "${BUILD_DIR}/DartConfiguration.tcl" \
      "${BUILD_DIR}/CTestConfiguration.ini" "${BUILD_DIR}/CTestCustom.cmake" \
      "${BUILD_DIR}/CMakeFiles/CTestScript.cmake"

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${1:-RelWithDebInfo}"
cmake --build "${BUILD_DIR}" -j"$(kateai_jobs)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
