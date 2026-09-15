---
sidebar_position: 2
title: Dependencies
---

# Dependencies

| Dependency | Minimum | Source |
| --- | --- | --- |
| Qt | 6.6 | KDE Craft |
| KDE Frameworks | 6.0 | KDE Craft |
| FFmpeg | 8.1 | A prebuilt tree with NVDEC enabled |
| libdatachannel | 0.23 | vcpkg, **with the `srtp` feature** |
| NDI SDK | 6 | Optional, headers only |

## libdatachannel must include media support

This is the single most common build failure.

```powershell
vcpkg install "libdatachannel[core,ws,srtp]:x64-windows" --recurse
```

:::danger
The default vcpkg build has `RTC_ENABLE_MEDIA=0` and exports that as an interface
compile definition. Every RTP depacketizer is then hidden behind a preprocessor
guard, and the build fails with `'H264RtpDepacketizer': is not a member of 'rtc'`.
The `srtp` feature is what enables WebRTC media transport.
:::

vcpkg is used in **classic mode** deliberately. There is no `vcpkg.json`, so the
existing `installed/x64-windows` tree is reused instead of each project rebuilding
its own copy of OpenSSL and libdatachannel.

## FFmpeg

The tree must provide `avcodec`, `avformat`, `avfilter`, `avutil`, `swresample`
and `swscale`, plus the `ffnvcodec` headers for NVDEC.

:::warning More than one FFmpeg on the machine
KDE Craft ships its own FFmpeg. If its include directory is searched first you
will compile against one version's headers while linking another version's import
libraries, which produces a runtime crash rather than a build error. C-Bridge
guards against this — see [CMake details](/build/cmake) — and the startup banner
prints the version actually compiled in.
:::

## NDI SDK

Only the headers are needed at build time; the library is loaded at runtime.
`FindNDI.cmake` locates the SDK through `NDI_SDK_DIR`. If it is absent, CMake
reports that and continues with `BUILD_CBRIDGE_WITH_NDI` switched off, so a
machine without the SDK still produces a working multicast-only build.
