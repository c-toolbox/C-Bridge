---
sidebar_position: 2
title: Streams
---

# Streams

Each entry in `streams` describes one WHEP source and the sinks it feeds.

```json
{
  "id": "11111111-1111-1111-1111-111111111111",
  "name": "Camera 1",
  "enabled": true,
  "whepUrl": "http://mediamtx.example:8889/cam1/whep",
  "username": "reader",
  "preferredCodecs": ["h264", "h265"],
  "audioEnabled": true,
  "reconnectInitialMs": 500,
  "reconnectMaxMs": 15000,
  "sinks": []
}
```

| Field | Meaning |
| --- | --- |
| `id` | Stable identifier. Generated if omitted. |
| `name` | Display name, also the default NDI sender name. |
| `enabled` | Whether **Start all** includes this stream. |
| `whepUrl` | The MediaMTX WHEP endpoint, ending in `/whep`. |
| `username` | HTTP Basic user. The password is stored separately. |
| `preferredCodecs` | Offer order. The first entry is preferred. |
| `audioEnabled` | Whether to negotiate an Opus track. |
| `reconnectInitialMs` | First retry delay; doubles on each failure. |
| `reconnectMaxMs` | Ceiling for the backoff. |

## Credentials

Passwords never appear in the configuration document so it can be shared safely.
If a URL is written with embedded credentials —
`http://user:pass@host:8889/cam1/whep` — C-Bridge lifts them out and sends them in
an `Authorization: Basic` header instead of leaving them in the request line.

## Codec negotiation

C-Bridge offers the codecs in `preferredCodecs` and MediaMTX selects one. The
chosen codec is read back from the SDP answer, because the right depacketizer has
to be installed before media begins to flow.

Only H.264 and H.265 are supported. MediaMTX can also offer AV1, VP8 and VP9, but
those cannot be passed through to MPEG-TS and are not currently decoded.

## Reconnection

A failed stream moves to `Retrying` and reconnects with exponential backoff,
resetting to `reconnectInitialMs` after a successful connection. Sinks are closed
on disconnect and reopened when a keyframe arrives again, so a receiver sees a
clean restart rather than a corrupt stream.
