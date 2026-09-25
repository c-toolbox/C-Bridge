# Stages the runtime dependencies that windeployqt does not know about next to C-Bridge.exe
# so the build-tree executable runs without any special environment. Two things are missing:
#
#   1. KDE QML modules beyond org.kde.kirigami. Kirigami loads further modules (e.g.
#      org.kde.desktop for its toolbar integration) dynamically at runtime, which
#      windeployqt's import scanner does not follow; without them the root QML component
#      fails with 'module "org.kde.desktop" is not installed'. We therefore deploy the
#      whole org/kde tree from the Qt installation.
#   2. The C++ DLLs those KDE QML plugins and the executable itself need at runtime
#      (KDE Frameworks/Kirigami plus third-party libraries such as harfbuzz, zstd, ...).
#      None of them are Qt modules, so windeployqt never deploys them either.
#
# Step 2 is a breadth-first walk over PE import tables using dumpbin: the seeds are the
# executable plus every deployed plugin/QML DLL; each import resolves in the order
# System32 (system-provided, stop walking) -> EXE_DIR (already deployed, keep walking)
# -> QT_BIN_DIR / VCPKG_BIN_DIR (stage it and keep walking). api-ms-*/ext-ms-* API sets
# are skipped. Resolving against EXE_DIR first also avoids the seed-directory conflicts
# that make file(GET_RUNTIME_DEPENDENCIES) fragile for this layout.
#
# Invoked from a POST_BUILD command (after windeployqt) with:
#   -DDEPLOY_EXE=<path to C-Bridge.exe>  -DEXE_DIR=<its directory>
#   -DQT_BIN_DIR=<Qt bin dir>           [-DQT_QML_DIR=<Qt qml dir>]
#   [-DVCPKG_BIN_DIR=<vcpkg triplet bin>]  [-DDUMPBIN_EXE=<dumpbin path>]

# The Visual Studio generator wraps the -D values in literal double quotes when it writes
# the post-build command line into the vcxproj; strip them so paths work with any generator.
foreach(_v IN ITEMS DEPLOY_EXE EXE_DIR QT_BIN_DIR QT_QML_DIR VCPKG_BIN_DIR DUMPBIN_EXE)
  if(DEFINED ${_v} AND ${_v} MATCHES "^\"(.*)\"$")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" _stripped "${${_v}}")
    set(${_v} "${_stripped}")
  endif()
endforeach()

foreach(_required DEPLOY_EXE EXE_DIR QT_BIN_DIR)
  if(NOT DEFINED ${_required} OR NOT ${_required})
    message(FATAL_ERROR "stage_build_tree_deps: missing -D${_required}")
  endif()
endforeach()

# --- 1. KDE QML modules --------------------------------------------------------
if(DEFINED QT_QML_DIR AND EXISTS "${QT_QML_DIR}/org/kde")
  file(COPY "${QT_QML_DIR}/org/kde" DESTINATION "${EXE_DIR}/qml/org")
else()
  message(WARNING "stage_build_tree_deps: no org/kde QML modules found (QT_QML_DIR='${QT_QML_DIR}'); the build-tree executable will fail to load its UI.")
endif()

# --- 2. PE import walk ---------------------------------------------------------
if(NOT DUMPBIN_EXE OR NOT EXISTS "${DUMPBIN_EXE}")
  # Locate dumpbin without relying on a developer PATH: glob the MSVC tool layouts.
  file(GLOB _msvc_bin_dirs
    "C:/Program Files/Microsoft Visual Studio/*/Community/VC/Tools/MSVC/*/bin/HostX64/x64"
    "C:/Program Files/Microsoft Visual Studio/*/Professional/VC/Tools/MSVC/*/bin/HostX64/x64"
    "C:/Program Files/Microsoft Visual Studio/*/Enterprise/VC/Tools/MSVC/*/bin/HostX64/x64"
    "C:/Program Files/Microsoft Visual Studio/*/BuildTools/VC/Tools/MSVC/*/bin/HostX64/x64")
  find_program(DUMPBIN_EXE NAMES dumpbin.exe HINTS ${_msvc_bin_dirs})
endif()

if(NOT DUMPBIN_EXE OR NOT EXISTS "${DUMPBIN_EXE}")
  message(WARNING "stage_build_tree_deps: dumpbin not found; skipping the PE import walk. The build-tree executable may be missing KDE Frameworks/Kirigami runtime DLLs.")
