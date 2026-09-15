set(TARGET_NAME C-Bridge)

configure_file(cbridgeversion.h.in "${CMAKE_CURRENT_BINARY_DIR}/cbridgeversion.h" @ONLY)

qt_add_executable(${TARGET_NAME}
  main.cpp
  bridgeapplication.cpp
  bridgeapplication.h
  bridgecontroller.cpp
  bridgecontroller.h

  models/streamlistmodel.cpp
  models/streamlistmodel.h
  models/sinklistmodel.cpp
  models/sinklistmodel.h
  models/streamdraft.cpp
  models/streamdraft.h

  core/bridgetypes.cpp
  core/bridgetypes.h
  core/bridgeengine.cpp
  core/bridgeengine.h
  core/mediaqueue.h
  core/streampipeline.cpp
  core/streampipeline.h

  media/bitstreaminspector.cpp
  media/bitstreaminspector.h
  media/avwrappers.h
  media/cudacontext.cpp
  media/cudacontext.h
  media/videodecoder.cpp
  media/videodecoder.h
  media/videofilter.cpp
  media/videofilter.h
  media/audiodecoder.cpp
  media/audiodecoder.h

  ndi/ndiruntime.cpp
  ndi/ndiruntime.h

  sinks/streamsink.h
  sinks/tsmulticastsink.cpp
  sinks/tsmulticastsink.h
  sinks/ndisink.cpp
  sinks/ndisink.h

  config/bridgeconfig.cpp
  config/bridgeconfig.h

  webrtc/linkheaderparser.cpp
  webrtc/linkheaderparser.h
  webrtc/whepclient.cpp
  webrtc/whepclient.h
  webrtc/webrtcsource.cpp
  webrtc/webrtcsource.h

  # Application icons, same as C-Slice: the qrc embeds the window icon into the
  # exe (auto-registered at startup by Qt 6), and icon.rc sets the Windows file
  # / taskbar icon.
  ../data/images/images.qrc
  ../data/images/icon.rc
)

# Same Qt policy choices as C-Slice (see its src/CMakeLists.txt): use the modern
# QML module resource prefix and auto-generate qmldir files for extra QML dirs.
qt_policy(SET QTP0001 NEW)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8.0)
    qt_policy(SET QTP0004 NEW)
endif()

# QML files sit at the module root so they register under their bare type name.
qt_add_qml_module(${TARGET_NAME}
  URI org.ctoolbox.cbridge
  VERSION 1.0
  QML_FILES
    Main.qml
    StreamEditorPage.qml
    SinkCard.qml
    TsSinkForm.qml
    NdiSinkForm.qml
)

# Same as C-Slice: with a shared Qt build the imported QML plugins are loaded from
# disk at runtime, so skip the import scan (it would warn about plugin link targets
# that do not exist in scope); with a static Qt build they must be linked instead.
get_target_property(_qt_core_target_type Qt6::Core TYPE)
if(_qt_core_target_type STREQUAL "STATIC_LIBRARY")
    qt_import_qml_plugins(${TARGET_NAME})
else()
    set_target_properties(${TARGET_NAME} PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()

target_include_directories(${TARGET_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
)

# Craft ships a different FFmpeg (8.0.1) whose headers would otherwise win over the
# pinned tree we link against, silently mismatching headers and import libs.
target_include_directories(${TARGET_NAME} BEFORE PRIVATE "${FFMPEG_INCLUDE_DIRS}")

target_link_libraries(${TARGET_NAME} PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Network
  Qt6::Qml
  Qt6::Quick
  Qt6::QuickControls2

  KF6::ColorScheme
  KF6::ConfigCore
  KF6::ConfigGui
  KF6::CoreAddons
  KF6::I18n
  KF6::IconThemes
  KF6::Kirigami
  KF6::WindowSystem

  FFmpeg::AVCODEC
  FFmpeg::AVFILTER
  FFmpeg::AVFORMAT
  FFmpeg::AVUTIL
  FFmpeg::SWRESAMPLE
  FFmpeg::SWSCALE

  LibDataChannel::LibDataChannel
)

if(BUILD_CBRIDGE_WITH_NDI)
  target_link_libraries(${TARGET_NAME} PRIVATE NDI::NDI)
  target_compile_definitions(${TARGET_NAME} PRIVATE CBRIDGE_NDI_SUPPORT)
endif()

if(MSVC)
  target_compile_options(${TARGET_NAME} PRIVATE
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /MP
    /wd4244  # conversion, possible loss of data (pervasive in the FFmpeg headers)
    /wd4267
    /experimental:external /external:anglebrackets /external:W0
  )
  target_compile_definitions(${TARGET_NAME} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
  )

  # Group the Qt-generated helper targets under "Generated Targets" in Visual Studio.
  foreach(_generated_target
      all_qmltyperegistrations
      ${TARGET_NAME}_automoc_json_extraction
      ${TARGET_NAME}_copy_qml
      ${TARGET_NAME}_copy_res
      ${TARGET_NAME}_qmltyperegistration)
    if(TARGET ${_generated_target})
      set_target_properties(${_generated_target} PROPERTIES FOLDER ${AUTO_TARGET_NAME})
    endif()
  endforeach()
  if(TARGET fetch-translations)
    set_target_properties(fetch-translations PROPERTIES FOLDER "External")
  endif()
endif()

# Third-party runtimes are not in PATH, so stage them next to the executable.
# Globbed rather than listed: this FFmpeg tree ships transitive deps (openh264, ICU)
# whose names change between builds.
if(WIN32 AND FFMPEG_RUNTIME_DIR)
  file(GLOB CBRIDGE_FFMPEG_DLLS "${FFMPEG_RUNTIME_DIR}/*.dll")
  if(CBRIDGE_FFMPEG_DLLS)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CBRIDGE_FFMPEG_DLLS} "$<TARGET_FILE_DIR:${TARGET_NAME}>"
      COMMAND_EXPAND_LISTS
      VERBATIM
    )
  endif()
