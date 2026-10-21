# The toolkit-agnostic core: 86Box on its own thread (PcHost), the session
# around it, and their tests and tools.

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---- 86Box on its own thread ------------------------------------------------
add_library(msdos_pchost STATIC 86box/PcHost.cpp)
target_include_directories(msdos_pchost PUBLIC 86box 86box/host)
target_link_libraries(msdos_pchost PUBLIC 86Box msdos_86box_headers SDL3::SDL3 Threads::Threads)
if(WIN32)
  target_link_libraries(msdos_pchost PUBLIC winmm ws2_32)
endif()

# ---- the x86 disassembler ------------------------------------------------------
msdos_provide_dependency(
  NAME zydis
  PATH third_party/zydis
  ARCHIVE "${ZYDIS_URL}"
  SHA256 "${ZYDIS_SHA256}"
  STRIP amalgamated-dist
  SENTINEL Zydis.h
  OVERRIDE ZYDIS_SRC
  RESULT ZYDIS_DIR)
add_library(msdos_zydis STATIC "${ZYDIS_DIR}/Zydis.c")
target_include_directories(msdos_zydis PUBLIC "${ZYDIS_DIR}")
target_compile_definitions(msdos_zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
set_target_properties(msdos_zydis PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(msdos_zydis PRIVATE -w)
endif()

# ---- the FujiNet boot floppies, embedded -----------------------------------------
set(MSDOS_BOOTDISK_C "${CMAKE_BINARY_DIR}/generated/bootdisk_images.c")
set(_autorun "")
set(_autorun_args "")
if(FUJINET_DIR AND EXISTS "${FUJINET_DIR}/data/webui/device_specific/BUILD_RS232/autorun.img")
  set(_autorun "${FUJINET_DIR}/data/webui/device_specific/BUILD_RS232/autorun.img")
  set(_autorun_args --autorun "${_autorun}")
endif()
add_custom_command(
  OUTPUT "${MSDOS_BOOTDISK_C}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          --freedos "${CMAKE_SOURCE_DIR}/tools/disks/freedos"
          --fujinet-sys "${CMAKE_SOURCE_DIR}/tools/disks/fujinet-msdos/FUJINET.SYS"
          ${_autorun_args}
          --out-c "${MSDOS_BOOTDISK_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          "${CMAKE_SOURCE_DIR}/tools/disks/fat12.py"
          "${CMAKE_SOURCE_DIR}/tools/disks/freedos/KERNEL.SYS"
          "${CMAKE_SOURCE_DIR}/tools/disks/freedos/COMMAND.COM"
          "${CMAKE_SOURCE_DIR}/tools/disks/freedos/bootsect.bin"
          "${CMAKE_SOURCE_DIR}/tools/disks/fujinet-msdos/FUJINET.SYS"
          ${_autorun}
  COMMENT "FujiNet boot floppies"
  VERBATIM)

# ---- the Help, embedded ---------------------------------------------------------------
set(MSDOS_HELP_C "${CMAKE_BINARY_DIR}/generated/help_content.c")
add_custom_command(
  OUTPUT "${MSDOS_HELP_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          --src "${CMAKE_SOURCE_DIR}/data/help/help.md"
          --out-c "${MSDOS_HELP_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          "${CMAKE_SOURCE_DIR}/data/help/help.md"
  COMMENT "Help"
  VERBATIM)

# ---- the session ------------------------------------------------------------------
add_library(msdos_session STATIC
  src/audio_sdl.c
  src/bootdisk.c
  "${MSDOS_BOOTDISK_C}"
  src/cfg86.c
  src/debug.cpp
  src/debug86.c
  src/fujinet_api.c
  src/fujinet_runtime.c
  src/gamepad_sdl.c
  src/help.c
  "${MSDOS_HELP_C}"
  src/hid_keys.c
  src/input.cpp
  src/machines.c
  src/media.cpp
  src/paths.c
  src/prefs.c
  src/rom_table.c
  src/roms.c
  src/screen_text.c
  src/session.cpp
  src/settings.c
  src/zipread.c)
target_include_directories(msdos_session PUBLIC include PRIVATE src)
target_link_libraries(msdos_session PUBLIC msdos_pchost msdos_zydis ${CMAKE_DL_LIBS})
find_package(ZLIB REQUIRED)
target_link_libraries(msdos_session PRIVATE ZLIB::ZLIB)

# where an uninstalled run finds the FujiNet runtime, and an installed one
target_compile_definitions(msdos_session PRIVATE
  MSDOS_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  MSDOS_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-msdos"
  MSDOS_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-msdos")

# ---- tools --------------------------------------------------------------------------
add_executable(msdos_headless tools/msdos_headless.cpp src/screen_text.c)
target_link_libraries(msdos_headless PRIVATE msdos_pchost ${CMAKE_DL_LIBS})

add_executable(msdos_session_run tools/msdos_session_run.c)
target_link_libraries(msdos_session_run PRIVATE msdos_session)
add_subdirectory(tests)
