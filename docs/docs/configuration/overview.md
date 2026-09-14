---
sidebar_position: 1
title: Overview
---

# Configuration overview

C-Bridge keeps two kinds of settings apart.

| | Stored in | Contains |
| --- | --- | --- |
| **Configuration documents** | JSON files you choose | Streams and their sinks |
| **Application preferences** | `QSettings` (per user) | Last config, auto-load, theme |

Configuration documents are meant to be shared, committed to version control and
copied between machines. Credentials are therefore **not** written into them — see
[Streams](/configuration/streams).

## Document format

```json
{
  "version": 1,
  "name": "Studio feeds",
  "streams": []
}
```

`version` is checked on load. A document produced by a newer C-Bridge is rejected
rather than silently misread; older documents are migrated forward.

## Startup selection

The configuration loaded at startup is resolved in this order:

1. `--config <path>` on the command line
2. the last successfully opened document, if auto-load is enabled
3. nothing — C-Bridge opens with an empty configuration

## Validation

The document is validated before anything starts. These problems block a run:

- a stream with no valid WHEP URL
- an enabled stream with no enabled sink
- a multicast group that is not inside `224.0.0.0/4`
- the same multicast `address:port` used by more than one stream
- the same NDI sender name used by more than one stream
- an NDI sink with an empty sender name

The duplicate-endpoint checks matter more than they look: two streams sharing a
multicast endpoint produces a single corrupt interleaved stream rather than an
error at the receiver.
