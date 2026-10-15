function(kateai_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE kateai_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kateai_test(test_sandbox)
kateai_test(test_permissions)
kateai_test(test_settings)
kateai_test(test_llmparse)
kateai_test(test_session)
kateai_test(test_retry)
kateai_test(test_agentparity)
kateai_test(test_checkpoints)
kateai_test(test_multiagent)
kateai_test(test_websearch)
kateai_test(test_contextmanager)
kateai_test(test_providers)
kateai_test(test_transcriptlayout)

# Renders through QTextDocument, which needs a QGuiApplication.
add_executable(test_codehighlight test_codehighlight.cpp)
target_link_libraries(test_codehighlight PRIVATE kateai_core Qt6::Test)
add_test(NAME test_codehighlight COMMAND test_codehighlight -platform offscreen)

# Drives a real AgentLoop against a mock LLM, so it needs an event loop.
add_executable(test_subtaskparallel test_subtaskparallel.cpp)
target_link_libraries(test_subtaskparallel PRIVATE kateai_core Qt6::Test)
add_test(NAME test_subtaskparallel
         COMMAND test_subtaskparallel -platform offscreen
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Drives a real stdio MCP server, so it needs a QCoreApplication event loop.
add_executable(test_mcpintegration test_mcpintegration.cpp)
target_link_libraries(test_mcpintegration PRIVATE kateai_core Qt6::Test)
add_test(NAME test_mcpintegration
         COMMAND test_mcpintegration -platform offscreen
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Drives a mock ACP agent over stdio JSON-RPC.
add_executable(test_acpclient test_acpclient.cpp)
target_link_libraries(test_acpclient PRIVATE kateai_core Qt6::Test)
add_test(NAME test_acpclient
         COMMAND test_acpclient -platform offscreen
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The question tool: the loop must pause on a question and resume from the
# answer. Needs the same mock-endpoint harness as the sub-agent test.
add_executable(test_questionflow test_questionflow.cpp)
target_link_libraries(test_questionflow PRIVATE kateai_core Qt6::Test)
add_test(NAME test_questionflow
         COMMAND test_questionflow -platform offscreen
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE kateai_core Qt6::Test)
add_test(NAME test_tools COMMAND test_tools -platform offscreen)

# Drives a QWidget queue, so it needs the Qt Widgets library and a QApplication.
# chattheme.cpp is included because the tracker paints through it, and the real
# build gets it from the kateai plugin target rather than kateai_core.
add_executable(test_edittracker test_edittracker.cpp ../src/edittracker.cpp ../src/chattheme.cpp)
target_link_libraries(test_edittracker PRIVATE kateai_core Qt6::Test Qt6::Widgets)
add_test(NAME test_edittracker COMMAND test_edittracker -platform offscreen)

# Sub-agent cards and the intent-dock chrome they live in. subtaskwidget.cpp is
# compiled in for the same reason as edittracker.cpp above: it belongs to the
# plugin target, not to kateai_core.
add_executable(test_intentdock test_intentdock.cpp ../src/subtaskwidget.cpp ../src/chattheme.cpp)
target_link_libraries(test_intentdock PRIVATE kateai_core Qt6::Test Qt6::Widgets)
add_test(NAME test_intentdock COMMAND test_intentdock -platform offscreen)

# The suggestion cards the suggest tool and the ACP client method post.
# suggestionwidget.cpp is compiled in for the same reason as subtaskwidget.cpp:
# it belongs to the plugin target rather than to kateai_core.
add_executable(test_suggestionwidget test_suggestionwidget.cpp ../src/suggestionwidget.cpp ../src/chattheme.cpp)
target_link_libraries(test_suggestionwidget PRIVATE kateai_core Qt6::Test Qt6::Widgets)
target_compile_definitions(test_suggestionwidget PRIVATE TRANSLATION_DOMAIN="kateai")
add_test(NAME test_suggestionwidget COMMAND test_suggestionwidget -platform offscreen)

# The question cards the question tool puts up until the user answers.
add_executable(test_questionwidget test_questionwidget.cpp ../src/questionwidget.cpp ../src/chattheme.cpp)
target_link_libraries(test_questionwidget PRIVATE kateai_core Qt6::Test Qt6::Widgets)
target_compile_definitions(test_questionwidget PRIVATE TRANSLATION_DOMAIN="kateai")
add_test(NAME test_questionwidget COMMAND test_questionwidget -platform offscreen)

# Tool-card headers and command previews, which are capped at three lines.
add_executable(test_toolcard test_toolcard.cpp ../src/toolcallwidget.cpp ../src/chattheme.cpp)
target_link_libraries(test_toolcard PRIVATE kateai_core Qt6::Test Qt6::Widgets)
add_test(NAME test_toolcard COMMAND test_toolcard -platform offscreen)

add_executable(test_promptedit test_promptedit.cpp ../src/promptedit.cpp)
target_link_libraries(test_promptedit PRIVATE kateai_core Qt6::Test)
add_test(NAME test_promptedit COMMAND test_promptedit -platform offscreen)

add_executable(test_overflow test_overflow.cpp ../src/chatwidget.cpp ../src/toolcallwidget.cpp
    ../src/promptedit.cpp ../src/permissionbar.cpp ../src/subtaskwidget.cpp
    ../src/suggestionwidget.cpp ../src/questionwidget.cpp ../src/edittracker.cpp
    ../src/turnstatus.cpp ../src/chattheme.cpp)
target_link_libraries(test_overflow PRIVATE kateai_core Qt6::Test Qt6::Widgets)
target_compile_definitions(test_overflow PRIVATE TRANSLATION_DOMAIN="kateai")
add_test(NAME test_overflow COMMAND test_overflow -platform offscreen)

add_executable(test_livecards test_livecards.cpp ../src/chatwidget.cpp ../src/toolcallwidget.cpp
    ../src/promptedit.cpp ../src/permissionbar.cpp ../src/subtaskwidget.cpp
    ../src/suggestionwidget.cpp ../src/questionwidget.cpp ../src/edittracker.cpp
    ../src/turnstatus.cpp ../src/chattheme.cpp)
target_link_libraries(test_livecards PRIVATE kateai_core Qt6::Test Qt6::Widgets)
target_compile_definitions(test_livecards PRIVATE TRANSLATION_DOMAIN="kateai")
add_test(NAME test_livecards COMMAND test_livecards -platform offscreen)
