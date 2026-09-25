cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

# The vcpkg toolchain has to be selected before project() runs.
option(BUILD_WITH_VCPKG_SUPPORT "Use the vcpkg toolchain from $ENV{VCPKG_ROOT}" ON)
if(BUILD_WITH_VCPKG_SUPPORT AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  else()
    message(WARNING "BUILD_WITH_VCPKG_SUPPORT is ON but VCPKG_ROOT is not set.")
  endif()
endif()

project(c-bridge
  VERSION 0.2.0
  DESCRIPTION "Multi-stream WebRTC gateway: MPEG-TS multicast and NDI output"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# Group the Qt/ECM generated targets in the Visual Studio solution.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(AUTO_TARGET_NAME "Generated Targets")
set(AUTO_SOURCE_NAME "Generated Files")
set_property(GLOBAL PROPERTY AUTOMOC_TARGETS_FOLDER ${AUTO_TARGET_NAME})
set_property(GLOBAL PROPERTY AUTORCC_TARGETS_FOLDER ${AUTO_TARGET_NAME})
set_property(GLOBAL PROPERTY AUTOGEN_TARGETS_FOLDER ${AUTO_TARGET_NAME})
set_property(GLOBAL PROPERTY AUTOGEN_SOURCE_GROUP ${AUTO_SOURCE_NAME})
set_property(GLOBAL PROPERTY AUTOMOC_SOURCE_GROUP ${AUTO_SOURCE_NAME})
# Qt assigns its own internal targets (aotstats, qmllint) to this folder; set it
# explicitly since we do not call qt_standard_project_setup().
set_property(GLOBAL PROPERTY QT_TARGETS_FOLDER "QtInternalTargets")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Debug builds must still resolve Release-only imported configs from vcpkg/Craft.
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL MinSizeRel RelWithDebInfo Release Debug "")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release MinSizeRel Debug "")
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE Release RelWithDebInfo MinSizeRel Debug "")

if(NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  # keep whatever the user asked for
else()
  get_filename_component(_cbridge_parent "${CMAKE_SOURCE_DIR}" DIRECTORY)
  set(CMAKE_INSTALL_PREFIX "${_cbridge_parent}/install" CACHE PATH "" FORCE)
endif()

set(QT_MIN_VERSION 6.6.0)
set(KF6_MIN_VERSION 6.0.0)

# Machine-specific locations come from the environment so no paths need to be
# committed: point CBRIDGE_CRAFT_ROOT at a Craft install providing Qt/KF6. An explicit
# -DCMAKE_PREFIX_PATH=... always takes precedence.
if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{CBRIDGE_CRAFT_ROOT})
  file(TO_CMAKE_PATH "$ENV{CBRIDGE_CRAFT_ROOT}" _cbridge_craft_root)
  set(CMAKE_PREFIX_PATH "${_cbridge_craft_root}")
endif()

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
  Core
  Gui
  Network
  Qml
  Quick
  QuickControls2
)

find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS
  ColorScheme
  Config
  CoreAddons
  I18n
  IconThemes
  Kirigami
  WindowSystem
)

# Same as C-Slice: report whether the Breeze icon theme is available (RECOMMENDED).
find_package(Breeze)
set_package_properties(Breeze PROPERTIES TYPE RECOMMENDED
    URL "https://api.kde.org/frameworks/breeze-icons/html/index.html")

find_package(FFmpeg REQUIRED COMPONENTS
  AVCODEC
  AVFILTER
  AVFORMAT
  AVUTIL
  SWRESAMPLE
  SWSCALE
)
message(STATUS "FFmpeg ${FFMPEG_VERSION_STRING} from ${CBRIDGE_FFMPEG_ROOT}")

find_package(LibDataChannel CONFIG REQUIRED)
set_package_properties(LibDataChannel PROPERTIES
  TYPE REQUIRED
  URL "https://github.com/paullouisageneau/libdatachannel"
  DESCRIPTION "WebRTC transport used to pull WHEP streams"
)

option(BUILD_CBRIDGE_WITH_NDI "Build the NDI output sink" ON)
if(BUILD_CBRIDGE_WITH_NDI)
  find_package(NDI)
  set_package_properties(NDI PROPERTIES
    TYPE OPTIONAL
    URL "https://ndi.video/for-developers/ndi-sdk/download/"
    DESCRIPTION "NDI SDK headers - the runtime DLL is loaded dynamically"
  )
  if(NOT NDI_FOUND)
    message(STATUS "NDI SDK not found (set NDI_SDK_DIR); building without the NDI sink.")
    set(BUILD_CBRIDGE_WITH_NDI OFF)
  endif()
endif()

option(CBRIDGE_INSTALL_DEPENDENCIES "Global On/Off for all CBRIDGE_INSTALL_* dependencies" OFF)

option(CBRIDGE_INSTALL_BUILD_BY_DEFAULT "Include INSTALL during default build" ON)
if(CBRIDGE_INSTALL_BUILD_BY_DEFAULT)
  set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
else()
  set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 0)
endif()

add_subdirectory(src)
add_subdirectory(data)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
