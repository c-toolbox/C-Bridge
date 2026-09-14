# C-Bridge

Multi-stream WebRTC gateway for Windows. Pulls streams from MediaMTX over WHEP and
fans them out to pluggable sinks:

- **MPEG-TS over UDP multicast** — compressed passthrough, no decode or encode
- **NDI** — via NVDEC decode and an avfilter GPU conversion (in progress)

## Prerequisites

| Dependency | Where |
| --- | --- |
| Qt 6.6+ and KDE Frameworks 6 | KDE Craft, e.g. `D:/Craft/VS2022_qt6X` |
| FFmpeg 8.1 | e.g. `D:/FFmpeg/mas2026/local64` (headers in `include/`, libs and DLLs in `bin-video/`) |
| libdatachannel | vcpkg, **with the `srtp` feature** |
| NDI 6 SDK | `NDI_SDK_DIR` environment variable (optional) |

### libdatachannel must be built with media support

The default vcpkg build disables WebRTC media transport, which silently removes every
RTP depacketizer:

```powershell
vcpkg install "libdatachannel[core,ws,srtp]:x64-windows" --recurse
```

Without the `srtp` feature the package exports `RTC_ENABLE_MEDIA=0` and the build
fails with "`H264RtpDepacketizer` is not a member of `rtc`".

vcpkg is used in **classic mode** on purpose — there is no `vcpkg.json`, so the
already-installed `x64-windows` tree is reused instead of being rebuilt per project.

## Building

Paths are set in `CMakePresets.json`; override `CMAKE_PREFIX_PATH` and
`CBRIDGE_FFMPEG_ROOT` there if your layout differs.

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

The executable lands in `build/bin/<config>/C-Bridge.exe`. FFmpeg DLLs are copied
next to it automatically; Qt and KF6 come from the Craft `bin` directory, which must
be on `PATH` at runtime.

## Installing (Visual Studio)

The solution includes CMake's `INSTALL` target in the default build set, so a regular
**Build** also stages a copy of the app under `../install/bin`. To stage all runtime
dependencies as well — Qt/KF6 plugins and QML modules from Craft, FFmpeg DLLs,
libdatachannel/vcpkg DLLs, NDI — configure with:

```powershell
cmake --preset windows-msvc -DCBRIDGE_INSTALL_DEPENDENCIES=ON
```

Individual steps can be toggled with `CBRIDGE_INSTALL_PLUGINS_FROM_CRAFT`,
`CBRIDGE_INSTALL_QML_FROM_CRAFT`, `CBRIDGE_INSTALL_GET_RUNTIME_DEPENDENCIES` and
`CBRIDGE_INSTALL_DLLS_FROM_PATH_LIST`. On Windows 11+ the default is to copy all DLLs
from those paths; on Windows 10 it falls back to resolving the direct dependencies of
`C-Bridge.exe`.

From the command line, `cmake --build` only builds the app targets; run the install
step explicitly:

```powershell
cmake --build build --target INSTALL --config RelWithDebInfo
```

## Running

```powershell
C-Bridge.exe --config data\configs\localtest.cbridge.json --autostart
```

Configurations are JSON documents listing streams and their sinks. Application
preferences (last config, auto-load) use `QSettings`.

Receive a multicast sink with mpv — the low-latency flags matter, since an untuned
receiver buffers enough to erase the latency advantage:

```powershell
mpv udp://239.1.1.1:5000 --profile=low-latency --cache=no --demuxer-lavf-o=fflags=+nobuffer
```

## Layout

```
cmake/      FindFFmpeg.cmake (pinned tree), FindNDI.cmake
src/
  config/   JSON configuration documents and validation
  core/     engine, per-stream pipeline, bounded media queue
  media/    bitstream inspection, decode (planned)
  sinks/    StreamSink interface, MPEG-TS multicast sink
  webrtc/   WHEP signaling, Link header parsing, libdatachannel source
  models/   QML list models
data/       runtime configs and logs
```