endif()

install(TARGETS ${TARGET_NAME} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(WIN32 AND CBRIDGE_INSTALL_DEPENDENCIES)
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
    get_filename_component(_qt_dir "${_qt_bin_dir}" DIRECTORY)

    # vcpkg classic mode: the libdatachannel runtime DLLs live in the installed
    # triplet's bin directory, not next to the import libraries.
    set(_vcpkg_bin_dir "")
    if(BUILD_WITH_VCPKG_SUPPORT AND DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg_root)
        if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
            set(_vcpkg_bin_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        else()
            set(_vcpkg_bin_dir "${_vcpkg_root}/installed/x64-windows/bin")
        endif()
    endif()

    install(CODE "set(QT_BIN_DIR \"${_qt_bin_dir}\")")

    option(CBRIDGE_INSTALL_PLUGINS_FROM_CRAFT "Copy complete plugins folder for Qt/KDE" ON)
    if(CBRIDGE_INSTALL_PLUGINS_FROM_CRAFT)
        install(CODE [[
            message("Copying Qt/KDE plugins folder")
        ]])
        install(DIRECTORY "${_qt_dir}/plugins" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" FILES_MATCHING PATTERN "*.dll")
    endif()

    option(CBRIDGE_INSTALL_QML_FROM_CRAFT "Copy complete qml folder for Qt/KDE" ON)
    if(CBRIDGE_INSTALL_QML_FROM_CRAFT)
        install(CODE [[
            message("Copying KDE qml folder")
        ]])
        install(DIRECTORY "${_qt_dir}/qml" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            FILES_MATCHING PATTERN "*.dll")
        install(DIRECTORY "${_qt_dir}/qml" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            FILES_MATCHING PATTERN "*.qmltypes")
        install(DIRECTORY "${_qt_dir}/qml" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            FILES_MATCHING PATTERN "qmldir")
        install(DIRECTORY "${_qt_dir}/qml/org" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/qml")
    endif()

    if(BUILD_CBRIDGE_WITH_NDI AND NDI_RUNTIME_DIR)
        install(CODE [[
            message("Copying NDI runtime DLLs")
        ]])
        # Trailing slash: copy the directory contents, not the x64/ folder itself.
        install(DIRECTORY "${NDI_RUNTIME_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" FILES_MATCHING PATTERN "*.dll")
    endif()

    set_property(TARGET ${TARGET_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${TARGET_NAME}>)
    if(_vcpkg_bin_dir)
        set_property(TARGET ${TARGET_NAME} PROPERTY VS_DEBUGGER_ENVIRONMENT "PATH=${FFMPEG_RUNTIME_DIR};${_vcpkg_bin_dir};%PATH%")
    else()
        set_property(TARGET ${TARGET_NAME} PROPERTY VS_DEBUGGER_ENVIRONMENT "PATH=${FFMPEG_RUNTIME_DIR};%PATH%")
    endif()

    # cmake_host_system_information(QUERY OS_VERSION) reports "Windows 10" for both
    # Windows 10 and 11, so query the registry for the actual build number (22000 is
    # the first Windows 11 build) to pick a dependency-copy strategy.
    set(OS_BUILD_NUM "")
    execute_process(
        COMMAND powershell -NoProfile -NonInteractive -Command
                "(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').CurrentBuildNumber"
        OUTPUT_VARIABLE OS_BUILD_NUM
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(OS_BUILD_NUM MATCHES "^[0-9]+$" AND OS_BUILD_NUM VERSION_GREATER_EQUAL 22000)
        message("Windows 11 or above detected (build ${OS_BUILD_NUM}), recommended to copy all DLLS from path instead of using GET_RUNTIME_DEPENDENCIES")
        set(INSTALL_GRP FALSE)
        set(INSTALL_DFP TRUE)
    else()
        message("Windows 10 or below detected, recommended to use GET_RUNTIME_DEPENDENCIES instead of copying all DLLS from path")
        set(INSTALL_GRP TRUE)
        set(INSTALL_DFP FALSE)
    endif()

    option(CBRIDGE_INSTALL_GET_RUNTIME_DEPENDENCIES "When running INSTALL, look for dependencies with GET_RUNTIME_DEPENDENCIES and copy them" ${INSTALL_GRP})
    if(CBRIDGE_INSTALL_GET_RUNTIME_DEPENDENCIES)
        set(MY_DEPENDENCY_PATHS "${_qt_bin_dir}" "${FFMPEG_RUNTIME_DIR}")
        if(_vcpkg_bin_dir)
            list(APPEND MY_DEPENDENCY_PATHS "${_vcpkg_bin_dir}")
        endif()
        # Transfer the values into the install script
        install(CODE "set(MY_DEPENDENCY_PATHS \"${MY_DEPENDENCY_PATHS}\")")
        install(CODE "set(target \"$<TARGET_FILE:${TARGET_NAME}>\")")
        # Install dependencies
        install(CODE [[
            message("Looking for direct dependencies for C-Bridge.exe (In Qt, FFmpeg and vcpkg directories)")
            file(GET_RUNTIME_DEPENDENCIES
                EXECUTABLES ${target}
                PRE_EXCLUDE_REGEXES api-ms msvcp vcruntime kernel32
                POST_EXCLUDE_REGEXES "(?i)C:[/\\]Windows[/\\]"
                CONFLICTING_DEPENDENCIES_PREFIX CONFLICTING_DEPENDENCIES
                RESOLVED_DEPENDENCIES_VAR RESOLVED_DEPENDENCIES
                UNRESOLVED_DEPENDENCIES_VAR UNRESOLVED_DEPENDENCIES
                DIRECTORIES ${MY_DEPENDENCY_PATHS}
            )
            foreach(DEPENDENCY ${CONFLICTING_DEPENDENCIES})
                message(WARNING "Conflicting dependency ${DEPENDENCY}")
            endforeach()
            message("Copying detected dependencies")
            foreach(DEPENDENCY ${RESOLVED_DEPENDENCIES})
                message("Copying ${DEPENDENCY}")
                file(INSTALL
                    DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
                    FILES "${DEPENDENCY}"
                )
            endforeach()
        ]])
    endif()

    option(CBRIDGE_INSTALL_DLLS_FROM_PATHS "When running INSTALL, copy all DLLS from CBRIDGE_INSTALL_DLLS_FROM_PATH_LIST to the INSTALL/bin folder" ${INSTALL_DFP})
    if(CBRIDGE_INSTALL_DLLS_FROM_PATHS)
        set(CIDFPL "${_qt_bin_dir}" "${FFMPEG_RUNTIME_DIR}")
        if(_vcpkg_bin_dir)
            list(APPEND CIDFPL "${_vcpkg_bin_dir}")
        endif()

        set(CBRIDGE_INSTALL_DLLS_FROM_PATH_LIST ${CIDFPL} CACHE STRING "The paths where to look for DLL:s")

        # Install dependencies
        install(CODE "set(_all_dll_copy_paths \"${CBRIDGE_INSTALL_DLLS_FROM_PATH_LIST}\")")
        install(CODE [[
                message("Copying all DLL:s from CBRIDGE_INSTALL_DLLS_FROM_PATH_LIST to install folder")
                foreach(DLL_DIR ${_all_dll_copy_paths})
                    message("Looking for DLL:s in ${DLL_DIR}")
                    if(EXISTS "${DLL_DIR}")
                        file(GLOB DLL_FILES "${DLL_DIR}/*.dll")
                        foreach(DLL_FILE ${DLL_FILES})
                            message("Copying ${DLL_FILE}")
                            file(INSTALL
                                DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
                                FILES "${DLL_FILE}"
                            )
                        endforeach()
                    else()
                        message("Directory ${DLL_DIR} does not exist")
                    endif()
                endforeach()
            ]])
    endif()

endif()
