cmake_minimum_required(VERSION 3.21)

project(BareboneQt
    VERSION 0.1.0
    DESCRIPTION "Windows-only Qt6 application shell inspired by Bitcoin-Qt"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(BareboneQtApp
    resources/resources.qrc
    src/main.cpp
    src/core/ConfigManager.cpp
    src/core/ConfigManager.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/SettingsPage.cpp
    src/ui/SettingsPage.h
    src/ui/TemplatePage.cpp
    src/ui/TemplatePage.h
    resources/app.manifest
)

if(WIN32)
    target_sources(BareboneQtApp PRIVATE resources/windows.rc)
endif()

target_link_libraries(BareboneQtApp PRIVATE Qt6::Widgets)

target_compile_definitions(BareboneQtApp PRIVATE
    QT_NO_KEYWORDS
    APP_VERSION="${PROJECT_VERSION}"
)

set_target_properties(BareboneQtApp PROPERTIES
    OUTPUT_NAME "Barebone-Qt"
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
else()
    message(FATAL_ERROR "Barebone-Qt is configured as a Windows-only template.")
endif()

install(TARGETS BareboneQtApp RUNTIME DESTINATION bin)
