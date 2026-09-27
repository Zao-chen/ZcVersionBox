if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    # Qt's offscreen Cocoa integration can crash while tearing down a GUI test
    # process on hosted macOS runners. The minimal plugin still exercises the
    # widget hierarchy without requiring a display server.
    set(ENV{QT_QPA_PLATFORM} minimal)
else()
    set(ENV{QT_QPA_PLATFORM} offscreen)
endif()
if(WIN32)
    set(ENV{PATH} "${QT_BIN_DIR};$ENV{PATH}")
    set(ENV{QT_QPA_PLATFORM_PLUGIN_PATH} "${QT_BIN_DIR}/../plugins/platforms")
endif()
# Qt Test may write to the debugger rather than inherited stdout on Windows.
# Always keep a text result and expose it to CTest, including after a failure.
file(WRITE "${TEST_LOG}" "")
execute_process(COMMAND "${TEST_EXECUTABLE}" -o "${TEST_LOG},txt"
    RESULT_VARIABLE result)
file(READ "${TEST_LOG}" output)
message("${output}")
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "Regression process failed: ${result}")
endif()
