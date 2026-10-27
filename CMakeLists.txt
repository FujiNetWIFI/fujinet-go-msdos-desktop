cmake_minimum_required(VERSION 3.20)

# Before project(): otherwise the bundle's minimum macOS is the CI SDK's.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# The only place the version lives (About boxes, bundle, release checks).
project(fujinet-go-msdos-desktop VERSION 0.4.2 LANGUAGES C CXX)

add_compile_definitions(MSDOS_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# At the top so every subdirectory sees CMAKE_INSTALL_DATADIR (a cache var
# that is empty when GNUInstallDirs is first included below the top).
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/Box86Core.cmake)

enable_testing()

# Before the core (the boot floppy takes FujiNet's DOS programs from the
# firmware tree) and the frontends (the macOS bundle depends on it).
include(cmake/FujiNetRuntime.cmake)

add_subdirectory(core)

function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# Application icons (rendered from source art by tools/icons/make-icons.py).
# Desktop environments look the icon up by the desktop-entry id, so the same
# art is installed under each frontend's id, at every hicolor size. Only for
# frontends built here: a package should not ship files for an app it does
# not contain.
set(MSDOS_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(MSDOS_ICON_TARGETS_AND_IDS
  fujinet-go-msdos-gnome online.fujinet.go.msdos.gnome
  fujinet-go-msdos-kde online.fujinet.go.msdos.kde)
list(LENGTH MSDOS_ICON_TARGETS_AND_IDS _msdos_icon_n)
math(EXPR _msdos_icon_last "${_msdos_icon_n} - 1")
foreach(_i RANGE 0 ${_msdos_icon_last} 2)
  list(GET MSDOS_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET MSDOS_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS MSDOS_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-msdos.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()

# No BIOS, video, disk-controller or MIDI ROM ships: every frontend binary
# is checked against the images a developer keeps under tools/roms (any
# layout; 86Box's roms tree works). SKIPs (77) when there are none to probe
# with, as in every CI and release checkout.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  foreach(_t fujinet-go-msdos-gnome fujinet-go-msdos-kde
             fujinet-go-msdos-windows fujinet-go-msdos-macos)
    if(TARGET ${_t})
      add_test(NAME no_embedded_roms_${_t}
               COMMAND "${Python3_EXECUTABLE}"
                       "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                       "$<TARGET_FILE:${_t}>"
                       "${CMAKE_SOURCE_DIR}/tools/roms")
      set_tests_properties(no_embedded_roms_${_t} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
  endforeach()
endif()

# ---- packaging --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/libadwaita)
# and KDE (Qt6) pull in entirely different runtime library stacks, and a user
# who wants one should not be made to depend on the other.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-msdos-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-msdos-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-msdos")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "IBM PCs from the 5150 to the Pentium with a built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs Qt6
  # alone makes one fixed list wrong for whichever frontend was not built.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  # libfujinet sits in a private directory the binary finds by itself
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
      "${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-msdos")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

include(CPack)
