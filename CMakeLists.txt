cmake_minimum_required(VERSION 3.24)
project(torchbearer LANGUAGES C CXX)
set(TB_VERSION "0.1" CACHE STRING "version string")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# dxgi.dll proxy: Proton loads native dxgi from the game folder first (dxgi=n), so the game picks
# this up; it forwards every export to the real dxgi in the system directory (DXVK under Proton).
add_library(torchbearer SHARED
  src/proxy.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/hook.cpp
  src/core/lab.cpp
  src/core/quiver.cpp
  src/core/present.cpp
  src/core/module.cpp
  src/features/features.cpp
  src/features/stereo/stereo.cpp
  src/features/stereo/gpu.cpp
  src/features/intro.cpp
  src/features/settings.cpp
  src/features/settings_keys.cpp
  src/features/levels.cpp
  src/dxgi.def)
set_target_properties(torchbearer PROPERTIES OUTPUT_NAME dxgi PREFIX "")
target_include_directories(torchbearer PRIVATE src third_party)
target_compile_definitions(torchbearer PRIVATE TB_VERSION="${TB_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  __USE_MINGW_ANSI_STDIO=1)
target_compile_options(torchbearer PRIVATE -Wall -Wno-unknown-pragmas -fno-strict-aliasing)
target_link_options(torchbearer PRIVATE -static -static-libgcc -static-libstdc++
  -Wl,--enable-stdcall-fixup)
target_link_libraries(torchbearer PRIVATE minhook dxguid uuid)

# Everything a mod folder needs: the DLL plus the shipped config and Quiver schema. Re-copied on
# every build (not only when the DLL relinks), so dist/ edits always reach build/mod.
add_custom_target(mod ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/mod
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:torchbearer> ${CMAKE_BINARY_DIR}/mod/dxgi.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/torchbearer.ini
    ${CMAKE_SOURCE_DIR}/dist/quiver-mod.toml ${CMAKE_SOURCE_DIR}/dist/quiver-options.toml
    ${CMAKE_BINARY_DIR}/mod/
  DEPENDS torchbearer)
