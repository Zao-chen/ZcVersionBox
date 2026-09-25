cmake_minimum_required(VERSION 3.21)
# Set this before project() initializes the Apple toolchain.
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum supported macOS")
endif()
project(ZcVersionBox VERSION 0.1.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 12.0)
        message(FATAL_ERROR "Qt 6.8 requires macOS 12.0 or newer")
    endif()
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.8.3 REQUIRED COMPONENTS Widgets Network Svg)
qt_standard_project_setup()
set(QLEMENTINE_SANDBOX OFF CACHE BOOL "" FORCE)
set(QLEMENTINE_SHOWCASE OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/qlementine EXCLUDE_FROM_ALL)

add_library(ZcAiLib SHARED IMPORTED GLOBAL)
set_target_properties(ZcAiLib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcAILib/include"
    INTERFACE_LINK_LIBRARIES "Qt6::Core;Qt6::Network")
if(WIN32)
    set_target_properties(ZcAiLib PROPERTIES
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcAILib/lib/ZcAiLib.lib"
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcAILib/bin/ZcAiLib.dll")
elseif(APPLE)
    set(ZCAI_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcAILib/lib/libZcAiLib.dylib")
    set_target_properties(ZcAiLib PROPERTIES IMPORTED_LOCATION "${ZCAI_RUNTIME}")
    execute_process(COMMAND lipo -archs "${ZCAI_RUNTIME}" OUTPUT_VARIABLE ZCAI_ARCHS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(CMAKE_OSX_ARCHITECTURES)
        set(REQUIRED_ARCHS ${CMAKE_OSX_ARCHITECTURES})
    else()
        set(REQUIRED_ARCHS ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    foreach(arch IN LISTS REQUIRED_ARCHS)
        if(NOT " ${ZCAI_ARCHS} " MATCHES " ${arch} ")
            message(FATAL_ERROR "ZcAiLib lacks ${arch}; available: ${ZCAI_ARCHS}. Supply a matching SDK before packaging.")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "This repository ships the AI SDK for Windows and macOS only")
endif()

add_library(zc_services STATIC
    GlobalConstants.h
    utils/apppaths.h utils/operationresult.h
    utils/gitcommand.h utils/gitcommand.cpp
    utils/backupservice.h utils/backupservice.cpp
    utils/backupmonitor.h utils/backupmonitor.cpp
    utils/backup_types.h utils/backup_dependencies.h
    utils/backup_catalog.h utils/backup_catalog.cpp
    utils/backup_files.h utils/backup_files.cpp
    utils/backup_git.h utils/backup_git.cpp
    utils/backup_engine.h utils/backup_engine.cpp
    utils/aigateway.h utils/aigateway.cpp
    utils/settingsservice.h utils/settingsservice.cpp
    utils/aiconfighelper.h utils/aiconfighelper.cpp
    utils/aicommitmessagehelper.h utils/aicommitmessagehelper.cpp)
target_include_directories(zc_services PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(zc_services PUBLIC Qt6::Core Qt6::Network ZcAiLib)
if(APPLE)
    target_sources(zc_services PRIVATE macos/macos_services.h macos/macos_services.mm)
    target_link_libraries(zc_services PUBLIC "-framework AppKit")
endif()
add_library(zc_ui STATIC
    windows/mainwindow.h windows/mainwindow.cpp windows/mainwindow.ui
    windows/mainwindow_navigation.h windows/mainwindow_navigation.cpp
    windows/mainwindow_presentation.h windows/mainwindow_presentation.cpp
    windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.h windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.cpp
    windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.h windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.cpp
    windows/mainwindow_child/homepage/homepage.h windows/mainwindow_child/homepage/homepage.cpp windows/mainwindow_child/homepage/homepage.ui
    windows/mainwindow_child/homepage/pages/homepage_page_dashboard.h windows/mainwindow_child/homepage/pages/homepage_page_dashboard.cpp windows/mainwindow_child/homepage/pages/homepage_page_dashboard.ui
    windows/mainwindow_child/homepage/pages/homepage_page_backup.h windows/mainwindow_child/homepage/pages/homepage_page_backup.cpp windows/mainwindow_child/homepage/pages/homepage_page_backup.ui
    windows/mainwindow_child/homepage/pages/homepage_page_diff.h windows/mainwindow_child/homepage/pages/homepage_page_diff.cpp windows/mainwindow_child/homepage/pages/homepage_page_diff.ui
    windows/mainwindow_child/settingpage/settingpage.h windows/mainwindow_child/settingpage/settingpage.cpp windows/mainwindow_child/settingpage/settingpage.ui windows/mainwindow_child/settingpage/settingpage_page_ai.ui
    windows/mainwindow_child/aboutpage/aboutpage.h windows/mainwindow_child/aboutpage/aboutpage.cpp windows/mainwindow_child/aboutpage/aboutpage.ui)
target_link_libraries(zc_ui PUBLIC zc_services qlementine Qt6::Widgets)
if(MSVC)
    target_compile_options(zc_services PRIVATE /utf-8)
    target_compile_options(zc_ui PRIVATE /utf-8)
endif()
qt_add_executable(ZcVersionBox WIN32 MACOSX_BUNDLE main.cpp res.qrc)
target_link_libraries(ZcVersionBox PRIVATE zc_ui)
target_compile_definitions(ZcVersionBox PRIVATE ZCVERSIONBOX_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(ZcVersionBox PRIVATE /utf-8)
endif()
if(WIN32)
    add_custom_command(TARGET ZcVersionBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZcAiLib>" "$<TARGET_FILE_DIR:ZcVersionBox>")
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}" REQUIRED)
    add_custom_command(TARGET ZcVersionBox POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:ZcVersionBox>")
endif()
if(APPLE)
    set(ZCVERSIONBOX_MACOS_BUNDLE_NAME ZcVersionBox)
    set(ZCVERSIONBOX_MACOS_BUNDLE_IDENTIFIER com.zc.versionbox)
    set(ZCVERSIONBOX_MACOS_ICON_FILE ZcVersionBox.icns)
    set(ZCVERSIONBOX_MACOS_BUNDLE_VERSION ${PROJECT_VERSION})
    set(ZCVERSIONBOX_MACOS_SHORT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
    set(ZCVERSIONBOX_MACOS_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET})
    configure_file(macos/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/ZcVersionBoxInfo.plist" @ONLY)
    target_sources(ZcVersionBox PRIVATE res/img/ZcVersionBox.icns)
    set_source_files_properties(res/img/ZcVersionBox.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(ZcVersionBox PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/ZcVersionBoxInfo.plist"
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks")
    add_custom_command(TARGET ZcVersionBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ZcVersionBox>/../Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ZCAI_RUNTIME}" "$<TARGET_FILE_DIR:ZcVersionBox>/../Frameworks/libZcAiLib.1.dylib")
endif()
include(GNUInstallDirs)
install(TARGETS ZcVersionBox BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8.3 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
