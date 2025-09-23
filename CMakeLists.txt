  cmake_minimum_required(VERSION 3.16)
  project(InfinityEngine-Enhancer)

  # Only set toolset if using Visual Studio generator
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
      set(CMAKE_GENERATOR_TOOLSET "v143")  # VS 2022
  endif()

  set(CMAKE_CXX_STANDARD 20)  # C++20 is more widely supported than C++26
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  # Download dependencies automatically
  include(FetchContent)

  # MinHook for function hooking
  FetchContent_Declare(
          minhook
          GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
          GIT_TAG v1.3.4
  )

  # spdlog for enhanced logging
  FetchContent_Declare(
          spdlog
          GIT_REPOSITORY https://github.com/gabime/spdlog.git
          GIT_TAG v1.15.3
  )

  FetchContent_MakeAvailable(minhook spdlog)

  add_library(InfinityEngine-Enhancer SHARED
          # Core infrastructure
          src/iee/core/config.cpp
          src/iee/core/config.h
          src/iee/core/logger.cpp
          src/iee/core/logger.h
          src/iee/core/version.h
          src/iee/core/hooking.h
          src/iee/core/pattern_scanner.h
          src/iee/core/pattern_scanner.cpp
          src/iee/dll_main.cpp
          # Game-specific modules
          src/iee/game/game_addrs.h
          src/iee/game/game_addrs.cpp
          src/iee/game/game_types.h
          src/iee/game/opengl_types.h
          src/iee/game/opengl_types.cpp
          src/iee/game/renderer.h
          src/iee/game/renderer.cpp

          # Application modules
          src/iee/app_context.h
          src/iee/hooks.h
          src/iee/hooks.cpp
          src/iee/exports.cpp)
  target_compile_definitions(InfinityEngine-Enhancer PRIVATE _WINDOWS _USRDLL WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(InfinityEngine-Enhancer PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

  target_link_libraries(InfinityEngine-Enhancer PRIVATE
          minhook
          spdlog::spdlog
          psapi
          opengl32)