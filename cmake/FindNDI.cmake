# Locates the NDI SDK via the NDI_SDK_DIR environment variable.
#
# C-Bridge links only against the headers and loads Processing.NDI.Lib.x64.dll at
# runtime (see src/ndi/ndiruntime.h), so the import library is optional: a machine
# with the SDK headers but no runtime still builds, and reports the missing runtime
# in the UI instead of failing to start.
#
# Imported target:
#   NDI::NDI          headers, plus the import library when one is present
#
# Result variables:
#   NDI_FOUND, NDI_INCLUDE_DIR, NDI_LIBRARY, NDI_RUNTIME_DIR

include(FindPackageHandleStandardArgs)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(_ndi_arch "x86")
else()
  set(_ndi_arch "x64")
endif()

set(_ndi_root "")
if(DEFINED ENV{NDI_SDK_DIR})
  file(TO_CMAKE_PATH "$ENV{NDI_SDK_DIR}" _ndi_root)
endif()

find_path(NDI_INCLUDE_DIR
  NAMES Processing.NDI.Lib.h
  PATHS "${_ndi_root}/Include"
  NO_DEFAULT_PATH
)

find_library(NDI_LIBRARY
  NAMES "Processing.NDI.Lib.${_ndi_arch}"
  PATHS "${_ndi_root}/Lib/${_ndi_arch}"
  NO_DEFAULT_PATH
)

if(EXISTS "${_ndi_root}/Bin/${_ndi_arch}")
  set(NDI_RUNTIME_DIR "${_ndi_root}/Bin/${_ndi_arch}")
endif()

find_package_handle_standard_args(NDI
  REQUIRED_VARS NDI_INCLUDE_DIR
)

if(NDI_FOUND AND NOT TARGET NDI::NDI)
  if(NDI_LIBRARY)
    add_library(NDI::NDI UNKNOWN IMPORTED)
    set_target_properties(NDI::NDI PROPERTIES IMPORTED_LOCATION "${NDI_LIBRARY}")
  else()
    add_library(NDI::NDI INTERFACE IMPORTED)
  endif()
  set_property(TARGET NDI::NDI APPEND
               PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${NDI_INCLUDE_DIR}")
endif()

mark_as_advanced(NDI_INCLUDE_DIR NDI_LIBRARY)
