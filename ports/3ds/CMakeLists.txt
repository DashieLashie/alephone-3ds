# Aleph One for the Nintendo 3DS (devkitARM + libctru).
#
# From the repository root:
#
#   cmake -S ports/3ds -B build-3ds
#   cmake --build build-3ds            # -> build-3ds/alephone.3dsx
#
# The toolchain defaults to $DEVKITPRO/cmake/3DS.cmake. Dependencies are taken
# from $DEVKITPRO/portlibs/3ds (see scripts/3ds/*.sh and docs/3ds/).
#
# The engine sources are not listed here: they are derived at configure time
# from the upstream Source_Files/**/Makefile.am (cmake/AutomakeSources.cmake)
# with the AM_CONDITIONAL values in cmake/A1AutomakeConditions.cmake, so this
# build compiles exactly what automake compiles for the same feature set
# (no network, no OpenGL, no Steam, no zzip, no tests). Upstream sources are
# not modified or rearranged by this file.
#
# Options:
#   A1_3DS_PKGDATADIR  PKGDATADIR written to confpaths.h (provisional)
cmake_minimum_required(VERSION 3.24)   # $<LINK_GROUP:RESCAN,...>

# --- Toolchain (must be settled before project()) -------------------------
if(NOT CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{DEVKITPRO} OR "$ENV{DEVKITPRO}" STREQUAL "")
    message(FATAL_ERROR
      "DEVKITPRO is not set. Export DEVKITPRO=/opt/devkitpro (see docs/3ds/ENVIRONMENT.md) "
      "or pass -DCMAKE_TOOLCHAIN_FILE=<devkitPro>/cmake/3DS.cmake.")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE FILEPATH
      "devkitPro 3DS toolchain file")
endif()
if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(FATAL_ERROR "Toolchain file ${CMAKE_TOOLCHAIN_FILE} not found "
                      "(install 3ds-cmake: scripts/3ds/setup-env.sh)")
endif()

get_property(_a1_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _a1_multi_config AND NOT CMAKE_BUILD_TYPE)
  # devkitPro's Release flags are "-g -O2 -DNDEBUG" (dkp-rule-overrides.cmake).
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(AlephOne3DS LANGUAGES C CXX)

# NINTENDO_3DS is set by Platform/Nintendo3DS.cmake, which project() loads.
if(NOT NINTENDO_3DS)
  message(FATAL_ERROR "This is the Nintendo 3DS build; configure it with "
                      "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake")
endif()

