# The embedded 86Box's platform layer, added by 86Box itself
# (EMBEDDED_HOST_DIR): the plat and ui targets its 86Box target links.
# 86Box's own platform-neutral helpers come from its tree.

set(_box86_src "${86Box_SOURCE_DIR}/src")

add_library(plat OBJECT
  host_plat.c
  host_paths.c
  host_joystick.c
  host_debug.c
  "${_box86_src}/unix/dummy_floppy_ioctl.c"
  "${_box86_src}/unix/dummy_cdrom_ioctl.c")

if(WIN32)
  target_sources(plat PRIVATE
    host_plat_win.c
    "${_box86_src}/qt/win_netsocket.c"
    "${_box86_src}/qt/win_dynld.c")
  target_link_libraries(86Box ws2_32)
else()
  target_sources(plat PRIVATE
    host_plat_unix.c
    "${_box86_src}/unix/unix_netsocket.c"
    "${_box86_src}/unix/unix_dynld.c")
  target_link_libraries(86Box ${CMAKE_DL_LIBS})
endif()

if(NOT CPPTHREADS)
  message(FATAL_ERROR "The embedded host expects 86Box's CPPTHREADS")
endif()
find_package(Threads REQUIRED)
target_link_libraries(86Box Threads::Threads)

find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(plat PRIVATE SDL3::Headers)

add_library(ui OBJECT
  host_ui.c
  host_audio.c
  "${_box86_src}/unix/sdl_cdrom.c")