else()
  set(_sys32 "$ENV{SystemRoot}/System32")

  # Seeds: the exe plus every deployed plugin/QML DLL (all of them are loaded at runtime).
  set(_queue "${DEPLOY_EXE}")
  file(GLOB_RECURSE _deployed_dlls "${EXE_DIR}/plugins/*.dll" "${EXE_DIR}/qml/*.dll")
  list(APPEND _queue ${_deployed_dlls})

  set(_visited "")
  set(_staged_names "")   # DLL names scheduled for staging (dedup)
  set(_stage_sources "")  # their source paths, in the same order
  set(_missing "")        # "name <- importer" pairs found nowhere

  while(_queue)
    list(GET _queue 0 _cur)
    list(REMOVE_AT _queue 0)
    list(FIND _visited "${_cur}" _seen)
    if(_seen GREATER_EQUAL 0)
      continue()
    endif()
    list(APPEND _visited "${_cur}")

    execute_process(
      COMMAND "${DUMPBIN_EXE}" /DEPENDENTS "${_cur}"
      OUTPUT_VARIABLE _dumpbin_out
      RESULT_VARIABLE _dumpbin_rc
      ERROR_QUIET
    )
    if(NOT _dumpbin_rc EQUAL 0)
      message(WARNING "stage_build_tree_deps: dumpbin failed on ${_cur}")
      continue()
    endif()

    # Extract imported DLL names (dumpbin prints one "    name.dll" line per import).
    # Note: this CMake build does not populate the output variable of string(REGEX MATCH/MATCHALL),
    # so filter line by line with if(MATCHES) + REGEX REPLACE instead.
    set(_imports "")
    string(REPLACE "\r\n" "\n" _dumpbin_out "${_dumpbin_out}")
    string(REPLACE "\n" ";" _dep_lines "${_dumpbin_out}")
    foreach(_line IN LISTS _dep_lines)
      if(_line MATCHES "^[ \t]*[A-Za-z0-9._-]+\\.dll$")
        string(REGEX REPLACE "^[ \t]*([A-Za-z0-9._-]+\\.dll)[ \t]*$" "\\1" _imp_name "${_line}")
        list(APPEND _imports "${_imp_name}")
      endif()
    endforeach()

    foreach(_imp IN LISTS _imports)
      if(_imp MATCHES "^(api-ms|ext-ms)-")
        continue()  # Windows API sets, provided by the system
      endif()
      if(EXISTS "${_sys32}/${_imp}")
        continue()  # system-provided; stop walking this branch
      endif()

      set(_local "${EXE_DIR}/${_imp}")
      if(EXISTS "${_local}")
        list(FIND _visited "${_local}" _lseen)
        if(_lseen LESS 0)
          list(APPEND _queue "${_local}")
        endif()
        continue()
      endif()

      set(_found "")
      foreach(_src IN LISTS QT_BIN_DIR VCPKG_BIN_DIR)
        if(_src AND EXISTS "${_src}/${_imp}")
          set(_found "${_src}/${_imp}")
          break()
        endif()
      endforeach()
      if(NOT _found)
        list(FIND _missing "${_imp}" _mseen)
        if(_mseen LESS 0)
          list(APPEND _missing "${_imp} <- ${_cur}")
        endif()
        continue()
      endif()

      list(FIND _staged_names "${_imp}" _sseen)
      if(_sseen LESS 0)
        list(APPEND _staged_names "${_imp}")
        list(APPEND _stage_sources "${_found}")
      endif()
      list(FIND _visited "${_found}" _fseen)
      if(_fseen LESS 0)
        list(APPEND _queue "${_found}")
      endif()
    endforeach()
  endwhile()

  if(_stage_sources)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_stage_sources} "${EXE_DIR}"
      RESULT_VARIABLE _copy_rc
    )
    if(NOT _copy_rc EQUAL 0)
      message(WARNING "stage_build_tree_deps: failed to stage runtime DLLs next to C-Bridge.exe")
    else()
      list(LENGTH _staged_names _staged_count)
      message(STATUS "stage_build_tree_deps: staged ${_staged_count} KDE Frameworks/Kirigami/third-party DLL(s) next to C-Bridge.exe")
    endif()
  else()
    message(STATUS "stage_build_tree_deps: all runtime dependencies already in place")
  endif()

  if(_missing)
    list(LENGTH _missing _missing_count)
    message(WARNING "stage_build_tree_deps: ${_missing_count} imported DLL(s) not found in System32, the build tree, Qt bin or vcpkg bin:")
    foreach(_m IN LISTS _missing)
      message(WARNING "  ${_m}")
    endforeach()
  endif()
endif()

