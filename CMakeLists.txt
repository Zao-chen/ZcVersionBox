cmake_minimum_required(VERSION 3.16)

project(ZcVersionBox VERSION 0.1 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

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
        windows/mainwindow_child/homepage/homepagechild_trackfile.h windows/mainwindow_child/homepage/homepagechild_trackfile.cpp windows/mainwindow_child/homepage/homepagechild_trackfile.ui
        windows/mainwindow_child/settingpage/settingpage.h windows/mainwindow_child/settingpage/settingpage.cpp windows/mainwindow_child/settingpage/settingpage.ui
        GlobalConstants.h
        res.qrc

        utils/fileutils.h utils/fileutils.cpp
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

target_include_directories(ZcVersionBox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ElaWidgetTools/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcWidgetTools/include
)

target_link_libraries(ZcVersionBox PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ElaWidgetTools/lib/elawidgettools.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ZcWidgetTools/lib/ZcWidgetTools.lib
)

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
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.ZcVersionBox)
endif()
set_target_properties(ZcVersionBox PROPERTIES
    ${BUNDLE_ID_OPTION}
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
