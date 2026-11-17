cmake_minimum_required(VERSION 3.24)
project(VibeIDE VERSION 0.1.3 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "VibeIDE requires macOS.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VTerm REQUIRED IMPORTED_TARGET vterm>=0.3)

set(APP_ICONS assets/VibeIDE.icns assets/VibeIDE.png)
set_source_files_properties(${APP_ICONS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(VibeIDE MACOSX_BUNDLE
    ${APP_ICONS}
    src/main.cpp
    src/CommitDiffView.cpp
    src/CommitDiffSource.cpp
    src/CurrentChangesPanel.cpp
    src/FileTransfer.cpp
    src/FilesPanel.cpp
    src/FindBar.cpp
    src/GitActions.cpp
    src/GitFileList.cpp
    src/GitPanel.cpp
    src/GitSquash.cpp
    src/Pty.cpp
    src/RunConfigurations.cpp
    src/SyntaxHighlighting.cpp
    src/Terminal.cpp
    src/TextFileEditor.cpp
    src/Workspace.cpp
)
target_link_libraries(VibeIDE PRIVATE Qt6::Widgets PkgConfig::VTerm)
set_target_properties(VibeIDE PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.vibeide.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "VibeIDE"
    MACOSX_BUNDLE_ICON_FILE "VibeIDE.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
