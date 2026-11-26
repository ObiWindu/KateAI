add_library(kateai_core STATIC
    types.cpp
    credentialstore.cpp
    processenv.cpp
    acpinstall.cpp
    settings.cpp
    sandbox.cpp
    permissions.cpp
    tools.cpp
    llmclient.cpp
    acpclient.cpp
    agentloop.cpp
    orchestration.cpp
    workspace.cpp
    documentbridge.cpp
    katelspbridge.cpp
    lsp.cpp
    debug.cpp
    katedebugbridge.cpp
    ecosystem.cpp
    kateecosystembridge.cpp
    sessionstore.cpp
    graph/projectgraph.cpp
    modes.cpp
    rules.cpp
    mcp.cpp
    checkpoint.cpp
    agentteam.cpp
    agentlocks.cpp
    agenttext.cpp
    chattheme.cpp
    codehighlight.cpp
    contextmanager.cpp
    transcriptlayout.cpp
    transcriptvirtualizer.cpp
    workspaceservices.cpp
    websearch.cpp
    voiceinput.cpp
    completionengine.cpp
    editranges.cpp
    harness/harnessflags.cpp
    harness/toolresults.cpp
    harness/models.cpp
    harness/tokens.cpp
    harness/prompt.cpp
    harness/memory.cpp
    harness/contextengine.cpp
    harness/registry.cpp
    harness/skills.cpp
    harness/worktree.cpp
    harness/events.cpp
    harness/eventstore.cpp
    harness/turn.cpp
    harness/middleware.cpp
    harness/checks.cpp
    harness/policy.cpp
    harness/audit.cpp
    harness/hooks.cpp
    harness/replay.cpp
    harness/cost.cpp
    harness/telemetry.cpp
    harness/otlp.cpp
    harness/acpserver.cpp
)

set_target_properties(kateai_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kateai_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kateai_core PRIVATE TRANSLATION_DOMAIN="kateai" KATEAI_VERSION="${PROJECT_VERSION}")
if(KATEAI_HAVE_KWALLET)
    target_compile_definitions(kateai_core PRIVATE KATEAI_HAVE_KWALLET=1)
    target_link_libraries(kateai_core PUBLIC KF6::Wallet)
else()
    # Always define the feature macro: `#if KATEAI_HAVE_KWALLET` in source must
    # not evaluate an undefined identifier under -Werror=undef.
    target_compile_definitions(kateai_core PRIVATE KATEAI_HAVE_KWALLET=0)
endif()
target_link_libraries(kateai_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Sql
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::I18n
        KF6::TextEditor
        KF6::SyntaxHighlighting
        KF6::XmlGui
        KF6::WidgetsAddons
)

kcoreaddons_add_plugin(kateai INSTALL_NAMESPACE "kf6/ktexteditor")
target_compile_definitions(kateai PRIVATE TRANSLATION_DOMAIN="kateai")
target_sources(kateai PRIVATE
    plugin.cpp
    pluginview.cpp
    editorassist.cpp
    kateaicommand.cpp
    configpage.cpp
    turnstatus.cpp
    chatwidget.cpp
    inspectorpanel.cpp
    toolcallwidget.cpp
    promptedit.cpp
    permissionbar.cpp
    edittracker.cpp
    subtaskwidget.cpp
    subtaskdock.cpp
    suggestionwidget.cpp
    questionwidget.cpp
    todowidget.cpp
    kateai.qrc
)
target_link_libraries(kateai PRIVATE
    kateai_core
    KF6::I18n
    KF6::TextEditor
    KF6::XmlGui
    KF6::ConfigWidgets
    KF6::WidgetsAddons
)

# Workspace ACLs default other::--- ; Kate must be able to read the plugin to list it.
if(UNIX)
    add_custom_command(TARGET kateai POST_BUILD
        COMMAND chmod a+rX "$<TARGET_FILE:kateai>"
        VERBATIM
    )
endif()

# One core, many clients: the headless CLI and ACP server drive the same
# AgentLoop as the editor plugin. They link the core library and install next
# to the plugin.
add_executable(kateai-headless
    headless/main.cpp
    headless/args.cpp
    headless/session.cpp
    headless/acpsession.cpp
)
target_compile_definitions(kateai-headless PRIVATE
    TRANSLATION_DOMAIN="kateai"
    KATEAI_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(kateai-headless PRIVATE kateai_core)
# The CLI is not a bundle: KDECMakeSettings defaults MACOSX_BUNDLE on for
# Apple executables, which would both wrap a command-line tool in a .app and
# fail configure because the install rule below has no BUNDLE DESTINATION.
if(APPLE)
    set_target_properties(kateai-headless PROPERTIES MACOSX_BUNDLE FALSE)
endif()
# BUNDLE DESTINATION keeps install() valid if MACOSX_BUNDLE is ever true
# again; CMake ignores it on platforms without bundle support.
install(TARGETS kateai-headless
    RUNTIME DESTINATION "${KDE_INSTALL_BINDIR}"
    BUNDLE DESTINATION "${KDE_INSTALL_BINDIR}"
)

# The 4 KiB page shim is a Linux-only interposer: it relies on <link.h>,
# dladdr1(), ElfW and the Linux __NR_* syscall numbers, and it spoofs glibc
# sysconf()/mmap() for the legacy Antigravity binary. macOS has none of those
# headers, and acpNeedsPageShim() is compile-time false off Linux, so building
# the shim there only breaks the build.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(kateai_page4k_shim SHARED
        page4k_shim.c
    )
    set_target_properties(kateai_page4k_shim PROPERTIES
        OUTPUT_NAME "libpage4k"
        PREFIX ""
        AUTOMOC OFF
        LINKER_LANGUAGE C
        C_VISIBILITY_PRESET default
    )
    target_compile_options(kateai_page4k_shim PRIVATE
        -fno-omit-frame-pointer
    )
    target_link_libraries(kateai_page4k_shim PRIVATE
        ${CMAKE_DL_LIBS}
        pthread
    )
    add_custom_command(TARGET kateai_page4k_shim POST_BUILD
        COMMAND chmod 755 "$<TARGET_FILE:kateai_page4k_shim>"
        VERBATIM
    )
    install(TARGETS kateai_page4k_shim
        LIBRARY DESTINATION "${KDE_INSTALL_LIBDIR}/kateai"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
endif()
