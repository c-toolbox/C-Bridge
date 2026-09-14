---
sidebar_position: 2
title: Performance
---

# Performance

## What each sink costs

Per 1080p60 stream:

| Resource | Multicast sink | NDI sink |
| --- | --- | --- |
| Network | source bitrate (4–10 Mbps) | ~125 Mbps |
| GPU decode | none | one NVDEC session |
| PCIe | none | ~250 MB/s readback |
| CPU | negligible | ~1 core (SpeedHQ encode) |

Twenty-four multicast streams is roughly 200 Mbps and almost no CPU. Twenty-four
NDI streams is roughly 3 Gbps, 24 cores and 6 GB/s across PCIe — which is why the
two sinks should not be treated as interchangeable.

## Threading

Each stream runs:

- libdatachannel's shared network threads
- one pipeline worker thread that drives every sink for that stream

The network thread only copies each access unit into a bounded queue. The worker
drains it. This means a slow or blocked sink can never apply back-pressure to the
transport, which would otherwise stall the peer connection and eventually drop the
stream.

## Backpressure

The queue is **drop-oldest**. When a sink cannot keep up, the oldest units are
discarded and the per-stream `dropped` counter rises. A steadily climbing counter
means the sink is structurally too slow, not that a transient occurred.

A non-zero queue depth that does not return to zero is the earliest warning sign;
watch it before watching dropped frames.

## GPU sharing

All decoders share **one CUDA device context** per process. Creating a context per
stream would multiply GPU memory use and add context switching at twenty-four
streams.

Note that NVDEC decode sessions are not the same limit as NVENC encode sessions;
C-Bridge only decodes, so the consumer-card session cap on encoding does not apply.

## Verifying hardware decoding

```powershell
nvidia-smi dmon -s u
```

The `dec` column should be non-zero while NDI sinks are running. If it stays at
zero, decoding silently fell back to software — check the log for the decoder line,
which records `NVDEC` or `software` when each decoder opens.

## Scaling guidance

- Prefer multicast for everything that can consume it.
- Cap the number of concurrent NDI sinks deliberately rather than discovering the
  limit under load.
- On a shared machine, remember other software competes for the same cores; the
  NDI encode path is the first thing to suffer.
