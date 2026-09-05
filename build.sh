#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Drop files ECM/CTest rewrite on every configure; mixed-user ACLs make that EPERM.
rm -f build/prefix.sh build/prefix.sh.fish build/ecm_uninstall.cmake \
      build/DartConfiguration.tcl build/CTestConfiguration.ini build/CTestCustom.cmake \
      build/CMakeFiles/CTestScript.cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE="${1:-RelWithDebInfo}"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
