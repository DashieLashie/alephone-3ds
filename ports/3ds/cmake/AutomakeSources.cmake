# AutomakeSources.cmake: derive Aleph One's source lists from the upstream
# Source_Files/**/Makefile.am files, so the 3DS build compiles exactly what
# automake would compile for a given set of AM_CONDITIONAL values.
#
# This is deliberately a small subset of automake, enough for Aleph One's
# Makefile.am files, and it fails loudly on anything it does not understand
# instead of guessing:
#   - backslash-newline continuations are joined;
#   - "#" comments are stripped; tab-indented recipe lines are ignored;
#   - "if COND" / "if !COND" / "else" / "endif" are evaluated against the
#     variables A1_AM_COND_<COND> (ON/OFF), which the caller must set for every
#     conditional that appears; an unknown conditional is a fatal error;
#   - "VAR = value" and "VAR += value" are recorded (only in active branches);
#   - $(VAR) / ${VAR} are expanded recursively from variables defined in the
#     same Makefile.am, plus top_srcdir, srcdir, top_builddir and builddir;
#   - SUBDIRS is followed recursively;
#   - noinst_LIBRARIES and bin_PROGRAMS are read; <canon>_SOURCES gives each
#     one's sources, of which only compiled ones (.c .cc .cpp .cxx) are kept;
#   - AM_CPPFLAGS is split into -I include directories, -D definitions and
#     anything else (returned as plain options).
#
# Usable from a project (include()) and from script mode (cmake -P).
include_guard(GLOBAL)

set(_A1_AM_COMPILED_EXT "c;cc;cpp;cxx")
set(_A1_AM_IGNORED_EXT "h;hh;hpp;hxx;inc;frag;vert")

