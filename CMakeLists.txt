cmake_minimum_required(VERSION 3.20)

project(InfinityEngine-Enhancer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(CMAKE_GENERATOR_TOOLSET "v143")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(IEE_BUILD_WINDOWS_DLL "Build the EEex DLL target" ${WIN32})

if(IEE_BUILD_WINDOWS_DLL AND NOT WIN32)
    message(FATAL_ERROR "IEE_BUILD_WINDOWS_DLL requires a Windows toolchain")
endif()

include(CTest)
include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable FetchContent updates after the initial clone" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)
FetchContent_MakeAvailable(spdlog)

set(IEE_VERSION "0.0.0-dev")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE IEE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(IEE_VERSION STREQUAL "")
    set(IEE_VERSION "0.0.0-dev")
endif()

message(STATUS "InfinityEngine-Enhancer version: ${IEE_VERSION}")

set(IEE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/iee/core/version.h.in"
    "${IEE_GENERATED_DIR}/iee/core/version.h"
    @ONLY
)

add_library(iee_common STATIC
    src/iee/core/config.cpp
    src/iee/core/logger.cpp
    src/iee/core/pattern_scanner.cpp
    src/iee/game/build_manifest.cpp
    src/iee/game/game_addrs.cpp
    src/iee/game/tile_upscale.cpp
    src/iee/game/tis_runtime.cpp
)

target_include_directories(iee_common
    PUBLIC
        "${CMAKE_SOURCE_DIR}/src"
        "${IEE_GENERATED_DIR}"
)

target_link_libraries(iee_common
    PUBLIC
        spdlog::spdlog
)

target_compile_features(iee_common PUBLIC cxx_std_20)

if(IEE_BUILD_WINDOWS_DLL)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
    )
    FetchContent_MakeAvailable(minhook)

    add_library(InfinityEngine-Enhancer SHARED
        src/iee/dll_main.cpp
        src/iee/exports.cpp
        src/iee/hooks.cpp
        src/iee/game/opengl_types.cpp
        src/iee/game/renderer.cpp
    )

    target_link_libraries(InfinityEngine-Enhancer
        PRIVATE
            iee_common
            minhook
            psapi
            opengl32
    )

    target_compile_definitions(InfinityEngine-Enhancer
        PRIVATE
            _WINDOWS
            _USRDLL
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    set_target_properties(InfinityEngine-Enhancer PROPERTIES OUTPUT_NAME "InfinityEngine-Enhancer")

    set(IEE_RELEASE_BUNDLE_DIR "${CMAKE_BINARY_DIR}/release-bundle")
    add_custom_target(release_bundle
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${IEE_RELEASE_BUNDLE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${IEE_RELEASE_BUNDLE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:InfinityEngine-Enhancer>" "${IEE_RELEASE_BUNDLE_DIR}/InfinityEngine-Enhancer.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/tools/M_IEEE.lua" "${IEE_RELEASE_BUNDLE_DIR}/M_IEEE.lua"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/tools/InfinityEngine-Enhancer.sample.ini" "${IEE_RELEASE_BUNDLE_DIR}/InfinityEngine-Enhancer.sample.ini"
        DEPENDS InfinityEngine-Enhancer
        VERBATIM
    )

    install(TARGETS InfinityEngine-Enhancer RUNTIME DESTINATION . LIBRARY DESTINATION .)
    install(
        FILES
            "${CMAKE_SOURCE_DIR}/tools/M_IEEE.lua"
            "${CMAKE_SOURCE_DIR}/tools/InfinityEngine-Enhancer.sample.ini"
        DESTINATION .
    )
else()
    message(STATUS "Windows DLL target disabled on this host; building iee_common and tests only.")
endif()

if(BUILD_TESTING)
    add_executable(iee_tests
        tests/iee_tests.cpp
    )

    target_link_libraries(iee_tests PRIVATE iee_common)
    add_test(NAME iee_tests COMMAND iee_tests)
endif()
