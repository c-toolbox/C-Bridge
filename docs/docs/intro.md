---
slug: /
sidebar_position: 1
title: Introduction
---

# C-Bridge

C-Bridge is a Windows gateway that pulls live streams from [MediaMTX](https://github.com/bluenviron/mediamtx)
over WebRTC and republishes them to one or more outputs. It is built for density:
a single instance is intended to carry on the order of two dozen 1080p60 streams.

## Outputs

C-Bridge fans each source out to any number of **sinks**, and the two available
sinks have very different cost profiles.

| | MPEG-TS over UDP multicast | NDI |
| --- | --- | --- |
| Processing | passthrough, no decode or encode | NVDEC decode, GPU convert, SpeedHQ encode |
| Bandwidth (1080p60) | source bitrate, typically 4–10 Mbps | roughly 125 Mbps |
| GPU | none | one decode session per stream |
| CPU | negligible | around one core per stream |
| Quality | bit-exact | re-encoded, visually lossless |
| Consumers | mpv, ffmpeg, VLC, hardware decoders | vMix, TriCaster, OBS, Studio Monitor |

The multicast sink is essentially free because the H.264/H.265 bitstream arriving
from WebRTC is written straight into an MPEG-TS muxer. Nothing is decoded.

:::tip Choosing a sink
Use multicast wherever the consumer can read MPEG-TS, and reserve NDI for the
handful of streams that must reach production tools. Twenty-four NDI outputs is
roughly 3 Gbps, which does not fit a 10GbE link alongside anything else; the same
twenty-four as multicast is around 150–240 Mbps.
:::

## How a stream flows

```
WHEP signaling (HTTP)  ->  libdatachannel peer connection
                               |
      Annex-B access units + Opus packets (RTP timestamps preserved)
                               |
        +----------------------+------------------------+
        |                                               |
  MPEG-TS muxer                              NVDEC decode -> avfilter
  udp://239.x.x.x:port                        -> UYVY -> NDI sender
```

Video needs no timestamp conversion on the multicast path: the RTP video clock is
90 kHz, which is exactly the MPEG-TS timebase.

## Where to go next

- [Install](/install) — prerequisites and first run
- [Configuration](/configuration/overview) — the JSON document format
- [MediaMTX sources](/operations/mediamtx) — preparing the upstream server
- [Multicast networking](/networking/multicast) — switch and address planning
- [Build from source](/build/overview)
