---
sidebar_position: 3
title: Sinks
---

# Sinks

A stream may have any number of sinks, and they run concurrently. A stream with
only multicast sinks never creates a decoder at all.

## MPEG-TS multicast

```json
{
  "kind": "ts-multicast",
  "enabled": true,
  "tsMulticast": {
    "groupAddress": "239.1.1.1",
    "port": 5000,
    "ttl": 8,
    "localAddress": "10.0.0.5",
    "packetSize": 1316,
    "patPeriodMs": 100,
    "pcrPeriodMs": 20
  }
}
```

| Field | Notes |
| --- | --- |
| `groupAddress` | Must be in `224.0.0.0/4`. Use the administratively scoped `239.0.0.0/8` range. |
| `port` | Unique per group address. |
| `ttl` | 1 stays on the local segment; raise it to cross routers. |
| `localAddress` | Interface to transmit from. **Set this on a multi-NIC host**, or Windows may choose the wrong adapter. |
| `packetSize` | 1316 is seven 188-byte TS packets and fits a 1500-byte MTU. Raising it above the path MTU causes fragmentation. |
| `patPeriodMs` | How often the PAT/PMT tables repeat. Lower means a receiver joining mid-stream locks on sooner. |
| `pcrPeriodMs` | Clock reference interval. |

The sink waits for the first keyframe before writing the TS header, because the
muxer needs the parameter sets that ride in-band with an IDR.

## NDI

```json
{
  "kind": "ndi",
  "enabled": true,
  "ndi": {
    "senderName": "Camera 1",
    "groups": "",
    "targetWidth": 0,
    "targetHeight": 0,
    "fpsNum": 0,
    "fpsDen": 1,
    "audioEnabled": true
  }
}
```

| Field | Notes |
| --- | --- |
| `senderName` | Must be unique across the machine. Defaults to the stream name. |
| `groups` | Optional NDI group list; empty means the default group. |
| `targetWidth` / `targetHeight` | `0` keeps the source resolution. Scaling happens on the GPU. |
| `fpsNum` | `0` keeps the source rate. Any other value inserts an `fps` filter. |
| `audioEnabled` | Whether to publish the decoded Opus audio. |

Frames are sent as UYVY, which halves the bus and encode cost compared with BGRA.
Alpha is not carried; NDI output is opaque.

:::note Decoding is per sink
The NDI sink owns its own decoder. Two NDI sinks on one stream would decode that
stream twice, so prefer one NDI sink per source.
:::
