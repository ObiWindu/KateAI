function(kateai_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE kateai_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kateai_test(test_sandbox)
kateai_test(test_permissions)
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

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE kateai_core Qt6::Test)
add_test(NAME test_tools COMMAND test_tools -platform offscreen)

# Drives a QWidget queue, so it needs the Qt Widgets library and a QApplication.
# chattheme.cpp is included because the tracker paints through it, and the real
# build gets it from the kateai plugin target rather than kateai_core.
add_executable(test_edittracker test_edittracker.cpp ../src/edittracker.cpp ../src/chattheme.cpp)
target_link_libraries(test_edittracker PRIVATE kateai_core Qt6::Test Qt6::Widgets)
add_test(NAME test_edittracker COMMAND test_edittracker -platform offscreen)

add_executable(test_promptedit test_promptedit.cpp ../src/promptedit.cpp)
target_link_libraries(test_promptedit PRIVATE kateai_core Qt6::Test)
add_test(NAME test_promptedit COMMAND test_promptedit -platform offscreen)