# _a1_am_expand(<out> <text>)
# Expands $(VAR) and ${VAR} using the _a1v_<VAR> variables visible in the
# calling scope (set by a1_am_parse_file).
function(_a1_am_expand out text)
  set(_iter 0)
  while(text MATCHES "\\$[({]([A-Za-z0-9_]+)[)}]")
    set(_name "${CMAKE_MATCH_1}")
    if(DEFINED _a1v_${_name})
      set(_val "${_a1v_${_name}}")
    else()
      set(_val "")
    endif()
    string(REPLACE "$(${_name})" "${_val}" text "${text}")
    string(REPLACE "\${${_name}}" "${_val}" text "${text}")
    math(EXPR _iter "${_iter} + 1")
    if(_iter GREATER 200)
      message(FATAL_ERROR "AutomakeSources: runaway expansion of '${text}'")
    endif()
  endwhile()
  string(REGEX REPLACE "[ \t]+" " " text "${text}")
  string(STRIP "${text}" text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# _a1_am_words(<out> <text>): whitespace-separated words as a CMake list.
function(_a1_am_words out text)
  string(STRIP "${text}" text)
  if(text STREQUAL "")
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  string(REGEX REPLACE "[ \t]+" ";" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# a1_am_parse_file(<Makefile.am> <top_srcdir> <prefix>)
# Sets in the caller's scope:
#   <prefix>_VARS                 names of the variables defined (active branches)
#   <prefix>_V_<name>             each variable, fully expanded
function(a1_am_parse_file am_file top_srcdir prefix)
  if(NOT EXISTS "${am_file}")
    message(FATAL_ERROR "AutomakeSources: ${am_file} not found")
  endif()
  get_filename_component(_srcdir "${am_file}" DIRECTORY)
  set(_a1v_top_srcdir "${top_srcdir}")
  set(_a1v_srcdir "${_srcdir}")
  # Generated files (none are compiled for the 3DS) would live next to the
  # sources in an in-tree automake build.
  set(_a1v_top_builddir "${top_srcdir}")
  set(_a1v_builddir "${_srcdir}")

  file(READ "${am_file}" _content)
  # Protect characters that are special in CMake lists before splitting.
  string(REPLACE "\r" "" _content "${_content}")
  string(REPLACE ";" "<A1_SEMI>" _content "${_content}")
  string(REPLACE "[" "<A1_LB>" _content "${_content}")
  string(REPLACE "]" "<A1_RB>" _content "${_content}")
  # Join continuation lines.
  string(REGEX REPLACE "\\\\\n" " " _content "${_content}")
  string(REPLACE "\n" ";" _lines "${_content}")

  set(_stack "")      # one entry per open "if": 1 = branch active, 0 = inactive
  set(_names "")
  set(_lineno 0)
  foreach(_line IN LISTS _lines)
    math(EXPR _lineno "${_lineno} + 1")
    # Recipe lines of make rules start with a tab.
    if(_line MATCHES "^\t")
      continue()
    endif()
    # Strip comments ("#" not preceded by a backslash).
    string(REGEX REPLACE "(^|[^\\\\])#.*$" "\\1" _line "${_line}")
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
      continue()
    endif()

    if(_line MATCHES "^if[ \t]+(!?)([A-Za-z0-9_]+)$")
      set(_neg "${CMAKE_MATCH_1}")
      set(_cond "${CMAKE_MATCH_2}")
      if(NOT DEFINED A1_AM_COND_${_cond})
        message(FATAL_ERROR "AutomakeSources: ${am_file}: conditional '${_cond}' "
                            "has no value; set A1_AM_COND_${_cond} to ON or OFF")
      endif()
      if(A1_AM_COND_${_cond})
        set(_val 1)
      else()
        set(_val 0)
      endif()
      if(_neg STREQUAL "!")
        math(EXPR _val "1 - ${_val}")
      endif()
      list(APPEND _stack ${_val})
      continue()
    elseif(_line MATCHES "^else([ \t].*)?$")
      list(LENGTH _stack _depth)
      if(_depth EQUAL 0)
        message(FATAL_ERROR "AutomakeSources: ${am_file}: 'else' without 'if'")
      endif()
      list(POP_BACK _stack _top)
      math(EXPR _top "1 - ${_top}")
      list(APPEND _stack ${_top})
      continue()
    elseif(_line MATCHES "^endif([ \t].*)?$")
      list(LENGTH _stack _depth)
      if(_depth EQUAL 0)
        message(FATAL_ERROR "AutomakeSources: ${am_file}: 'endif' without 'if'")
      endif()
      list(POP_BACK _stack)
      continue()
    endif()

    if("0" IN_LIST _stack)
      continue()      # inside an inactive branch
    endif()

    if(_line MATCHES "^([A-Za-z0-9_]+)[ \t]*(\\+?=)[ \t]*(.*)$")
      set(_name "${CMAKE_MATCH_1}")
      set(_op "${CMAKE_MATCH_2}")
      set(_value "${CMAKE_MATCH_3}")
      if(_op STREQUAL "+=" AND DEFINED _a1v_${_name})
        set(_a1v_${_name} "${_a1v_${_name}} ${_value}")
      else()
        set(_a1v_${_name} "${_value}")
      endif()
      list(APPEND _names "${_name}")
    elseif(_line MATCHES "^[^=]*:([^=]|$)")
      # A make rule ("target: prerequisites"); its recipe is skipped above.
    elseif(_line MATCHES "=")
      message(FATAL_ERROR "AutomakeSources: ${am_file}:${_lineno}: unsupported "
                          "assignment syntax: ${_line}")
    else()
      message(FATAL_ERROR "AutomakeSources: ${am_file}:${_lineno}: cannot parse: ${_line}")
    endif()
  endforeach()

  list(LENGTH _stack _depth)
  if(NOT _depth EQUAL 0)
    message(FATAL_ERROR "AutomakeSources: ${am_file}: unterminated 'if'")
  endif()

  list(REMOVE_DUPLICATES _names)
  set(${prefix}_VARS "${_names}" PARENT_SCOPE)
  foreach(_name IN LISTS _names)
    _a1_am_expand(_exp "${_a1v_${_name}}")
    string(REPLACE "<A1_SEMI>" "\\;" _exp "${_exp}")
    string(REPLACE "<A1_LB>" "[" _exp "${_exp}")
    string(REPLACE "<A1_RB>" "]" _exp "${_exp}")
    set(${prefix}_V_${_name} "${_exp}" PARENT_SCOPE)
  endforeach()
endfunction()

# _a1_am_canon(<out> <name>): automake's canonical name (libfoo.a -> libfoo_a).
function(_a1_am_canon out name)
  string(REGEX REPLACE "[^A-Za-z0-9_@]" "_" name "${name}")
  set(${out} "${name}" PARENT_SCOPE)
endfunction()

# a1_automake_collect(TOP_SRCDIR <dir> START <relative dir> [PROGRAMS <name>...]
#                     PREFIX <prefix>)
#
# Walks START/Makefile.am and its SUBDIRS recursively. Every noinst_LIBRARIES
# entry and every bin_PROGRAMS entry named in PROGRAMS becomes a "unit".
# Sets in the caller's scope:
#   <prefix>_UNITS          unit names: libraries without "lib"/".a"
#                           (libcseries.a -> cseries), programs as named
#   <prefix>_MAKEFILES      every Makefile.am that was read
#   <prefix>_DIRS           every directory walked (relative to TOP_SRCDIR)
#   <prefix>_<unit>_KIND    LIBRARY or PROGRAM
#   <prefix>_<unit>_NAME    automake name (libcseries.a / alephone)
#   <prefix>_<unit>_DIR     absolute source directory
#   <prefix>_<unit>_MAKEFILE the Makefile.am that defines it
#   <prefix>_<unit>_SOURCES absolute paths of the compiled sources
#   <prefix>_<unit>_INCLUDES absolute -I directories from AM_CPPFLAGS (in order)
#   <prefix>_<unit>_DEFINES  -D definitions from AM_CPPFLAGS
#   <prefix>_<unit>_OPTIONS  any other AM_CPPFLAGS words
function(a1_automake_collect)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "TOP_SRCDIR;START;PREFIX" "PROGRAMS")
  if(NOT ARG_TOP_SRCDIR OR NOT ARG_START OR NOT ARG_PREFIX)
    message(FATAL_ERROR "a1_automake_collect: TOP_SRCDIR, START and PREFIX are required")
  endif()
  set(P "${ARG_PREFIX}")
  set(_units "")
  set(_makefiles "")
  set(_dirs "")
  set(_todo "${ARG_START}")
  set(_wanted_programs "${ARG_PROGRAMS}")
  set(_found_programs "")

  while(_todo)
    list(POP_FRONT _todo _rel)
    set(_dir "${ARG_TOP_SRCDIR}/${_rel}")
    set(_am "${_dir}/Makefile.am")
    a1_am_parse_file("${_am}" "${ARG_TOP_SRCDIR}" _am)
    list(APPEND _makefiles "${_am}")
    list(APPEND _dirs "${_rel}")

    set(_cppflags "")
    if(DEFINED _am_V_AM_CPPFLAGS)
      _a1_am_words(_cppflags "${_am_V_AM_CPPFLAGS}")
    endif()
    set(_inc "")
    set(_def "")
    set(_opt "")
    foreach(_w IN LISTS _cppflags)
      if(_w MATCHES "^-I(.+)$")
        get_filename_component(_p "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${_dir}")
        list(APPEND _inc "${_p}")
      elseif(_w MATCHES "^-D(.+)$")
        list(APPEND _def "${CMAKE_MATCH_1}")
      else()
        list(APPEND _opt "${_w}")
      endif()
    endforeach()

    # Collect units: libraries, then the requested programs.
    set(_targets "")
    if(DEFINED _am_V_noinst_LIBRARIES)
      _a1_am_words(_libs "${_am_V_noinst_LIBRARIES}")
      foreach(_l IN LISTS _libs)
        list(APPEND _targets "LIBRARY|${_l}")
      endforeach()
    endif()
    if(DEFINED _am_V_bin_PROGRAMS)
      _a1_am_words(_progs "${_am_V_bin_PROGRAMS}")
      foreach(_p IN LISTS _progs)
        if(_p IN_LIST _wanted_programs)
          list(APPEND _targets "PROGRAM|${_p}")
          list(APPEND _found_programs "${_p}")
        endif()
      endforeach()
    endif()

    foreach(_t IN LISTS _targets)
      string(REPLACE "|" ";" _t "${_t}")
      list(GET _t 0 _kind)
      list(GET _t 1 _name)
      _a1_am_canon(_canon "${_name}")
      foreach(_suffix CPPFLAGS CFLAGS CXXFLAGS)
        if(DEFINED _am_V_${_canon}_${_suffix})
          message(FATAL_ERROR "AutomakeSources: ${_am}: per-target ${_canon}_${_suffix} "
                              "is not supported by this parser")
        endif()
      endforeach()
      if(_kind STREQUAL "LIBRARY")
        if(NOT _name MATCHES "^lib(.+)\\.a$")
          message(FATAL_ERROR "AutomakeSources: ${_am}: unexpected library name ${_name}")
        endif()
        set(_unit "${CMAKE_MATCH_1}")
      else()
        set(_unit "${_name}")
      endif()
      if(_unit IN_LIST _units)
        message(FATAL_ERROR "AutomakeSources: duplicate unit '${_unit}' in ${_am}")
      endif()

      set(_srcs "")
      if(DEFINED _am_V_${_canon}_SOURCES)
        _a1_am_words(_words "${_am_V_${_canon}_SOURCES}")
        foreach(_s IN LISTS _words)
          get_filename_component(_ext "${_s}" LAST_EXT)
          string(SUBSTRING "${_ext}" 1 -1 _ext)
          if(_ext IN_LIST _A1_AM_COMPILED_EXT)
            get_filename_component(_abs "${_s}" ABSOLUTE BASE_DIR "${_dir}")
            if(NOT EXISTS "${_abs}")
              message(FATAL_ERROR "AutomakeSources: ${_am}: source ${_s} does not exist")
            endif()
            list(APPEND _srcs "${_abs}")
          elseif(NOT _ext IN_LIST _A1_AM_IGNORED_EXT)
            message(FATAL_ERROR "AutomakeSources: ${_am}: don't know how to build "
                                "'${_s}' (extension '${_ext}')")
          endif()
        endforeach()
      endif()

      list(APPEND _units "${_unit}")
      set(${P}_${_unit}_KIND "${_kind}" PARENT_SCOPE)
      set(${P}_${_unit}_NAME "${_name}" PARENT_SCOPE)
      set(${P}_${_unit}_DIR "${_dir}" PARENT_SCOPE)
      set(${P}_${_unit}_MAKEFILE "${_am}" PARENT_SCOPE)
      set(${P}_${_unit}_SOURCES "${_srcs}" PARENT_SCOPE)
      set(${P}_${_unit}_INCLUDES "${_inc}" PARENT_SCOPE)
      set(${P}_${_unit}_DEFINES "${_def}" PARENT_SCOPE)
      set(${P}_${_unit}_OPTIONS "${_opt}" PARENT_SCOPE)
    endforeach()

    # Recurse into SUBDIRS ("." means this directory, already handled).
    if(DEFINED _am_V_SUBDIRS)
      _a1_am_words(_subs "${_am_V_SUBDIRS}")
      set(_new "")
      foreach(_s IN LISTS _subs)
        if(NOT _s STREQUAL ".")
          if(_rel STREQUAL ".")
            list(APPEND _new "${_s}")
          else()
            list(APPEND _new "${_rel}/${_s}")
          endif()
        endif()
      endforeach()
      # Depth-first, in SUBDIRS order.
      list(PREPEND _todo ${_new})
    endif()

    # Clear this file's variables before the next one.
    foreach(_v IN LISTS _am_VARS)
      unset(_am_V_${_v})
    endforeach()
    unset(_am_VARS)
  endwhile()

  foreach(_p IN LISTS _wanted_programs)
    if(NOT _p IN_LIST _found_programs)
      message(FATAL_ERROR "AutomakeSources: program '${_p}' is not in bin_PROGRAMS "
                          "for these conditionals")
    endif()
  endforeach()

  set(${P}_UNITS "${_units}" PARENT_SCOPE)
  set(${P}_MAKEFILES "${_makefiles}" PARENT_SCOPE)
  set(${P}_DIRS "${_dirs}" PARENT_SCOPE)
endfunction()

# a1_automake_write_list(PREFIX <prefix> BASE <dir> OUTPUT <file>)
# Writes every unit's sources, relative to BASE, one per line and sorted, to
# OUTPUT (only when the content changes). Used for scripts/3ds/check-sources.sh.
function(a1_automake_write_list)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "PREFIX;BASE;OUTPUT" "")
  set(_all "")
  foreach(_u IN LISTS ${ARG_PREFIX}_UNITS)
    foreach(_s IN LISTS ${ARG_PREFIX}_${_u}_SOURCES)
      file(RELATIVE_PATH _r "${ARG_BASE}" "${_s}")
      list(APPEND _all "${_r}")
    endforeach()
  endforeach()
  list(SORT _all)
  list(JOIN _all "\n" _text)
  set(_text "${_text}\n")
  set(_old "")
  if(EXISTS "${ARG_OUTPUT}")
    file(READ "${ARG_OUTPUT}" _old)
  endif()
  if(NOT _old STREQUAL _text)
    file(WRITE "${ARG_OUTPUT}" "${_text}")
  endif()
endfunction()
