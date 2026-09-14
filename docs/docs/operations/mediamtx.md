---
sidebar_position: 1
title: MediaMTX sources
---

# MediaMTX sources

C-Bridge reads from [MediaMTX](https://github.com/bluenviron/mediamtx) using WHEP,
the WebRTC-HTTP Egress Protocol.

## Server configuration

The WebRTC server must be enabled. The relevant part of `mediamtx.yml`:

```yaml
webrtc: true
webrtcAddress: :8889
webrtcLocalUDPAddress: :8189
webrtcIPsFromInterfaces: true

paths:
  cam1:
    source: publisher
```

Each path is reachable at `http://<host>:8889/<path>/whep`. On a LAN no STUN or
TURN server is needed; any that MediaMTX advertises in its `Link` headers are
picked up automatically.

## Publishing a test source

Any protocol MediaMTX accepts will do. RTSP is the simplest:

```powershell
ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=60 `
       -f lavfi -i sine=frequency=1000 `
       -c:v libx264 -preset veryfast -tune zerolatency -g 120 `
       -c:a libopus -b:a 128k `
       -f rtsp rtsp://localhost:8554/cam1
```

MediaMTX then serves the same path over WHEP without re-encoding.

:::tip Keyframe interval
`-g 120` gives a keyframe every two seconds at 60 fps. C-Bridge cannot open a sink
until it has seen a keyframe, so a very long GOP delays startup by exactly that
interval. MediaMTX does not generate a keyframe on connect; C-Bridge sends a PLI
when the track opens, but the publisher has to honour it.
:::

## Authentication

C-Bridge supports HTTP Basic. Configure a reader user in `mediamtx.yml`:

```yaml
authInternalUsers:
  - user: reader
    pass: secret
    permissions:
      - action: read
```

Put the user name in the stream's `username` field. See
[Streams](/configuration/streams) for how the password is stored.

## Supported codecs

MediaMTX can offer AV1, VP9, VP8, H.265 and H.264 for video, and Opus, G.722,
G.711 and LPCM for audio. C-Bridge negotiates **H.264 or H.265 video and Opus
audio** only. Ensure your publisher produces one of those; a source published as
VP9 will connect and then fail with no supported codec.
