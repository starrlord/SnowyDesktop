cmake_minimum_required(VERSION 3.20)

# Release builds pass -DSNOWYSTARR_VERSION=x.y.z (CI takes it from the git tag).
set(SNOWYSTARR_VERSION "1.2.0" CACHE STRING "snowystarr version (major.minor.patch)")
if(NOT SNOWYSTARR_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "SNOWYSTARR_VERSION must look like 1.2.3 (got '${SNOWYSTARR_VERSION}').")
endif()
project(snowystarr VERSION ${SNOWYSTARR_VERSION} LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "snowystarr is a Windows application.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so the single exe runs without a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(snowystarr WIN32
    src/main.cpp
    src/App.cpp
    src/App.h
    src/Common.h
    src/Drift.cpp
    src/Drift.h
    src/FlakeArt.cpp
    src/FlakeArt.h
    src/Lights.cpp
    src/Lights.h
    src/LightsArt.cpp
    src/LightsEditor.cpp
    src/LightsEditor.h
    src/LightsHud.cpp
    src/LightsHud.h
    src/LightsParts.h
    src/Noise.h
    src/Renderer.cpp
    src/Renderer.h
    src/Scene.cpp
    src/Scene.h
    src/Settings.cpp
    src/Settings.h
    src/SettingsWindow.cpp
    src/SettingsWindow.h
    src/Scenery.cpp
    src/Scenery.h
    src/SceneryArt.cpp
    src/SceneryParts.h
    res/resource.h
    res/snowystarr.rc
    res/snowystarr.manifest
)

target_include_directories(snowystarr PRIVATE src res)

# Developer aids: offscreen settings snapshot (SNOWYSTARR_SNAPSHOT=<png>), faster drifts for
# screenshots (SNOWYSTARR_DRIFT_BOOST=<n>), keep rendering under the screen saver (SNOWYSTARR_IGNORE_AWAY=1).
option(SNOWYSTARR_DEV_TOOLS "Build developer test hooks" OFF)
if(SNOWYSTARR_DEV_TOOLS)
    target_compile_definitions(snowystarr PRIVATE SNOWYSTARR_DEV_TOOLS)
endif()

# Applies to both the C++ and the resource compiler (VERSIONINFO).
target_compile_definitions(snowystarr PRIVATE
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
    SNOWYSTARR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SNOWYSTARR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    SNOWYSTARR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    SNOWYSTARR_VERSION_STR="${PROJECT_VERSION}"
)

if(MSVC)
    # Control Flow Guard + CET shadow stacks; release PDBs for crash analysis.
    target_compile_options(snowystarr PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /MP /guard:cf>
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2 /Oi /GL /Gw /Zi>)
    target_link_options(snowystarr PRIVATE /guard:cf /CETCOMPAT
        $<$<CONFIG:Release>:/LTCG /DEBUG /OPT:REF /OPT:ICF>)
endif()

target_link_libraries(snowystarr PRIVATE
    d3d11 dxgi d2d1 dcomp dwrite dwmapi windowscodecs
    shcore shell32 shlwapi comctl32 uxtheme ole32 user32 gdi32 advapi32 wtsapi32 powrprof comdlg32
    runtimeobject  # C++/WinRT (the MSIX startup task). Not windowsapp: that would move every
                   # kernel32 import to API sets, which older Windows 10 builds may not all have.
)

# lightslab: renders the holiday lights offscreen to a PNG, for working on
# their look and motion (see tools/lightslab.cpp).
if(SNOWYSTARR_DEV_TOOLS)
    add_executable(lightslab
        tools/lightslab.cpp
        src/Drift.cpp
        src/FlakeArt.cpp
        src/Lights.cpp
        src/LightsArt.cpp
        src/Scene.cpp
        src/Scenery.cpp
        src/SceneryArt.cpp
        src/Settings.cpp
        res/snowystarr.rc
    )
    target_include_directories(lightslab PRIVATE src res)
    target_compile_definitions(lightslab PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
        SNOWYSTARR_DEV_TOOLS
        SNOWYSTARR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        SNOWYSTARR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        SNOWYSTARR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        SNOWYSTARR_VERSION_STR="${PROJECT_VERSION}"
    )
    if(MSVC)
        target_compile_options(lightslab PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /MP>
            $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2>)
    endif()
    target_link_libraries(lightslab PRIVATE d3d11 dxgi d2d1 dwrite windowscodecs ole32 user32 gdi32 advapi32 shell32
        runtimeobject)

    # lightsedit_test: the hanging-mode editor's checks, run without any windows
    # (see tools/lightsedit_test.cpp); --hud also renders the HUD to a PNG.
    add_executable(lightsedit_test
        tools/lightsedit_test.cpp
        src/Drift.cpp
        src/FlakeArt.cpp
        src/Lights.cpp
        src/LightsArt.cpp
        src/LightsEditor.cpp
        src/LightsHud.cpp
        src/Scene.cpp
        src/Scenery.cpp
        src/SceneryArt.cpp
        src/Settings.cpp
    )
    target_include_directories(lightsedit_test PRIVATE src res)
    target_compile_definitions(lightsedit_test PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
        SNOWYSTARR_DEV_TOOLS
    )
    if(MSVC)
        target_compile_options(lightsedit_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /MP>
            $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2>)
    endif()
    target_link_libraries(lightsedit_test PRIVATE d3d11 dxgi d2d1 dwrite windowscodecs shcore ole32 user32 gdi32 advapi32 shell32
        runtimeobject)

    # settingslab: renders the settings window offscreen to PNGs, both tabs in
    # both themes, for working on its layout (see tools/settingslab.cpp).
    add_executable(settingslab
        tools/settingslab.cpp
        src/Drift.cpp
        src/FlakeArt.cpp
        src/Lights.cpp
        src/LightsArt.cpp
        src/Scene.cpp
        src/Scenery.cpp
        src/SceneryArt.cpp
        src/Settings.cpp
        src/SettingsWindow.cpp
        res/snowystarr.rc
    )
    target_include_directories(settingslab PRIVATE src res)
    target_compile_definitions(settingslab PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
        SNOWYSTARR_DEV_TOOLS
        SNOWYSTARR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        SNOWYSTARR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        SNOWYSTARR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        SNOWYSTARR_VERSION_STR="${PROJECT_VERSION}"
    )
    if(MSVC)
        target_compile_options(settingslab PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /MP>
            $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2>)
    endif()
    target_link_libraries(settingslab PRIVATE d3d11 dxgi d2d1 dwrite dwmapi windowscodecs shcore shell32 comdlg32
        comctl32 uxtheme ole32 user32 gdi32 advapi32 runtimeobject)
endif()
