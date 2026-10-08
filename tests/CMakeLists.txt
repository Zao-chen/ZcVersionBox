set(ZCVERSIONBOX_TEST_PROFILE "full" CACHE STRING "Test scope: smoke, standard or full")
set_property(CACHE ZCVERSIONBOX_TEST_PROFILE PROPERTY STRINGS smoke standard full)
if(NOT ZCVERSIONBOX_TEST_PROFILE MATCHES "^(smoke|standard|full)$")
    message(FATAL_ERROR "ZCVERSIONBOX_TEST_PROFILE must be smoke, standard or full")
endif()

function(zc_add_regression target source dependency)
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
endfunction()

function(zc_register_regression target name timeout)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
        "-DTEST_LOG=${CMAKE_CURRENT_BINARY_DIR}/${name}.txt"
        "-DQT_BIN_DIR=${QT_BIN_DIR}"
        "-DTEST_TIMEOUT=${timeout}"
        "-DTEST_FUNCTIONS=${ARGN}"
        "-DTEST_PROFILE=${ZCVERSIONBOX_TEST_PROFILE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")
    # Let the runner write a diagnostic before CTest terminates its process.
    math(EXPR ctest_timeout "${timeout} + 30")
    set_tests_properties(${name} PROPERTIES TIMEOUT ${ctest_timeout}
        LABELS "${ZCVERSIONBOX_TEST_PROFILE}")
endfunction()

zc_add_regression(zc_tests regression.cpp zc_ui ../res.qrc)
zc_add_regression(zc_backup_tests backup_core.cpp zc_services)
zc_add_regression(zc_update_tests update_service.cpp zc_services)
if(ZCVERSIONBOX_TEST_PROFILE STREQUAL "smoke")
    zc_register_regression(zc_tests regression 180
        platformRuntimeSmoke closingWithoutTrayExitsWindow localBackupRestoreAndDiff
        historyMenuCreatesBranchAfterMouseDispatch:mouse-create-and-switch-detail
        historyMenuCreatesBranchAfterMouseDispatch:direct-create-and-switch-detail
        directHistoryEditAndRestore)
    zc_register_regression(zc_backup_tests backup_core 180
        fingerprintReaderAllowsAtomicSourceReplacement
        sourceEventsHandleAtomicSavesNewDirectoriesAndRecreation
        branchSwitchSavesWorkAndKeepsIgnoredFiles
        branchSwitchRejectsStaleConfirmationAndIgnoredCollision
        restorePreservesGitOnlyDirectoriesAndRollsBackPartialMoves
        recursiveStorageAndLinksAreRejected concurrentProcessCannotWriteLockedStore)
else()
    if(WIN32)
        set(ui_timeout 600)
        set(core_timeout 1200)
    else()
        set(ui_timeout 300)
        set(core_timeout 600)
    endif()
    zc_register_regression(zc_tests regression ${ui_timeout})
    zc_register_regression(zc_backup_tests backup_core ${core_timeout})
endif()
zc_register_regression(zc_update_tests update_core 60)
