add_library(kateai_core STATIC
    types.cpp
    settings.cpp
    sandbox.cpp
    permissions.cpp
    tools.cpp
    llmclient.cpp
    agentloop.cpp
    workspace.cpp
    documentbridge.cpp
)

set_target_properties(kateai_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kateai_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kateai_core PRIVATE TRANSLATION_DOMAIN="kateai")
target_link_libraries(kateai_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::I18n
        KF6::TextEditor
)

kcoreaddons_add_plugin(kateai INSTALL_NAMESPACE "kf6/ktexteditor")
target_compile_definitions(kateai PRIVATE TRANSLATION_DOMAIN="kateai")
target_sources(kateai PRIVATE
    plugin.cpp
    pluginview.cpp
    configpage.cpp
    chatwidget.cpp
    promptedit.cpp
    permissionbar.cpp
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

# Workspace ACLs default other::--- ; Kate must be able to read the .so to list it.
add_custom_command(TARGET kateai POST_BUILD
    COMMAND chmod a+rX "$<TARGET_FILE:kateai>"
    VERBATIM
)
