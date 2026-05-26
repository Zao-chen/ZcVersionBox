cmake_minimum_required(VERSION 3.16)

project(ZcVersionBox VERSION 0.1 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

if(APPLE)
    foreach(qt_target Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Core)
        if(TARGET ${qt_target})
            foreach(qt_prop INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
                get_target_property(qt_values ${qt_target} ${qt_prop})
                if(qt_values)
                    list(FILTER qt_values EXCLUDE REGEX "AGL")
                    set_property(TARGET ${qt_target} PROPERTY ${qt_prop} "${qt_values}")
                endif()
            endforeach()
        endif()
    endforeach()

    foreach(gl_target OpenGL::OpenGL OpenGL::GL OpenGL::GLU)
        if(TARGET ${gl_target})
            foreach(gl_prop INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
                get_target_property(gl_values ${gl_target} ${gl_prop})
                if(gl_values)
                    list(FILTER gl_values EXCLUDE REGEX "AGL")
                    set_property(TARGET ${gl_target} PROPERTY ${gl_prop} "${gl_values}")
                endif()
            endforeach()
        endif()
    endforeach()

    if(TARGET WrapOpenGL::WrapOpenGL)
        foreach(wrap_prop INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
            get_target_property(wrap_values WrapOpenGL::WrapOpenGL ${wrap_prop})
            if(wrap_values)
                list(FILTER wrap_values EXCLUDE REGEX "AGL")
                set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY ${wrap_prop} "${wrap_values}")
            endif()
        endforeach()
    endif()
endif()

set(PROJECT_SOURCES
        main.cpp
        windows/mainwindow.cpp
        windows/mainwindow.h
        windows/mainwindow.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(ZcVersionBox
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        windows/mainwindow_child/homepage/homepage.h windows/mainwindow_child/homepage/homepage.cpp windows/mainwindow_child/homepage/homepage.ui
        windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.cpp
        windows/mainwindow_child/homepage/pages/homepage_page_backup.cpp
        windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.h windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.cpp windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.ui
        windows/mainwindow_child/aboutpage/aboutpage.h windows/mainwindow_child/aboutpage/aboutpage.cpp windows/mainwindow_child/aboutpage/aboutpage.ui
        windows/mainwindow_child/settingpage/settingpage.h windows/mainwindow_child/settingpage/settingpage.cpp windows/mainwindow_child/settingpage/settingpage.ui
        GlobalConstants.h
        res.qrc

        utils/fileutils.h utils/fileutils.cpp
        utils/aicommitmessagehelper.h utils/aicommitmessagehelper.cpp
    )
    set_source_files_properties(
        windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.cpp
        windows/mainwindow_child/homepage/pages/homepage_page_backup.cpp
        PROPERTIES SKIP_AUTOUIC ON
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET ZcVersionBox APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(ZcVersionBox SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(ZcVersionBox
            ${PROJECT_SOURCES}
        )
    endif()
endif()

include_directories(
    include
)

set(LOCAL_THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")
set(PARENT_THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_include_directories(ZcVersionBox PRIVATE
    ${LOCAL_THIRDPARTY_DIR}/ElaWidgetTools/include
    ${LOCAL_THIRDPARTY_DIR}/ZcWidgetTools/include
    ${LOCAL_THIRDPARTY_DIR}/ZcAILib/include
    ${PARENT_THIRDPARTY_DIR}/ElaWidgetTools/ElaWidgetTools
    ${PARENT_THIRDPARTY_DIR}/ZcWidgetTools/src
    ${PARENT_THIRDPARTY_DIR}/ZcAILib/src
)

if(APPLE)
    set(ELA_WIDGETTOOLS_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ElaWidgetTools/lib"
        "${PARENT_THIRDPARTY_DIR}/ElaWidgetTools/build-mac-universal/ElaWidgetTools"
    )
    set(ZC_WIDGETTOOLS_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ZcWidgetTools/lib"
        "${PARENT_THIRDPARTY_DIR}/ZcWidgetTools/build-mac-universal"
    )
    set(ZCAI_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ZcAILib/lib"
        "${PARENT_THIRDPARTY_DIR}/ZcAILib/build-mac-universal"
        "${PARENT_THIRDPARTY_DIR}/ZcAILib/build"
    )
else()
    set(ELA_WIDGETTOOLS_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ElaWidgetTools/lib"
        "${PARENT_THIRDPARTY_DIR}/ElaWidgetTools/build-mac-universal/ElaWidgetTools"
    )
    set(ZC_WIDGETTOOLS_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ZcWidgetTools/lib"
        "${PARENT_THIRDPARTY_DIR}/ZcWidgetTools/build-mac-universal"
    )
    set(ZCAI_LIB_DIRS
        "${LOCAL_THIRDPARTY_DIR}/ZcAILib/lib"
        "${PARENT_THIRDPARTY_DIR}/ZcAILib/build-mac-universal"
    )
endif()

find_library(ELA_WIDGETTOOLS_LIB
    NAMES ElaWidgetTools elawidgettools
    PATHS ${ELA_WIDGETTOOLS_LIB_DIRS}
    NO_DEFAULT_PATH
)
find_library(ZC_WIDGETTOOLS_LIB
    NAMES ZcWidgetTools zcwidgettools
    PATHS ${ZC_WIDGETTOOLS_LIB_DIRS}
    NO_DEFAULT_PATH
)
find_library(ZCAI_LIB
    NAMES ZcAiLib zcailib
    PATHS ${ZCAI_LIB_DIRS}
    NO_DEFAULT_PATH
)

if(NOT ELA_WIDGETTOOLS_LIB)
    message(FATAL_ERROR "Missing ElaWidgetTools library. Put libelawidgettools.a/.dylib or elawidgettools.lib in 3rdparty/ElaWidgetTools/lib")
endif()
if(NOT ZC_WIDGETTOOLS_LIB)
    message(FATAL_ERROR "Missing ZcWidgetTools library. Put libZcWidgetTools.a/.dylib or ZcWidgetTools.lib in 3rdparty/ZcWidgetTools/lib")
endif()
if(NOT ZCAI_LIB)
    message(FATAL_ERROR "Missing ZcAiLib library. Put libZcAiLib.a/.dylib or ZcAiLib.lib in 3rdparty/ZcAILib/lib")
endif()

target_link_libraries(ZcVersionBox PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    ${ZCAI_LIB}
    ${ELA_WIDGETTOOLS_LIB}
    ${ZC_WIDGETTOOLS_LIB}
)

if(APPLE)
    set_target_properties(ZcVersionBox PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    function(copy_macos_runtime_library target library_path runtime_name)
        if(library_path MATCHES "\\.dylib$")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/../Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${library_path}"
                        "$<TARGET_FILE_DIR:${target}>/../Frameworks/${runtime_name}"
                COMMENT "Copying ${runtime_name} into macOS app bundle"
            )
        endif()
    endfunction()

    copy_macos_runtime_library(ZcVersionBox "${ZCAI_LIB}" "libZcAiLib.1.dylib")
    copy_macos_runtime_library(ZcVersionBox "${ELA_WIDGETTOOLS_LIB}" "libElaWidgetTools.dylib")
    copy_macos_runtime_library(ZcVersionBox "${ZC_WIDGETTOOLS_LIB}" "libZcWidgetTools.1.dylib")
endif()

if(WIN32 AND QT_VERSION_MAJOR EQUAL 6)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    if(QT_QMAKE_EXECUTABLE)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    endif()

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ZcVersionBox POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-compiler-runtime --no-translations "$<TARGET_FILE:ZcVersionBox>"
            COMMENT "Deploying Qt runtime"
        )
    endif()

    set(ELA_WIDGET_TOOLS_DLL "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ElaWidgetTools/bin/elawidgettools.dll")
    if(EXISTS "${ELA_WIDGET_TOOLS_DLL}")
        add_custom_command(TARGET ZcVersionBox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ELA_WIDGET_TOOLS_DLL}"
                    "$<TARGET_FILE_DIR:ZcVersionBox>/elawidgettools.dll"
            COMMENT "Copying ElaWidgetTools runtime"
        )
    endif()

    set(ZC_WIDGET_TOOLS_DLL "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcWidgetTools/bin/ZcWidgetTools.dll")
    if(EXISTS "${ZC_WIDGET_TOOLS_DLL}")
        add_custom_command(TARGET ZcVersionBox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ZC_WIDGET_TOOLS_DLL}"
                    "$<TARGET_FILE_DIR:ZcVersionBox>/ZcWidgetTools.dll"
            COMMENT "Copying ZcWidgetTools runtime"
        )
    endif()

    set(ZCAI_DLL "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcAILib/bin/ZcAiLib.dll")
    if(EXISTS "${ZCAI_DLL}")
        add_custom_command(TARGET ZcVersionBox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ZCAI_DLL}"
                    "$<TARGET_FILE_DIR:ZcVersionBox>/ZcAiLib.dll"
            COMMENT "Copying ZcAiLib runtime"
        )
    endif()
endif()

set_target_properties(ZcVersionBox PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.zc.versionbox
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE FALSE
)

include(GNUInstallDirs)
install(TARGETS ZcVersionBox
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(ZcVersionBox)
endif()
