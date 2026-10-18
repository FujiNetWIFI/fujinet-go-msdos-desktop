# 86Box as a static library (the fork's EMBEDDED_HOST mode). Our platform
# layer, core/86box/host, provides the plat and ui targets 86Box links.
#
# The fork carries every change we need, so the tree is used in place: no
# staging, no patches. Override with -DBOX86_SRC=<checkout> while working on
# the fork itself.

msdos_provide_dependency(
  NAME 86Box
  PATH third_party/86box
  URL "${BOX86_URL}"
  COMMIT "${BOX86_COMMIT}"
  SENTINEL src/include/86box/86box.h
  OVERRIDE BOX86_SRC
  RESULT BOX86_SOURCE_DIR)

# 86Box's own options, fixed for the embedded build. FORCE: a user's cache
# must not turn on a backend whose host side we do not provide.
function(_msdos_86box_option name value)
  set(${name} ${value} CACHE BOOL "Fixed by fujinet-go-msdos-desktop" FORCE)
endfunction()

_msdos_86box_option(EMBEDDED_HOST ON)
_msdos_86box_option(QT OFF)
_msdos_86box_option(OPENAL OFF)
_msdos_86box_option(RTMIDI OFF)
_msdos_86box_option(FLUIDSYNTH OFF)
_msdos_86box_option(MUNT ON)
_msdos_86box_option(SOUNDCANVAS OFF)
_msdos_86box_option(DISCORD OFF)
_msdos_86box_option(VNC OFF)
_msdos_86box_option(MINITRACE OFF)
_msdos_86box_option(GDBSTUB ON)
_msdos_86box_option(AUTOMATION OFF)
_msdos_86box_option(DEV_BRANCH OFF)
_msdos_86box_option(DYNAREC ON)
_msdos_86box_option(NEW_DYNAREC ON)
_msdos_86box_option(CPPTHREADS ON)
_msdos_86box_option(LIBASAN OFF)
_msdos_86box_option(BUILD_TESTING_86BOX OFF)
set(EMBEDDED_HOST_DIR "${CMAKE_SOURCE_DIR}/core/86box/host" CACHE PATH
    "Fixed by fujinet-go-msdos-desktop" FORCE)
if(NOT DEFINED STATIC_BUILD)
  set(STATIC_BUILD OFF CACHE BOOL "86Box static build" FORCE)
endif()

# 86Box's tests key on BUILD_TESTING, which CTest would otherwise turn on.
set(_msdos_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)

# 86Box is optimised whatever our own build type: at -O0 an XT barely
# reaches real time and a Pentium crawls. Compile options come after the
# per-config flags, so -O2 beats Debug's -O0; the subdirectory takes a copy,
# and ours is put back below.
get_directory_property(_msdos_saved_compile_options COMPILE_OPTIONS)
add_compile_options($<$<CONFIG:Debug>:-O2>)

# Its warnings are its own business.
set(_msdos_saved_c_flags "${CMAKE_C_FLAGS}")
set(_msdos_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
string(APPEND CMAKE_C_FLAGS " -w")
string(APPEND CMAKE_CXX_FLAGS " -w")

add_subdirectory("${BOX86_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/86box" EXCLUDE_FROM_ALL)

set(CMAKE_C_FLAGS "${_msdos_saved_c_flags}")
set(CMAKE_CXX_FLAGS "${_msdos_saved_cxx_flags}")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_msdos_saved_compile_options}")
set(BUILD_TESTING "${_msdos_saved_build_testing}")

# 86Box's include directories are directory-scoped and do not travel with the
# target; the host layer and our own code reach its headers through this.
add_library(msdos_86box_headers INTERFACE)
target_include_directories(msdos_86box_headers INTERFACE
  "${BOX86_SOURCE_DIR}/src/include"
  "${BOX86_SOURCE_DIR}/src/cpu"
  "${BOX86_SOURCE_DIR}/src/codegen"
  "${CMAKE_BINARY_DIR}/86box/src/include")
