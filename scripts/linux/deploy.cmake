find_package(Qt6 6.8.3 REQUIRED COMPONENTS WaylandClient)
target_link_libraries(ZcVersionBox PRIVATE Qt6::WaylandClient)
# Qt's default Linux deployment selects XCB only. Include both Wayland QPA
# backends explicitly, together with their dependency and shell plugins.
qt_import_plugins(ZcVersionBox
    INCLUDE_BY_TYPE platforms Qt6::QXcbIntegrationPlugin
        Qt6::QWaylandIntegrationPlugin Qt6::QWaylandEglPlatformIntegrationPlugin
    EXCLUDE_BY_TYPE egldeviceintegrations)
if(NOT CMAKE_INSTALL_BINDIR STREQUAL "bin" OR NOT CMAKE_INSTALL_LIBDIR STREQUAL "lib")
    message(FATAL_ERROR "Linux packaging requires CMAKE_INSTALL_BINDIR=bin and CMAKE_INSTALL_LIBDIR=lib")
endif()
set_target_properties(ZcVersionBox PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "$ORIGIN/../lib")
set_target_properties(ZcAiLib PROPERTIES INSTALL_RPATH "$ORIGIN")
install(TARGETS ZcAiLib LIBRARY DESTINATION lib NAMELINK_SKIP)
qt_generate_deploy_app_script(TARGET ZcVersionBox OUTPUT_SCRIPT zc_linux_deploy_script NO_TRANSLATIONS)
install(SCRIPT "${zc_linux_deploy_script}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/cpack.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LinuxCPackConfig.cmake" @ONLY)
