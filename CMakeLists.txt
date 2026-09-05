cmake_minimum_required(VERSION 3.25)
project(kateai VERSION 0.1.0 LANGUAGES CXX)

set(QT_MAJOR_VERSION "6")
set(QT_MIN_VERSION "6.5.0")
set(KF6_DEP_VERSION "6.0.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KATEAI_USER_INSTALL "Install into ~/.local for the current user only (mode 700, no sudo)" OFF)

if(KATEAI_USER_INSTALL)
    if(NOT DEFINED ENV{HOME} OR "$ENV{HOME}" STREQUAL "")
        message(FATAL_ERROR "KATEAI_USER_INSTALL requires HOME")
    endif()
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "User-local prefix" FORCE)
    set(KDE_INSTALL_USE_QT_SYS_PATHS OFF CACHE BOOL "Do not write into the system Qt plugin dir" FORCE)
    # CACHE PATH would turn a relative value into ${CMAKE_SOURCE_DIR}/lib/qt6/plugins.
    set(KDE_INSTALL_QTPLUGINDIR "${CMAKE_INSTALL_PREFIX}/lib/qt6/plugins" CACHE STRING "Qt plugins under the install prefix" FORCE)
else()
    # System Kate only scans Qt's plugin dir, not /usr/local.
    set(KDE_INSTALL_USE_QT_SYS_PATHS ON CACHE BOOL "Install into Qt plugin dir (required for Kate)" FORCE)
endif()

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# A previous cmake run as another user can leave ACL-locked files in the
# build dir. ECM/CTest then fail on configure_file() with EPERM.
foreach(_kateai_stale
        "${CMAKE_BINARY_DIR}/prefix.sh"
        "${CMAKE_BINARY_DIR}/prefix.sh.fish"
        "${CMAKE_BINARY_DIR}/ecm_uninstall.cmake"
        "${CMAKE_BINARY_DIR}/DartConfiguration.tcl"
        "${CMAKE_BINARY_DIR}/CTestConfiguration.ini"
        "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
        "${CMAKE_BINARY_DIR}/CMakeFiles/CTestScript.cmake")
    if(EXISTS "${_kateai_stale}")
        file(REMOVE "${_kateai_stale}")
    endif()
endforeach()

set(KDE_SKIP_UNINSTALL_TARGET ON)
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)
enable_testing()

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(KF6 ${KF6_DEP_VERSION} REQUIRED COMPONENTS
    CoreAddons
    I18n
    Config
    ConfigWidgets
    TextEditor
    XmlGui
    WidgetsAddons
)

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

if(NOT KDE_INSTALL_FULL_QTPLUGINDIR)
    set(KDE_INSTALL_FULL_QTPLUGINDIR "${CMAKE_INSTALL_PREFIX}/lib/qt6/plugins")
endif()
set(KATEAI_PLUGIN_DEST "${KDE_INSTALL_FULL_QTPLUGINDIR}/kf6/ktexteditor/kateai.so")
if(KATEAI_USER_INSTALL)
    set(KATEAI_PLUGIN_MODE "OWNER_READ OWNER_WRITE OWNER_EXECUTE")
else()
    set(KATEAI_PLUGIN_MODE "OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE")
endif()
install(CODE "
    set(_plugin \"\$ENV{DESTDIR}${KATEAI_PLUGIN_DEST}\")
    if(EXISTS \"\${_plugin}\")
        file(CHMOD \"\${_plugin}\" FILE_PERMISSIONS ${KATEAI_PLUGIN_MODE})
        message(STATUS \"Installed Kate AI plugin: \${_plugin}\")
    endif()
")

message(STATUS "Kate AI plugin install path: ${KATEAI_PLUGIN_DEST}")
if(KATEAI_USER_INSTALL)
    message(STATUS "User install (mode 700): cmake --build build && cmake --install build")
else()
    message(STATUS "System install (mode 755): cmake --build build && sudo cmake --install build")
endif()
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
