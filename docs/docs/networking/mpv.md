---
sidebar_position: 2
title: Receiving with mpv
---

# Receiving with mpv

mpv uses libavformat, so it reads C-Bridge's MPEG-TS output directly, including
Opus audio, with no transcoding.

## Command

```powershell
mpv udp://239.1.1.1:5000 --profile=low-latency --cache=no --demuxer-lavf-o=fflags=+nobuffer
```

The **Copy** button next to each stream in the UI produces exactly this line with
the right group and port filled in.

:::danger Tuning the receiver is not optional
By default mpv buffers enough to erase the benefit of the passthrough path. An
untuned MPEG-TS receiver can be *slower* end to end than NDI, which typically sits
around one frame. The latency lives in the receiver, not in the bridge.
:::

| Flag | Why |
| --- | --- |
| `--profile=low-latency` | Applies mpv's bundled low-latency preset |
| `--cache=no` | Disables the demuxer cache |
| `--demuxer-lavf-o=fflags=+nobuffer` | Stops libavformat buffering ahead |
| `--untimed` | Optional: display frames as soon as they decode, ignoring the presentation clock |

## Joining a specific interface

On a multi-homed machine, tell mpv which interface to join from:

```powershell
mpv "udp://239.1.1.1:5000?localaddr=10.0.0.20" --profile=low-latency --cache=no
```

## Inspecting the stream

To confirm what is actually on the wire rather than what you expect:

```powershell
ffprobe -hide_banner udp://239.1.1.1:5000
```

This reports the program tables, stream types and resolution. If `ffprobe` blocks
without output, the stream is not reaching the host at all and the problem is
[network-side](/networking/multicast) rather than in C-Bridge.
