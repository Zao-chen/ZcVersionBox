cmake_minimum_required(VERSION 3.16)

project(ZcVersionBox VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(ZcVersionBox
        MANUAL_FINALIZATION
        include/ElaWidgetTools.qrc
        ${PROJECT_SOURCES}
        mainwindow_child/homepage.h mainwindow_child/homepage.cpp mainwindow_child/homepage.ui
        mainwindow_child/homepagechild_trackfile.h mainwindow_child/homepagechild_trackfile.cpp mainwindow_child/homepagechild_trackfile.ui
        mainwindow_child/settingpage.h mainwindow_child/settingpage.cpp mainwindow_child/settingpage.ui
        GlobalConstants.h
        res.qrc
        mainwindow_child/homepagechild_backupfile.h mainwindow_child/homepagechild_backupfile.cpp mainwindow_child/homepagechild_backupfile.ui
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

target_link_libraries(ZcVersionBox PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
${CMAKE_CURRENT_SOURCE_DIR}/lib/elawidgettools.lib
)

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
    WIN32_EXECUTABLE TRUE
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
