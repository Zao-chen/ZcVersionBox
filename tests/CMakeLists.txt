qt_add_executable(zc_tests regression.cpp ../res.qrc)
set_target_properties(zc_tests PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
target_link_libraries(zc_tests PRIVATE zc_ui Qt6::Test)
if(MSVC)
    target_compile_options(zc_tests PRIVATE /utf-8)
endif()
if(WIN32)
    add_custom_command(TARGET zc_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:zc_tests>")
else()
    set_target_properties(zc_tests PROPERTIES BUILD_RPATH "@loader_path")
    add_custom_command(TARGET zc_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:zc_tests>/libZcAiLib.1.dylib")
endif()
add_test(NAME regression COMMAND ${CMAKE_COMMAND}
    "-DTEST_EXECUTABLE=$<TARGET_FILE:zc_tests>"
    "-DTEST_LOG=${CMAKE_CURRENT_BINARY_DIR}/regression.txt"
    "-DQT_BIN_DIR=${QT_BIN_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")
set_tests_properties(regression PROPERTIES TIMEOUT 120)
