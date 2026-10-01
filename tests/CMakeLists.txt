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

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE kateai_core Qt6::Test)
add_test(NAME test_tools COMMAND test_tools -platform offscreen)

add_executable(test_promptedit test_promptedit.cpp ../src/promptedit.cpp)
target_link_libraries(test_promptedit PRIVATE kateai_core Qt6::Test)
add_test(NAME test_promptedit COMMAND test_promptedit -platform offscreen)

# Conversation history regressions: covers the "New Thread loses the previous
# conversation" flow against a real ChatWidget.
add_executable(test_conversations
    test_conversations.cpp
    ../src/chatwidget.cpp
    ../src/toolcallwidget.cpp
    ../src/promptedit.cpp
    ../src/permissionbar.cpp
    ../src/edittracker.cpp
)
target_link_libraries(test_conversations PRIVATE
    kateai_core
    KF6::ConfigCore
    KF6::I18n
    KF6::TextEditor
    KF6::WidgetsAddons
    Qt6::Test
)
set_target_properties(test_conversations PROPERTIES AUTOMOC ON)
add_test(NAME test_conversations COMMAND test_conversations -platform offscreen)
