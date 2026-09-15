---
sidebar_position: 1
title: Overview
---

# Building C-Bridge

C-Bridge builds with CMake and MSVC on Windows.

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

The executable lands in `build/bin/<config>/C-Bridge.exe`, with the FFmpeg DLLs
copied beside it automatically.

Paths are set in `CMakePresets.json`. Override them there if your layout differs:

| Cache variable | Purpose |
| --- | --- |
| `CMAKE_PREFIX_PATH` | Where Qt 6 and KDE Frameworks 6 live (a Craft root) |
| `CBRIDGE_FFMPEG_ROOT` | Root of the FFmpeg 8.1 tree |
| `CBRIDGE_FFMPEG_LIBDIR_NAME` | Subdirectory holding the import libs and DLLs |
| `BUILD_CBRIDGE_WITH_NDI` | Set `OFF` to build without the NDI sink |

A second preset, `windows-msvc-no-ndi`, builds into a separate directory with NDI
disabled, which is useful for confirming the multicast path has no NDI dependency.

## Running from the build tree

Qt and KDE Frameworks are not copied next to the executable during a normal build,
so the Craft `bin` directory has to be on `PATH`:

```powershell
$env:PATH = "D:\Craft\VS2022_qt6X\bin;$env:PATH"
.\build\bin\RelWithDebInfo\C-Bridge.exe
```

For a self-contained folder, configure with `CBRIDGE_INSTALL_DEPENDENCIES=ON` and
build the `INSTALL` target.

See [Dependencies](/build/dependencies) for the libraries involved and
[CMake details](/build/cmake) for how each is located.
