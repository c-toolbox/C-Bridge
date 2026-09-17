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

## RTP multicast

```json
{
  "kind": "rtp-multicast",
  "enabled": true,
  "rtpMulticast": {
    "groupAddress": "239.1.1.2",
    "port": 5004,
    "ttl": 8,
    "localAddress": "",
    "packetSize": 1316
  }
}
```

| Field | Notes |
| --- | --- |
| `groupAddress` | Must be in `224.0.0.0/4`. Use the administratively scoped `239.0.0.0/8` range. |
| `port` | Video RTP port (UDP). Audio is sent to `port + 1`, so keep both free. |
| `ttl` | 1 stays on the local segment; raise it to cross routers. |
| `localAddress` | Interface to transmit from. **Set this on a multi-NIC host**, or Windows may choose the wrong adapter. |
| `packetSize` | Maximum RTP datagram size in bytes. 1316 fits a standard MTU without fragmentation. |

Unlike MPEG-TS multicast, video and audio go out as two independent raw RTP
streams with no container: H.264/HEVC NAL units on the video port (90 kHz clock)
and Opus packets on the audio port (48 kHz clock). Receivers join the UDP ports
directly; there is no SDP or signaling to negotiate.

- ffplay, video only: `ffplay -f h264 udp://239.1.1.2:5004`
- GStreamer: `udpsrc address=239.1.1.2 port=5004 caps="application/x-rtp,media=video" ! rtpjitterbuffer ! avdec_h264 ! videoconvert ! autovideosink`

The parameter sets (SPS/PPS) are re-sent with every keyframe, so a receiver that
joins mid-stream locks on at the next IDR. Each stream carries its own SSRC and
timestamp base; RTCP sender reports ride alongside the media so receivers can
align the two streams.

## RTSP unicast

```json
{
  "kind": "rtsp-unicast",
  "enabled": true,
  "rtsp": {
    "port": 8554,
    "path": "stream",
    "localAddress": ""
  }
}
```

| Field | Notes |
| --- | --- |
| `port` | TCP port for RTSP signaling. Media goes to a UDP port the player picks during SETUP. |
| `path` | The URL path, e.g. `stream` in `rtsp://host:8554/stream`. Defaults to `stream`. |
| `localAddress` | Interface to bind RTSP signaling on and send media from. Empty listens on all interfaces and lets the OS choose; **set this on a multi-NIC host**. |

Players open `rtsp://<bridge-host>:<port>/<path>` with unicast UDP transport:

- VLC: *Media → Open Network Stream*, select UDP as the transport.
- ffplay: `ffplay -rtsp_transport udp rtsp://host:8554/stream`

Each player gets its own MPEG-TS over a dedicated UDP socket, so this works across
networks where multicast does not reach. A late joiner is held until the next
keyframe and then starts on a clean GOP; when it disconnects its resources are
released immediately. Two enabled RTSP sinks may not share the same `:port/path`.

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
