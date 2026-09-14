# Locates a pinned FFmpeg build tree.
#
# This project deliberately does NOT search the system: several FFmpeg trees exist
# side by side on the build machines and picking the wrong one yields ABI crashes
# at runtime rather than link errors. Point CBRIDGE_FFMPEG_ROOT at a prefix that
# contains include/ and a library directory (default layout: bin-video/).
#
# Components: AVCODEC AVFORMAT AVUTIL AVFILTER AVDEVICE SWSCALE SWRESAMPLE
#
# Imported targets:
#   FFmpeg::<COMPONENT>   one per found component
#   FFmpeg::FFmpeg        aggregate of all requested components
#
# Result variables:
#   FFMPEG_FOUND, FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARIES, FFMPEG_RUNTIME_DIR,
#   FFMPEG_VERSION_STRING

include(FindPackageHandleStandardArgs)

set(CBRIDGE_FFMPEG_ROOT "D:/FFmpeg/mas2026/local64"
    CACHE PATH "Root of the FFmpeg 8.1 build tree (contains include/ and bin-video/)")

set(CBRIDGE_FFMPEG_LIBDIR_NAME "bin-video"
    CACHE STRING "Directory under CBRIDGE_FFMPEG_ROOT holding the import libs and DLLs")

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS AVCODEC AVFORMAT AVUTIL)
endif()

set(_ffmpeg_root "${CBRIDGE_FFMPEG_ROOT}")
file(TO_CMAKE_PATH "${_ffmpeg_root}" _ffmpeg_root)
set(_ffmpeg_libdir "${_ffmpeg_root}/${CBRIDGE_FFMPEG_LIBDIR_NAME}")

find_path(FFMPEG_INCLUDE_DIR
  NAMES libavcodec/avcodec.h
  PATHS "${_ffmpeg_root}/include"
  NO_DEFAULT_PATH
)

# Read the version out of the headers; there is no pkg-config on Windows here.
set(FFMPEG_VERSION_STRING "unknown")
if(FFMPEG_INCLUDE_DIR AND EXISTS "${FFMPEG_INCLUDE_DIR}/libavutil/ffversion.h")
  file(STRINGS "${FFMPEG_INCLUDE_DIR}/libavutil/ffversion.h" _ffversion_line
       REGEX "^#define[ \t]+FFMPEG_VERSION[ \t]+\"")
  if(_ffversion_line)
    string(REGEX REPLACE "^#define[ \t]+FFMPEG_VERSION[ \t]+\"([^\"]+)\".*$" "\\1"
           FFMPEG_VERSION_STRING "${_ffversion_line}")
  endif()
endif()

set(_ffmpeg_all_components AVCODEC AVFORMAT AVUTIL AVFILTER AVDEVICE SWSCALE SWRESAMPLE)

foreach(_comp IN LISTS _ffmpeg_all_components)
  string(TOLOWER "${_comp}" _lib)

  find_library(${_comp}_LIBRARY
    NAMES ${_lib}
    PATHS "${_ffmpeg_libdir}" "${_ffmpeg_root}/lib"
    NO_DEFAULT_PATH
  )
  mark_as_advanced(${_comp}_LIBRARY)

  if(${_comp}_LIBRARY AND FFMPEG_INCLUDE_DIR)
    set(FFmpeg_${_comp}_FOUND TRUE)
    set(${_comp}_FOUND TRUE)

    if(NOT TARGET FFmpeg::${_comp})
      add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${_comp} PROPERTIES
        IMPORTED_LOCATION "${${_comp}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
      )
    endif()
  else()
    set(FFmpeg_${_comp}_FOUND FALSE)
    set(${_comp}_FOUND FALSE)
  endif()
endforeach()

set(FFMPEG_LIBRARIES "")
foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
  if(${_comp}_FOUND)
    list(APPEND FFMPEG_LIBRARIES "${${_comp}_LIBRARY}")
  endif()
endforeach()

set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
if(EXISTS "${_ffmpeg_libdir}")
  set(FFMPEG_RUNTIME_DIR "${_ffmpeg_libdir}")
endif()

find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFMPEG_INCLUDE_DIR FFMPEG_LIBRARIES
  VERSION_VAR FFMPEG_VERSION_STRING
  HANDLE_COMPONENTS
)

if(FFMPEG_FOUND AND NOT TARGET FFmpeg::FFmpeg)
  add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
  foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(${_comp}_FOUND)
      set_property(TARGET FFmpeg::FFmpeg APPEND
                   PROPERTY INTERFACE_LINK_LIBRARIES FFmpeg::${_comp})
    endif()
  endforeach()
endif()

mark_as_advanced(FFMPEG_INCLUDE_DIR)
