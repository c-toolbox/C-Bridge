---
sidebar_position: 1
title: Multicast
---

# Multicast networking

Multicast is what makes one sender affordable for many receivers. It is also the
part most likely to be misconfigured, because a broken multicast network fails
silently rather than with an error.

## Address planning

Use the administratively scoped range `239.0.0.0/8`. C-Bridge suggests
`239.1.x.y` with ports from 5000 upwards when you add a stream, and refuses to
start if two streams share an `address:port`.

:::warning The low 23 bits are what matter
An IPv4 multicast group maps onto a MAC address using only the low 23 bits of the
address. `239.1.1.1` and `239.129.1.1` therefore share a MAC address, and a host
joined to one will receive the other's traffic and have to discard it in software.
Vary the **last two octets** rather than the second.
:::

## Switch configuration

Enable **IGMP snooping** on the VLAN carrying the streams, and make sure an IGMP
querier exists on that VLAN — snooping without a querier causes membership state
to expire, which shows up as traffic that works for a minute and then stops.

Without snooping, a switch floods multicast to every port. With twenty-four 1080p60
streams this saturates every link on the VLAN.

## Picking the right interface

On a host with more than one NIC, always set `localAddress` on the sink to the IP
of the interface you intend to transmit from. Windows otherwise selects an adapter
by routing metric, which is frequently the management NIC rather than the 10GbE
data NIC.

## TTL

`ttl` defaults to 8. Use 1 to confine traffic to the local segment. Crossing a
router additionally requires multicast routing (PIM) to be configured — IGMP alone
only handles host-to-switch membership.

## Bandwidth

Multicast sinks carry the source bitrate unchanged, so twenty-four 1080p60 H.264
streams at 8 Mbps is roughly 200 Mbps. The same streams as NDI would be about
3 Gbps.

## No retransmission

UDP multicast has no recovery mechanism. A lost packet becomes a visible artifact
until the next keyframe. On a correctly configured switched LAN loss is normally
zero; if it is not, the fix belongs in the network rather than in C-Bridge.
Forward error correction (SMPTE 2022-1) and RIST are possible future sinks.
