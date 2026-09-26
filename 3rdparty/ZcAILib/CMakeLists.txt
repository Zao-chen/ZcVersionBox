cmake_minimum_required(VERSION 3.16)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum supported macOS")
endif()
project(ZcAiLib VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(zcailib_top_level OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(zcailib_top_level ON)
endif()
option(ZCAILIB_BUILD_EXAMPLES "Build the Widgets example" ${zcailib_top_level})
option(ZCAILIB_BUILD_STATIC "Build the static SDK as well" ${zcailib_top_level})
option(ZCAILIB_BUILD_TESTING "Build isolated SDK and package tests" ${zcailib_top_level})
option(ZCAILIB_INSTALL "Install the SDK and CMake package" ${zcailib_top_level})
find_package(Qt6 6.8.3 REQUIRED COMPONENTS Core Network)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Expose the same public spelling on case-sensitive build and install hosts.
configure_file(src/aiprovider.h include/AiProvider.h COPYONLY)
configure_file(src/ZcAiLib_global.h include/ZcAiLib_global.h COPYONLY)
set(ZCAILIB_SOURCES src/aiprovider.cpp src/aiprovider.h src/ZcAiLib_global.h)
function(zcailib_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_link_libraries(${target} PUBLIC Qt6::Core Qt6::Network)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON DEBUG_POSTFIX "")
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()
add_library(ZcAiLib SHARED ${ZCAILIB_SOURCES})
add_library(ZcAiLib::ZcAiLib ALIAS ZcAiLib)
zcailib_configure_target(ZcAiLib)
target_compile_definitions(ZcAiLib PRIVATE ZCAILIB_LIBRARY)
set_target_properties(ZcAiLib PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1
    MACOSX_RPATH ON INSTALL_NAME_DIR "@rpath")
if(UNIX AND NOT APPLE)
    set_target_properties(ZcAiLib PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
if(MINGW)
    set_target_properties(ZcAiLib PROPERTIES IMPORT_SUFFIX ".a")
endif()
set(zcailib_install_targets ZcAiLib)
if(ZCAILIB_BUILD_STATIC)
    add_library(ZcAiLibStatic STATIC ${ZCAILIB_SOURCES})
    add_library(ZcAiLib::ZcAiLibStatic ALIAS ZcAiLibStatic)
    zcailib_configure_target(ZcAiLibStatic)
    target_compile_definitions(ZcAiLibStatic PUBLIC ZCAILIB_STATIC)
    # Windows import and static libraries must not overwrite each other.
    if(NOT WIN32)
        set_target_properties(ZcAiLibStatic PROPERTIES OUTPUT_NAME ZcAiLib)
    endif()
    list(APPEND zcailib_install_targets ZcAiLibStatic)
endif()
if(ZCAILIB_INSTALL)
    install(TARGETS ${zcailib_install_targets} EXPORT ZcAiLibTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/AiProvider.h"
        src/ZcAiLib_global.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    set(zcailib_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/ZcAiLib")
    configure_package_config_file(cmake/ZcAiLibConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/ZcAiLibConfig.cmake"
        INSTALL_DESTINATION ${zcailib_package_dir})
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZcAiLibConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(EXPORT ZcAiLibTargets NAMESPACE ZcAiLib:: DESTINATION ${zcailib_package_dir})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZcAiLibConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ZcAiLibConfigVersion.cmake" DESTINATION ${zcailib_package_dir})
    install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/ZcAiLib)
endif()
if(ZCAILIB_BUILD_EXAMPLES)
    find_package(Qt6 6.8.3 REQUIRED COMPONENTS Widgets)
    # AGL is absent from macOS SDK 26+, while older Qt 6.8 OpenGL discovery can
    # still add it. Qt Gui uses OpenGL directly and has no AGL calls.
    if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
        get_property(zcailib_gl TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES)
        list(FILTER zcailib_gl EXCLUDE REGEX "AGL.framework$|^-framework AGL$")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${zcailib_gl}")
    endif()
    add_executable(ZcAiLib-Example example/main.cpp example/mainwindow.h
        example/mainwindow.cpp example/mainwindow.ui)
    target_link_libraries(ZcAiLib-Example PRIVATE Qt6::Widgets ZcAiLib::ZcAiLib)
    if(MSVC)
        target_compile_options(ZcAiLib-Example PRIVATE /utf-8)
    endif()
    if(WIN32)
        add_custom_command(TARGET ZcAiLib-Example POST_BUILD COMMAND ${CMAKE_COMMAND}
            -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:ZcAiLib-Example>")
    endif()
endif()
if(ZCAILIB_BUILD_TESTING)
    enable_testing()
    find_package(Qt6 6.8.3 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
