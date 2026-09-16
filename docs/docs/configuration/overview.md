---
sidebar_position: 1
title: Overview
---

# Configuration overview

C-Bridge keeps two kinds of settings apart.

| | Stored in | Contains |
| --- | --- | --- |
| **Configuration documents** | JSON files you choose | Streams and their sinks |
| **Application preferences** | KConfig (`C-Bridge/cbridge.conf`, per user) | Startup document, auto-load, start-on-load flags |

Configuration documents are meant to be shared, committed to version control and
copied between machines. Credentials are therefore **not** written into them — see
[Streams](/configuration/streams). Preferences live in the KDE KConfig store and are
edited through the **Settings** dialog on the streams page.

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

1. `--stream-config <path>` on the command line
2. the startup document chosen in **Settings**, if it exists
3. the last successfully opened document, if auto-load is enabled
4. nothing — C-Bridge opens with an empty configuration

## Starting streams at startup

After a configuration has loaded at startup, C-Bridge can start streams automatically:

- **Start all enabled streams after the configuration has loaded** starts every enabled stream.
- Each stream in the document also has its own *start on load* flag in **Settings**; flagged
  streams start even when the global switch is off. Disabled streams never start automatically.

The `--autostart` command-line option still forces a full start regardless of these settings.

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
