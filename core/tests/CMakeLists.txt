# The session tests. The 5150/5160 boot on GLaBIOS (GPL, fixtures/) in
# place of IBM's BIOS, so they run in CI, which has no PC ROMs.
set(_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# A FreeDOS floppy with nothing of FujiNet's on it, for typing at A:\>.
set(_plain "${CMAKE_CURRENT_BINARY_DIR}/plain-freedos-360k.img")
add_custom_command(
  OUTPUT "${_plain}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          --freedos "${CMAKE_SOURCE_DIR}/tools/disks/freedos" --plain-out "${_plain}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          "${CMAKE_SOURCE_DIR}/tools/disks/fat12.py"
  COMMENT "Plain FreeDOS floppy for the tests")
# The same, loading FUJINET.SYS at COM2 with no CONFIG run.
set(_fnplain "${CMAKE_CURRENT_BINARY_DIR}/fujinet-plain-360k.img")
add_custom_command(
  OUTPUT "${_fnplain}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          --freedos "${CMAKE_SOURCE_DIR}/tools/disks/freedos" --plain-out "${_fnplain}"
          --fujinet-sys "${CMAKE_SOURCE_DIR}/tools/disks/fujinet-msdos/FUJINET.SYS"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/disks/make-boot-floppy.py"
          "${CMAKE_SOURCE_DIR}/tools/disks/fat12.py"
          "${CMAKE_SOURCE_DIR}/tools/disks/fujinet-msdos/FUJINET.SYS"
  COMMENT "FreeDOS floppy with FUJINET.SYS for the tests")
add_custom_target(msdos_test_floppy ALL DEPENDS "${_plain}" "${_fnplain}")

foreach(_t session roms bootdisk media profiles input debug netboot fujinet_speed help
           hd_reset hostport)
  if(_t STREQUAL "roms")
    set(_src rom_test.c)
  else()
    set(_src ${_t}_test.c)
  endif()
  add_executable(${_t}_test ${_src})
  target_link_libraries(${_t}_test PRIVATE msdos_session)
  target_compile_definitions(${_t}_test PRIVATE
    MSDOS_TEST_FIXTURES="${_fixtures}" MSDOS_TEST_PLAIN_FLOPPY="${_plain}"
    MSDOS_TEST_FUJINET_FLOPPY="${_fnplain}")
  add_dependencies(${_t}_test msdos_test_floppy)
endforeach()

add_test(NAME roms COMMAND roms_test)
add_test(NAME bootdisk COMMAND bootdisk_test)
add_test(NAME media COMMAND media_test)
add_test(NAME profiles COMMAND profiles_test)
add_test(NAME help COMMAND help_test)
add_test(NAME hostport COMMAND hostport_test)
add_test(NAME session COMMAND session_test)
add_test(NAME input COMMAND input_test)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(session input debug PROPERTIES TIMEOUT 300)
# Needs a hard disk controller ROM (MSDOS_TEST_ROMS), which CI has not.
add_test(NAME hd_reset COMMAND hd_reset_test)
set_tests_properties(hd_reset PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
foreach(_com 2 1 3 4)
  add_test(NAME netboot_com${_com} COMMAND netboot_test ${_com})
  set_tests_properties(netboot_com${_com} PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 300 RUN_SERIAL ON)
endforeach()
foreach(_m fifo nofifo)
  add_test(NAME fujinet_speed_${_m} COMMAND fujinet_speed_test ${_m})
  set_tests_properties(fujinet_speed_${_m} PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 300 RUN_SERIAL ON)
endforeach()

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM AND WITH_FUJINET)
  add_test(NAME fujinet_update COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/update_test.sh")
  set_tests_properties(fujinet_update PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS network)
endif()
