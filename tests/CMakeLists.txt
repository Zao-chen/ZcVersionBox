function(zc_add_regression target source name dependency)
    qt_add_executable(${target} ${source} backup_test_support.h ${ARGN})
    set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
    target_link_libraries(${target} PRIVATE ${dependency} Qt6::Test)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:${target}>")
    elseif(APPLE)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:${target}>/libZcAiLib.1.dylib")
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
        "-DTEST_LOG=${CMAKE_CURRENT_BINARY_DIR}/${name}.txt"
        "-DQT_BIN_DIR=${QT_BIN_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

zc_add_regression(zc_tests regression.cpp regression zc_ui ../res.qrc)
zc_add_regression(zc_backup_tests backup_core.cpp backup_core zc_services)
# Includes the isolated 10,000-file source and complete backup safety checks.
set_tests_properties(backup_core PROPERTIES TIMEOUT 600)
