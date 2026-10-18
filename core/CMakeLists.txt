# The toolkit-agnostic core: 86Box on its own thread (PcHost), the session
# around it, and their tests and tools.

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)

add_library(msdos_pchost STATIC 86box/PcHost.cpp)
target_include_directories(msdos_pchost PUBLIC 86box 86box/host)
target_link_libraries(msdos_pchost PUBLIC 86Box msdos_86box_headers SDL3::SDL3 Threads::Threads)
if(WIN32)
  target_link_libraries(msdos_pchost PUBLIC winmm)
endif()

add_executable(msdos_headless tools/msdos_headless.cpp src/screen_text.c)
target_link_libraries(msdos_headless PRIVATE msdos_pchost ${CMAKE_DL_LIBS})
