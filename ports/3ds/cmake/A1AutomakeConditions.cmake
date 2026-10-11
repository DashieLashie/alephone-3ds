# AM_CONDITIONAL values for the 3DS build, i.e. the configure.ac outcome of
#   ./configure --disable-opengl --disable-network --without-zzip ...
# on a non-Windows, non-Steam target without Catch2. These select which
# sources Source_Files/**/Makefile.am compile (see AutomakeSources.cmake).
#
# Shared by ports/3ds/CMakeLists.txt and scripts/3ds/check-sources.sh, so the
# source check always uses the same values as the build.
include_guard(GLOBAL)

set(A1_AM_COND_MAKE_NETWORK         OFF)  # A1_NO_NETWORK: network_games.cpp + network_dummy.cpp only
set(A1_AM_COND_MAKE_OPENGL          OFF)  # no Rasterizer_Shader / RenderRasterize_Shader
set(A1_AM_COND_STEAM                OFF)  # no steamshim_child.cpp
set(A1_AM_COND_BUILD_ZZIP_RWOPS     OFF)  # no SDL_rwops_zzip.c
set(A1_AM_COND_MAKE_WINDOWS         OFF)  # POSIX thread priority, single "alephone" program
set(A1_AM_COND_BUILD_ALEPHONE_TESTS OFF)  # no alephone_tests
set(A1_AM_COND_STANDALONE_HUB       OFF)  # bin_PROGRAMS = alephone
# Conditionals defined in configure.ac but not used by Source_Files/**/Makefile.am
# (listed so a future upstream use fails loudly only if it is genuinely new).
set(A1_AM_COND_INSTALL_MIME         OFF)
