cmake_minimum_required(VERSION 3.21)

project(BareboneQt
    VERSION 0.1.0
    DESCRIPTION "Qt6 application shell inspired by Bitcoin-Qt"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Network WebChannel WebEngineWidgets Widgets)

qt_standard_project_setup()

qt_add_executable(BareboneQtApp
    resources/resources.qrc
    src/main.cpp
    src/core/ConfigManager.cpp
    src/core/ConfigManager.h
    src/ui/AiWebBridge.cpp
    src/ui/AiWebBridge.h
    src/ui/BricsCadPage.cpp
    src/ui/BricsCadPage.h
    src/ui/ChatPage.cpp
    src/ui/ChatPage.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/SettingsPage.cpp
    src/ui/SettingsPage.h
    src/ui/TemplatePage.cpp
    src/ui/TemplatePage.h
)

if(WIN32)
    target_sources(BareboneQtApp PRIVATE
        resources/app.manifest
        resources/windows.rc
    )
endif()

target_link_libraries(BareboneQtApp PRIVATE Qt6::Network Qt6::WebChannel Qt6::WebEngineWidgets Qt6::Widgets)

target_compile_definitions(BareboneQtApp PRIVATE
    QT_NO_KEYWORDS
    APP_VERSION="${PROJECT_VERSION}"
)

set_target_properties(BareboneQtApp PROPERTIES
    OUTPUT_NAME "Barebone-Qt"
    QT_QML_MODULE_NO_IMPORT_SCAN TRUE
)

if(WIN32)
    set_target_properties(BareboneQtApp PROPERTIES WIN32_EXECUTABLE TRUE)

    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt6
        HINTS "${Qt6_DIR}/../../../bin"
        PATH_SUFFIXES bin
    )

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET BareboneQtApp POST_BUILD
            COMMAND powershell
                -NoProfile
                -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/windows/fix-embedded-manifest.ps1"
                -ExePath "$<TARGET_FILE:BareboneQtApp>"
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:BareboneQtApp>"
            COMMENT "Fixing embedded Windows manifest and deploying Qt runtime with windeployqt"
        )
    else()
        message(WARNING "windeployqt was not found. The Windows executable will need Qt DLLs deployed manually.")
    endif()
endif()

if(APPLE)
    find_program(SIPS_EXECUTABLE NAMES sips REQUIRED)
    find_program(ICONUTIL_EXECUTABLE NAMES iconutil REQUIRED)

    set(BAREBONE_MACOS_ICONSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/BareboneQtApp.iconset")
    set(BAREBONE_MACOS_ICON_FILE "${CMAKE_CURRENT_BINARY_DIR}/AppLogo.icns")

    add_custom_command(
        OUTPUT "${BAREBONE_MACOS_ICON_FILE}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${BAREBONE_MACOS_ICONSET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BAREBONE_MACOS_ICONSET_DIR}"
        COMMAND "${SIPS_EXECUTABLE}" -z 16 16 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_16x16.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 32 32 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_16x16@2x.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 32 32 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_32x32.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 64 64 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_32x32@2x.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 128 128 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_128x128.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 256 256 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_128x128@2x.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 256 256 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_256x256.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 512 512 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_256x256@2x.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 512 512 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_512x512.png"
        COMMAND "${SIPS_EXECUTABLE}" -z 1024 1024 "${CMAKE_SOURCE_DIR}/AppLogo.png" --out "${BAREBONE_MACOS_ICONSET_DIR}/icon_512x512@2x.png"
        COMMAND "${ICONUTIL_EXECUTABLE}" -c icns "${BAREBONE_MACOS_ICONSET_DIR}" -o "${BAREBONE_MACOS_ICON_FILE}"
        DEPENDS "${CMAKE_SOURCE_DIR}/AppLogo.png"
        VERBATIM
    )

    set_source_files_properties("${BAREBONE_MACOS_ICON_FILE}" PROPERTIES
        GENERATED TRUE
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(BareboneQtApp PRIVATE "${BAREBONE_MACOS_ICON_FILE}")

    set_target_properties(BareboneQtApp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Barebone-Qt"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.barebone.qt"
        MACOSX_BUNDLE_ICON_FILE "AppLogo.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

set(BAREBONE_BRX_SDK_PATH "C:/Program Files/Bricsys/BRXSDK/BRX26.1.05.0" CACHE PATH "Path to the BricsCAD BRX SDK")
option(BAREBONE_BUILD_BRX_PLUGIN "Build the optional BricsCAD BRX plugin" OFF)

if(BAREBONE_BUILD_BRX_PLUGIN)
    if(NOT MSVC)
        message(FATAL_ERROR "The BricsCAD BRX V26 plugin must be built with MSVC / Visual Studio 2022.")
    endif()

    if(NOT EXISTS "${BAREBONE_BRX_SDK_PATH}/inc64" OR NOT EXISTS "${BAREBONE_BRX_SDK_PATH}/inc" OR NOT EXISTS "${BAREBONE_BRX_SDK_PATH}/lib64")
        message(FATAL_ERROR "BRX SDK not found at BAREBONE_BRX_SDK_PATH='${BAREBONE_BRX_SDK_PATH}'.")
    endif()

    add_library(BareboneBrx MODULE
        src/bricscad/BareboneBrxPlugin.cpp
    )

    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The BricsCAD BRX V26 plugin must be built for x64.")
    endif()
    if(MSVC_VERSION LESS 1930 OR MSVC_VERSION GREATER_EQUAL 1950)
        message(FATAL_ERROR
            "The BricsCAD BRX V26 plugin requires the Visual Studio 2022 v143 toolset "
            "(MSVC_VERSION 1930..1949); active MSVC_VERSION=${MSVC_VERSION}.")
    endif()
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION
       AND CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.19041.0")
        message(FATAL_ERROR "The BricsCAD BRX V26 plugin requires Windows SDK 10.0.19041.0 or newer.")
    endif()
    if(CMAKE_SYSTEM_VERSION AND CMAKE_SYSTEM_VERSION VERSION_LESS "10.0.19041.0")
        message(FATAL_ERROR "The BricsCAD BRX V26 plugin requires Windows SDK 10.0.19041.0 or newer.")
    endif()

    target_compile_features(BareboneBrx PRIVATE cxx_std_20)
    target_compile_definitions(BareboneBrx PRIVATE
        BRX_APP
        UNICODE
        _UNICODE
        __BRXTARGET=26
    )
    target_compile_options(BareboneBrx PRIVATE /EHa)
    target_include_directories(BareboneBrx PRIVATE
        "${BAREBONE_BRX_SDK_PATH}/inc64"
        "${BAREBONE_BRX_SDK_PATH}/inc"
    )
    target_link_directories(BareboneBrx PRIVATE
        "${BAREBONE_BRX_SDK_PATH}/lib64"
    )
    target_link_libraries(BareboneBrx PRIVATE
        "${BAREBONE_BRX_SDK_PATH}/lib64/brx26.lib"
        "${BAREBONE_BRX_SDK_PATH}/lib64/drx_entrypoint.lib"
        ws2_32
    )
    set_target_properties(BareboneBrx PROPERTIES
        AUTOMOC OFF
        AUTORCC OFF
        AUTOUIC OFF
        PREFIX ""
        SUFFIX ".brx"
        OUTPUT_NAME "BareboneBrx"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bricscad"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bricscad"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bricscad"
    )
endif()

install(TARGETS BareboneQtApp
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