get_filename_component(A1_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(A1_SOURCE_FILES "${A1_REPO_ROOT}/Source_Files")
set(A1_PORTLIBS "${DEVKITPRO}/portlibs/3ds")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---------------------------------------------------------------
set(A1_3DS_PKGDATADIR "sdmc:/3ds/alephone" CACHE STRING
    "PKGDATADIR for confpaths.h (default data path). Provisional: 3DS runtime data paths are a later stage (docs/3ds/AUDIT.md f)")
set(A1_3DS_SMDH_TITLE "Aleph One" CACHE STRING "SMDH title")
set(A1_3DS_SMDH_DESCRIPTION "Marathon engine" CACHE STRING "SMDH short description")
set(A1_3DS_SMDH_AUTHOR "Aleph One developers / 3DS port by DashieLashie" CACHE STRING "SMDH author")
set(A1_3DS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png" CACHE FILEPATH "48x48 SMDH icon")

# --- Compiler flags ----------------------------------------------------------
# The toolchain supplies -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# -mword-relocations -ffunction-sections -D__3DS__ (CMAKE_<LANG>_FLAGS_INIT);
# the Release configuration adds -g -O2 -DNDEBUG.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)         # -std=gnu++17
# C: no -std, like automake (the compiler default).

string(TOUPPER "CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}" _a1_cfg_flags_var)
foreach(_a1_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_INIT ${_a1_cfg_flags_var})
  if("${${_a1_var}}" MATCHES "-fno-exceptions|-fno-rtti")
    message(FATAL_ERROR "${_a1_var} contains -fno-exceptions/-fno-rtti ('${${_a1_var}}'); "
                        "Aleph One needs C++ exceptions and RTTI")
  endif()
endforeach()
if(NOT "${CMAKE_CXX_FLAGS} ${${_a1_cfg_flags_var}}" MATCHES "-O2")
  message(WARNING "-O2 is not in CMAKE_CXX_FLAGS / ${_a1_cfg_flags_var} "
                  "('${CMAKE_CXX_FLAGS} ${${_a1_cfg_flags_var}}'); the port is meant to be built with -O2")
endif()

add_compile_options(
  "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>"
  "$<$<COMPILE_LANGUAGE:CXX>:-frtti>"
)
add_compile_definitions(HAVE_CONFIG_H A1_PLATFORM_3DS)
if(NOT "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "-D__3DS__")
  add_compile_definitions(__3DS__)   # normally already in the toolchain flags
endif()

# --- config.h ----------------------------------------------------------------
# Engine TUs include "config.h" (HAVE_CONFIG_H); it must be ports/3ds/config.h.
# A config.h left in the source tree by an in-tree autotools run would be
# found first by quoted includes, so refuse to configure if there is one.
file(GLOB_RECURSE _a1_stray_config LIST_DIRECTORIES false "${A1_SOURCE_FILES}/config.h")
if(EXISTS "${A1_REPO_ROOT}/config.h")
  list(APPEND _a1_stray_config "${A1_REPO_ROOT}/config.h")
endif()
if(_a1_stray_config)
  message(FATAL_ERROR "Found config.h outside ports/3ds, probably from an in-tree ./configure run: "
                      "${_a1_stray_config}\nIt could shadow ports/3ds/config.h; remove it "
                      "(e.g. 'make distclean') and re-run cmake.")
endif()

set(A1_CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/config.h")
set(_a1_alephversion "${A1_SOURCE_FILES}/Misc/alephversion.h")
file(STRINGS "${_a1_alephversion}" _a1_datever REGEX "^#define A1_DATE_VERSION ")
file(STRINGS "${A1_CONFIG_H}" _a1_pkgver REGEX "^#define PACKAGE_VERSION ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" _a1_datever "${_a1_datever}")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" _a1_pkgver "${_a1_pkgver}")
if(NOT _a1_datever STREQUAL _a1_pkgver)
  message(WARNING "ports/3ds/config.h PACKAGE_VERSION (${_a1_pkgver}) differs from "
                  "A1_DATE_VERSION in Misc/alephversion.h (${_a1_datever}); update config.h")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A1_CONFIG_H}" "${_a1_alephversion}")

# confpaths.h: automake generates it in the CSeries build directory as
#   #define PKGDATADIR "$(pkgdatadir)"
# (Source_Files/CSeries/Makefile.am); CSeries/cspaths_sdl.cpp includes it.
set(A1_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${A1_GENERATED_DIR}/confpaths.h"
     CONTENT "#define PKGDATADIR \"@A1_3DS_PKGDATADIR@\"\n" @ONLY)

# --- Dependencies --------------------------------------------------------------
# pkg-config is devkitPro's arm-none-eabi-pkg-config (found by the toolchain);
# it always passes --static, so the link lines below are the static ones.
find_package(PkgConfig REQUIRED)
message(STATUS "pkg-config: ${PKG_CONFIG_EXECUTABLE}")

# One query, so the libraries keep the relative order pkg-config gives for
# static linking (SDL2_ttf SDL2main SDL2 ctru m freetype bz2 png16 m m ctru z;
# CMake repeats/merges some entries on the final link line). ctru and m are
# also appended last by the toolchain's CMAKE_CXX_STANDARD_LIBRARIES.
pkg_check_modules(A1_SDL REQUIRED IMPORTED_TARGET
  "sdl2>=2.0.16" SDL2_ttf freetype2 libpng zlib)

add_library(a1_deps INTERFACE)

pkg_check_modules(A1_OPENAL IMPORTED_TARGET "openal>=1.14.0")
if(NOT A1_OPENAL_FOUND)
  message(FATAL_ERROR "libopenal.a not found (no openal.pc in ${A1_PORTLIBS}/lib/pkgconfig); "
                      "run scripts/3ds/build-openal.sh")
endif()
target_link_libraries(a1_deps INTERFACE PkgConfig::A1_OPENAL)

pkg_check_modules(A1_SNDFILE IMPORTED_TARGET sndfile)
if(NOT A1_SNDFILE_FOUND)
  message(FATAL_ERROR "libsndfile.a not found (no sndfile.pc in ${A1_PORTLIBS}/lib/pkgconfig); "
                      "run scripts/3ds/build-libsndfile.sh")
endif()
target_link_libraries(a1_deps INTERFACE PkgConfig::A1_SNDFILE)

target_link_libraries(a1_deps INTERFACE PkgConfig::A1_SDL)

# Only static archives may be linked.
foreach(_a1_pkg A1_SDL A1_OPENAL A1_SNDFILE)
  if(TARGET PkgConfig::${_a1_pkg})
    get_target_property(_a1_libs PkgConfig::${_a1_pkg} INTERFACE_LINK_LIBRARIES)
    foreach(_a1_lib IN LISTS _a1_libs)
      if(_a1_lib MATCHES "\\.(so|dylib)(\\.|$)")
        message(FATAL_ERROR "${_a1_pkg} resolved to a shared library: ${_a1_lib}")
      endif()
    endforeach()
    list(JOIN _a1_libs " " _a1_libs)
    message(STATUS "${_a1_pkg} libraries: ${_a1_libs}")
  endif()
endforeach()

# Boost: headers only (A1_FS_STD, so no Boost.Filesystem/System libraries).
find_path(A1_BOOST_INCLUDE_DIR NAMES boost/version.hpp
          HINTS "${A1_PORTLIBS}/include"
          DOC "Boost headers (scripts/3ds/install-boost-headers.sh)")
if(NOT A1_BOOST_INCLUDE_DIR)
  message(FATAL_ERROR "Boost headers not found in ${A1_PORTLIBS}/include; "
                      "run scripts/3ds/install-boost-headers.sh")
endif()
file(STRINGS "${A1_BOOST_INCLUDE_DIR}/boost/version.hpp" _a1_boost_ver REGEX "^#define BOOST_VERSION ")
string(REGEX REPLACE "^#define BOOST_VERSION ([0-9]+).*" "\\1" _a1_boost_ver "${_a1_boost_ver}")
if(_a1_boost_ver LESS 106900)   # configure.ac: AX_BOOST_BASE([1.69.0])
  message(FATAL_ERROR "Boost ${_a1_boost_ver} in ${A1_BOOST_INCLUDE_DIR} is older than 1.69.0")
endif()
# The headers configure.ac checks with AX_CHECK_BOOST_HEADER.
foreach(_a1_h tokenizer.hpp unordered_map.hpp algorithm/string/predicate.hpp
              iostreams/stream_buffer.hpp property_tree/xml_parser.hpp
              system/system_error.hpp uuid/uuid.hpp)
  if(NOT EXISTS "${A1_BOOST_INCLUDE_DIR}/boost/${_a1_h}")
    message(FATAL_ERROR "boost/${_a1_h} missing from ${A1_BOOST_INCLUDE_DIR}")
  endif()
endforeach()
math(EXPR _a1_boost_major "${_a1_boost_ver} / 100000")
math(EXPR _a1_boost_minor "${_a1_boost_ver} / 100 % 1000")
math(EXPR _a1_boost_patch "${_a1_boost_ver} % 100")
message(STATUS "Boost headers: ${A1_BOOST_INCLUDE_DIR} "
               "(${_a1_boost_major}.${_a1_boost_minor}.${_a1_boost_patch})")
target_include_directories(a1_deps SYSTEM INTERFACE "${A1_BOOST_INCLUDE_DIR}")

# --- Engine sources, derived from Source_Files/**/Makefile.am ---------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/A1AutomakeConditions.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AutomakeSources.cmake")
a1_automake_collect(TOP_SRCDIR "${A1_REPO_ROOT}" START Source_Files
                    PROGRAMS alephone PREFIX A1AM)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${A1AM_MAKEFILES}
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/A1AutomakeConditions.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AutomakeSources.cmake")
a1_automake_write_list(PREFIX A1AM BASE "${A1_SOURCE_FILES}"
                       OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/aleph-sources.txt")

set(_a1_libs "")
set(_a1_total 0)
foreach(_u IN LISTS A1AM_UNITS)
  set(_srcs "${A1AM_${_u}_SOURCES}")
  list(LENGTH _srcs _n)
  math(EXPR _a1_total "${_a1_total} + ${_n}")
  file(RELATIVE_PATH _d "${A1_REPO_ROOT}" "${A1AM_${_u}_DIR}")
  message(STATUS "  ${_d}: ${A1AM_${_u}_NAME}: ${_n} sources")

  if(A1AM_${_u}_KIND STREQUAL "LIBRARY")
    set(_t "a1_${_u}")
    add_library(${_t} STATIC ${_srcs})
    set_target_properties(${_t} PROPERTIES OUTPUT_NAME "${_u}")   # lib<name>.a, as automake
    list(APPEND _a1_libs ${_t})
  else()
    set(_t "${_u}")
    add_executable(${_t} ${_srcs})
  endif()

  # Same include order as automake's compile line:
  #   DEFAULT_INCLUDES (-I. -I$(srcdir) -I$(top_builddir)), then AM_CPPFLAGS,
  #   then CPPFLAGS (dependency include directories, here as -isystem).
  # "." (the build dir, for confpaths.h) -> generated/, top_builddir
  # (config.h) -> ports/3ds.
  target_include_directories(${_t} PRIVATE
    "${A1_GENERATED_DIR}"
    "${A1AM_${_u}_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${A1AM_${_u}_INCLUDES})
  target_compile_definitions(${_t} PRIVATE ${A1AM_${_u}_DEFINES})
  if(A1AM_${_u}_OPTIONS)
    target_compile_options(${_t} PRIVATE ${A1AM_${_u}_OPTIONS})
  endif()
  target_link_libraries(${_t} PRIVATE a1_deps)
endforeach()
message(STATUS "Aleph One sources: ${_a1_total} (list: ${CMAKE_CURRENT_BINARY_DIR}/aleph-sources.txt)")

if(NOT TARGET alephone)
  message(FATAL_ERROR "Makefile.am parsing did not produce the alephone program")
endif()

# automake links the noinst libraries twice (alephone_LDADD) to resolve their
# circular references; a rescanned link group does the same. CMake only
# predefines the RESCAN feature for some host platforms, so define it for the
# GNU linker here.
foreach(_a1_lang C CXX)
  set(CMAKE_${_a1_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
  set(CMAKE_${_a1_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
endforeach()
list(JOIN _a1_libs "," _a1_group)
target_link_libraries(alephone PRIVATE "$<LINK_GROUP:RESCAN,${_a1_group}>" a1_deps)

# libsysbase hooks libctru does not provide (pthread keys, sched_yield);
# see source/sysbase_hooks.c and docs/3ds/PATCHES.md. Linked as an object,
# so its strong definitions replace libsysbase's weak references/stubs.
target_sources(alephone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/sysbase_hooks.c")

# --- Packaging: alephone.elf -> alephone.3dsx with embedded SMDH --------------
ctr_generate_smdh(alephone.smdh
  NAME        "${A1_3DS_SMDH_TITLE}"
  DESCRIPTION "${A1_3DS_SMDH_DESCRIPTION}"
  AUTHOR      "${A1_3DS_SMDH_AUTHOR}"
  ICON        "${A1_3DS_ICON}"
)
ctr_create_3dsx(alephone SMDH alephone.smdh)   # -> ${CMAKE_CURRENT_BINARY_DIR}/alephone.3dsx

message(STATUS "Summary: SDL2 ${A1_SDL_sdl2_VERSION}, SDL2_ttf ${A1_SDL_SDL2_ttf_VERSION}, "
               "audio: OpenAL Soft ${A1_OPENAL_VERSION}, decoding: libsndfile ${A1_SNDFILE_VERSION}, "
               "PKGDATADIR \"${A1_3DS_PKGDATADIR}\" (provisional), build type ${CMAKE_BUILD_TYPE}")
