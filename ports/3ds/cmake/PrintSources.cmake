# Script mode: write the source list the 3DS build derives from
# Source_Files/**/Makefile.am, without configuring a build.
#
#   cmake -DOUTPUT=<file> -P ports/3ds/cmake/PrintSources.cmake
#
# One path per line, relative to Source_Files, sorted. Used by
# scripts/3ds/check-sources.sh. The configure step of ports/3ds writes the same
# list to <build dir>/aleph-sources.txt.
cmake_minimum_required(VERSION 3.24)
if(NOT OUTPUT)
  message(FATAL_ERROR "usage: cmake -DOUTPUT=<file> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/A1AutomakeConditions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AutomakeSources.cmake")
a1_automake_collect(TOP_SRCDIR "${_repo}" START Source_Files PROGRAMS alephone PREFIX A1AM)
a1_automake_write_list(PREFIX A1AM BASE "${_repo}/Source_Files" OUTPUT "${OUTPUT}")
foreach(_u IN LISTS A1AM_UNITS)
  list(LENGTH A1AM_${_u}_SOURCES _n)
  file(RELATIVE_PATH _d "${_repo}" "${A1AM_${_u}_DIR}")
  message(STATUS "${_d}: ${A1AM_${_u}_NAME}: ${_n} sources")
endforeach()
