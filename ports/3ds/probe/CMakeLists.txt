# Aleph One 3DS toolchain probe.
#
#   cmake -S ports/3ds/probe -B build/3ds-probe \
#         -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/3ds-probe
#
# Produces build/3ds-probe/aleph-probe.3dsx.
cmake_minimum_required(VERSION 3.13)

project(aleph-probe LANGUAGES CXX)

# NINTENDO_3DS is set by the platform file, which project() loads.
if(NOT NINTENDO_3DS)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake")
endif()

option(PROBE_WITH_SDL "Include the SDL2 window test" ON)

add_executable(aleph-probe source/main.cpp)

# Match the flags the Aleph One port will use. Set explicitly rather than via
# CMAKE_CXX_STANDARD so the exact flags are visible in the build log.
target_compile_options(aleph-probe PRIVATE
  -std=gnu++17 -fexceptions -frtti
  -Wall -Wextra
)

if(PROBE_WITH_SDL)
  find_package(SDL2 REQUIRED)
  target_compile_definitions(aleph-probe PRIVATE PROBE_WITH_SDL)
  target_link_libraries(aleph-probe PRIVATE SDL2::SDL2)
endif()

ctr_generate_smdh(aleph-probe.smdh
  NAME        "Aleph Probe"
  DESCRIPTION "Aleph One 3DS toolchain probe"
  AUTHOR      "Aleph One 3DS port"
)

ctr_create_3dsx(aleph-probe
  SMDH aleph-probe.smdh
)
