---
sidebar_position: 3
title: CMake details
---

# CMake details

## Locating FFmpeg

`cmake/FindFFmpeg.cmake` deliberately does **not** search the system. It resolves
everything under `CBRIDGE_FFMPEG_ROOT` with `NO_DEFAULT_PATH`:

```cmake
find_library(${_comp}_LIBRARY
  NAMES ${_lib}
  PATHS "${_ffmpeg_libdir}" "${_ffmpeg_root}/lib"
  NO_DEFAULT_PATH
)
```

Development machines routinely carry several FFmpeg trees. Letting CMake search
freely picks an arbitrary one, and mixing headers from one build with import
libraries from another fails at runtime rather than at link time.

The module also exposes `FFMPEG_VERSION_STRING`, read out of `ffversion.h`, which
is printed during configuration so the chosen tree is visible in the log.

## Include priority

Finding the right FFmpeg is not sufficient. Qt and KDE Frameworks propagate the
Craft include directory through their interface properties, and Craft ships its
own FFmpeg headers. Those would otherwise be searched first:

```cmake
target_include_directories(${TARGET_NAME} BEFORE PRIVATE "${FFMPEG_INCLUDE_DIRS}")
```

`BEFORE` places the pinned tree ahead of every interface include directory.

## Locating NDI

`cmake/FindNDI.cmake` requires only `NDI_INCLUDE_DIR`. The import library is
optional, because C-Bridge loads the DLL at runtime:

```cmake
find_package_handle_standard_args(NDI REQUIRED_VARS NDI_INCLUDE_DIR)
```

When no import library is present an `INTERFACE` target carrying just the headers
is created instead of an `UNKNOWN IMPORTED` one.

## Deploying runtime DLLs

The FFmpeg DLLs are globbed rather than listed by name:

```cmake
file(GLOB CBRIDGE_FFMPEG_DLLS "${FFMPEG_RUNTIME_DIR}/*.dll")
```

These trees ship transitive dependencies — openh264, ICU, libmpv — whose file
names change between builds, so an explicit list goes stale silently.

## QML module layout

QML files live at the root of the module source directory, not in a `qml/`
subdirectory. A file listed as `qml/Main.qml` is registered under that relative
path, and `loadFromModule` then reports that the module "contains no type named
Main".

## Gotcha: semicolons in CMake strings

```cmake
# Fails: the semicolon is parsed as a list separator
set_package_properties(NDI PROPERTIES DESCRIPTION "headers; loaded dynamically")
```

CMake reports "Unknown keywords given to SET_PACKAGE_PROPERTIES". Use a dash.
