---
sidebar_position: 2
title: Install
---

# Install

C-Bridge is currently Windows-only and ships as a folder you can copy.

## Requirements

- Windows 10 or 11, x64
- An NVIDIA GPU for the NDI sink (NVDEC). The multicast sink needs no GPU at all,
  since it never decodes.
- A network that permits IPv4 multicast if you use the multicast sink

## Runtime dependencies

Everything except the NDI runtime is deployed next to `C-Bridge.exe`:

- Qt 6 and KDE Frameworks 6 libraries
- FFmpeg 8.1 (`avcodec-62.dll`, `avformat-62.dll`, `avfilter-11.dll`, …)
- `datachannel.dll` and its OpenSSL dependencies

### NDI runtime

The NDI library is **not** bundled. C-Bridge loads
`Processing.NDI.Lib.x64.dll` at runtime and searches, in order:

1. the directory named by `NDILIB_REDIST_FOLDER`
2. `NDI_RUNTIME_DIR_V6`, then `NDI_RUNTIME_DIR_V5`
3. `%NDI_SDK_DIR%\Bin\x64` and `%NDI_SDK_DIR%\Redist`
4. the standard DLL search path

If it cannot be found, C-Bridge still starts and every multicast sink keeps
working; only NDI sinks report an error. Install the redistributable from
[ndi.video](https://ndi.video/) to enable them.

## First run

```powershell
C-Bridge.exe --config data\configs\localtest.cbridge.json
```

| Option | Effect |
| --- | --- |
| `--config <path>` | Load this configuration document at startup |
| `--autostart` | Start every enabled stream once the config is loaded |

With no `--config`, C-Bridge reloads the configuration it last opened. The startup
banner in `data/log/cbridge.log` records the resolved FFmpeg, libdatachannel and
NDI versions, which is the quickest way to confirm a deployment picked up the
libraries you expected.
