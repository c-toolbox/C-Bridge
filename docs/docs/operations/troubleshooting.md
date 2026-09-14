---
sidebar_position: 3
title: Troubleshooting
---

# Troubleshooting

The log at `data/log/cbridge.log` rotates at 8 MB and is the first place to look.
The startup banner records the resolved FFmpeg, libdatachannel and NDI versions.

## The stream never leaves Connecting

The WHEP handshake is not completing.

- Confirm the endpoint answers: `curl -i -X OPTIONS http://host:8889/cam1/whep`
- Check the path exists in MediaMTX and currently has a publisher
- With authentication enabled, confirm the user has `read` permission
- Check `webrtcLocalUDPAddress` (default `:8189`) is reachable; the HTTP handshake
  can succeed while media is blocked by a firewall

## It reaches Running but nothing is output

Sinks open only after the **first keyframe**. If the publisher uses a long GOP,
expect a delay of exactly one keyframe interval. If nothing appears at all, the
source may be publishing a codec C-Bridge does not negotiate — the log reports
"contains no supported video codec".

## Nothing arrives at the multicast receiver

Work outwards from the sender:

1. `ffprobe udp://239.1.1.1:5000` **on the C-Bridge host**. If this works, the
   sink is fine and the problem is the network.
2. The same command on the receiving host. If it fails, look at IGMP snooping, the
   querier and TTL — see [Multicast](/networking/multicast).
3. On a multi-NIC sender, set `localAddress` on the sink. This is the single most
   common cause.

## The picture is fine but stutters or tears

Packet loss on the multicast path. UDP has no retransmission, so loss shows as
artifacts until the next keyframe. Check switch port error counters and whether
the link is saturated.

## Latency is worse than expected

Almost always the receiver. An untuned mpv buffers heavily; see
[Receiving with mpv](/networking/mpv). Measure with the low-latency flags before
concluding the bridge is at fault.

## NDI sinks fail while multicast works

The NDI runtime was not found. The log records which paths were searched. Install
the NDI redistributable, or set `NDI_RUNTIME_DIR_V6`. Multicast sinks are
unaffected by this failure by design.

## The NDI source does not appear in receivers

- Sender names must be unique on the machine; C-Bridge rejects duplicates at
  validation, but another application may already use the name
- NDI discovery uses mDNS, which is often blocked across subnets. Receivers on a
  different VLAN need an NDI discovery server
- Check the Windows firewall allows `C-Bridge.exe`

## Decoding is slower than expected

Confirm NVDEC is actually in use with `nvidia-smi dmon -s u`; see
[Performance](/operations/performance).

## Wrong FFmpeg gets loaded

The startup banner prints the FFmpeg version. If it is not the version you
deployed, another FFmpeg earlier in the DLL search path is winning. The build
pins its include directory ahead of any other, but at runtime the DLLs beside the
executable must also be the matching ones.
