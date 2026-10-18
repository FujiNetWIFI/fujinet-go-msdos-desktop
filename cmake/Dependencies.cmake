# External source dependencies, provided by the build itself (no submodules).
#
#   86Box             the FujiNetWIFI fork (feat/fujinet): upstream 86Box plus
#                     the fujinet COM device and EMBEDDED_HOST, added as a
#                     subproject by cmake/Box86Core.cmake.
#   fujinet-firmware  FujiNetWIFI master; built as the in-process runtime by
#                     cmake/FujiNetRuntime.cmake. Local builds move this pin
#                     forward automatically (tools/fujinet/update-fujinet-firmware.sh).
#   zydis             the x86 disassembler (amalgamated release) for the debugger.
#
# Each pin is recorded in three places: here and in both flatpak manifests
# (build-aux/flatpak/*.yml).
#
# Resolution order: BOX86_SRC / FUJINET_SRC / ZYDIS_SRC (cache or
# environment) -> the gitignored clone in third_party/<name> -> a fresh clone
# (or download) of the pin.

find_package(Git QUIET)

set(BOX86_COMMIT "909d7fe5cab1918fdda92cbaceb2716fbf8bd2b8")
set(BOX86_URL "https://github.com/FujiNetWIFI/86Box")

set(FUJINET_COMMIT "0a76fb9dc492ed8e3cf8d651d3af46c9844a590f")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

set(ZYDIS_VERSION "4.1.0")
set(ZYDIS_URL "https://github.com/zyantific/zydis/releases/download/v${ZYDIS_VERSION}/zydis-amalgamated.tar.gz")
set(ZYDIS_SHA256 "aa9b82be3a37a2998bd8e16cf583bbf2b6c3d80e97dc20504169dc32ca1ced59")

# msdos_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                         RESULT <out> [URL <u> COMMIT <c>]
#                         [ARCHIVE <url> SHA256 <h> STRIP <dir>])
function(msdos_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;ARCHIVE;SHA256;STRIP;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}" AND DEP_ARCHIVE)
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}.archive")
    get_filename_component(_parent "${_path}" DIRECTORY)
    set(_tmp "${_parent}/.${DEP_NAME}-extract")

    if(NOT EXISTS "${_dl}")
      message(STATUS "${DEP_NAME}: downloading ${DEP_ARCHIVE}")
      file(DOWNLOAD "${DEP_ARCHIVE}" "${_dl}"
           EXPECTED_HASH SHA256=${DEP_SHA256}
           TLS_VERIFY ON
           STATUS _dl_status)
      list(GET _dl_status 0 _dl_rc)
      if(NOT _dl_rc EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        file(REMOVE "${_dl}")
        message(FATAL_ERROR
          "Could not download ${DEP_NAME} from ${DEP_ARCHIVE}: ${_dl_msg}\n"
          "Download it by hand and unpack it, then point ${DEP_OVERRIDE} at "
          "the result.")
      endif()
    endif()

    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

    set(_extracted "${_tmp}")
    if(DEP_STRIP)
      set(_extracted "${_tmp}/${DEP_STRIP}")
    endif()
    if(NOT EXISTS "${_extracted}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_NAME} archive did not contain ${DEP_STRIP}/${DEP_SENTINEL} "
        "(${DEP_ARCHIVE}).")
    endif()

    file(REMOVE_RECURSE "${_path}")
    file(RENAME "${_extracted}" "${_path}" RESULT _mv)
    file(REMOVE_RECURSE "${_tmp}")
    if(NOT _mv EQUAL 0)
      message(FATAL_ERROR "${DEP_NAME}: could not move the unpacked archive "
                          "into ${_path}: ${_mv}")
    endif()

  elseif(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    set(_is_submodule FALSE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules")
      file(READ "${CMAKE_SOURCE_DIR}/.gitmodules" _gitmodules)
      string(FIND "${_gitmodules}" "path = ${DEP_PATH}" _found)
      if(NOT _found EQUAL -1)
        set(_is_submodule TRUE)
      endif()
    endif()

    if(_is_submodule AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    if(DEP_ARCHIVE)
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Unpack ${DEP_ARCHIVE} into "
        "${DEP_PATH}, or point ${DEP_OVERRIDE} at an existing checkout.")
    else()
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  if(DEP_COMMIT AND EXISTS "${_path}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" status --porcelain
                  --untracked-files=no
          OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET)
        if(_dirty)
          message(FATAL_ERROR
            "${DEP_NAME}: ${DEP_PATH} is at ${_head} with local changes, but "
            "cmake/Dependencies.cmake pins ${DEP_COMMIT}. Commit or stash "
            "them and re-run cmake to move to the pin, or keep working on "
            "this tree with -D${DEP_OVERRIDE}=${_path}.")
        endif()

        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                  "${DEP_COMMIT}^{commit}"
          RESULT_VARIABLE _have ERROR_QUIET)
        if(NOT _have EQUAL 0)
          message(STATUS "${DEP_NAME}: fetching pinned ${DEP_COMMIT}")
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
            RESULT_VARIABLE _rc)
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                    "${DEP_COMMIT}^{commit}"
            RESULT_VARIABLE _have ERROR_QUIET)
          if(NOT _have EQUAL 0)
            execute_process(
              COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
                      "${DEP_COMMIT}"
              RESULT_VARIABLE _have)
          endif()
          if(NOT _have EQUAL 0)
            message(FATAL_ERROR
              "${DEP_NAME}: pinned commit ${DEP_COMMIT} is not in ${DEP_PATH} "
              "and could not be fetched from origin. Fetch it by hand, or "
              "point ${DEP_OVERRIDE} at a checkout that has it.")
          endif()
        endif()

        message(STATUS "${DEP_NAME}: moving ${_head} -> pinned ${DEP_COMMIT}")
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" -c advice.detachedHead=false
                  checkout --quiet "${DEP_COMMIT}"
          RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
          message(FATAL_ERROR
            "${DEP_NAME}: could not check out ${DEP_COMMIT} in ${DEP_PATH}.")
        endif()
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
