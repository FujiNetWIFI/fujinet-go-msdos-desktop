cmake_minimum_required(VERSION 3.20)

# Before project(): otherwise the bundle's minimum macOS is the CI SDK's.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# The only place the version lives (About boxes, bundle, release checks).
project(fujinet-go-msdos-desktop VERSION 0.1.0 LANGUAGES C CXX)

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
add_subdirectory(core)

# Set up before the frontends so the macOS bundle can depend on it.
include(cmake/FujiNetRuntime.cmake)
