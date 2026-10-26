cmake_minimum_required(VERSION 3.21)

set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "" FORCE)
set(CMAKE_TOOLCHAIN_FILE "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

project(SkyrimQuestManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_XBYAK ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibSSE-NG.git
    GIT_TAG        ng
)
FetchContent_MakeAvailable(CommonLibSSE)

find_package(xbyak CONFIG REQUIRED)

# Eigene Plugin-Quelldateien
set(PLUGIN_SOURCES
    src/main.cpp
    src/D3DHook.cpp
    src/QuestManager.cpp
)

# Alle ImGui .cpp Dateien im Ordner src/imgui/ automatisch erfassen
file(GLOB IMGUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/*.cpp")

# Wichtig: Precompiled Header (pch.h) für ImGui-Dateien deaktivieren,
# damit du nicht in jede imgui_*.cpp "#include pch.h" schreiben musst!
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_library(${PROJECT_NAME} SHARED
    ${PLUGIN_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    xbyak::xbyak
    d3d11
    dxgi
)

target_compile_options(${PROJECT_NAME} PRIVATE
    /utf-8
    /W3
)