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

## Adding streams from C-Bridge

C-Bridge can browse the paths of one or more MediaMTX servers and add them to the
current configuration without typing URLs by hand. The control API must be enabled:

```yaml
api: yes
# apiAddress: :9997   # default
```

Open **MediaMTX streams…** from the toolbar:

1. Manage the server list (host, API endpoint, WebRTC endpoint). The list is stored in
   `data/mediamtx-servers.json` next to the executable; passwords are kept for the session
   only and never written to that file.
2. **Test Connection** checks reachability and credentials against `/v3/paths/list`.
3. **Fetch Streams** lists every active path — with source type, codecs and reader count —
   plus configured paths that are currently idle (marked *configured, not active*).

With auto-detection enabled the WebRTC port and scheme are read from the server's own
configuration (`webrtcAddress` / `webrtcEncryption`) after each fetch, so a non-default
WebRTC address is picked up without editing the entry.

Selecting a path and pressing **Add to Configuration** creates a stream entry with the WHEP
URL `<scheme>://<host>:<port>/<path>/whep` and a default MPEG-TS multicast sink, ready for
editing like any other stream. A path that is already part of the configuration is not added
twice.

### Credentials in the URL

By default only the server's user name is stored on the new stream entry (see
[Streams](/configuration/streams) — passwords never appear in the document). The usable server
password is carried over to the new entry for this session, and afterwards it can be entered or
changed on the stream's edit page (**Password** field, session only); when the stream starts,
C-Bridge falls back to the Windows Credential Manager if no session password was entered. If a
path needs read authentication and you want the credentials in the document itself, tick
**Include credentials in URL** when adding: the `user:password@host` form is written into
`whepUrl`, and C-Bridge lifts it out at runtime and sends it as an `Authorization: Basic` header.
The trade-off is that the password then lives in the configuration file in clear text, so only do
this for documents you keep local.

Paths whose `readAuthentication` is set to `internal` are marked *read authentication required*
in the list. For those the checkbox is enabled automatically when adding — the entry would be
unreadable without credentials — and C-Bridge refuses to add one unless a password is available
(entered for the session or stored in the Windows Credential Manager).

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
