/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace CBridge {

/// Recovers structurally valid Opus payloads from packets that carry undeclared trailing bytes.
///
/// Some upstream encoders append one or a few extra bytes to their Opus RTP payloads without
/// declaring them in the TOC. For code mode 1 ("two CBR frames") any odd payload length after
/// the TOC byte is rejected outright by libopus's opus_packet_parse_impl(), so every conforming
/// decoder fails on those packets while a size-1 retry decodes fine (the NDI path hides this in
/// AudioDecoder). Passthrough sinks (TS multicast, RTP multicast, RTSP) deliver the raw bytes and
/// their receivers drop most of the audio.
class OpusPayloadSanitizer {
public:
    /// Maximum number of trailing bytes to strip when recovering a valid packet. The observed
    /// defect is a single stray byte; four leaves headroom without risking an over-aggressive cut.
    static constexpr std::size_t kMaxTrailingGarbage = 4;

    /// Returns the largest prefix length of [data, data + size) that passes the structural checks
    /// libopus performs in opus_packet_parse_impl() before it will decode anything (self-delimited
    /// mode off). A well-formed packet returns size unchanged. If no prefix down to
    /// kMaxTrailingGarbage bytes is structurally valid, size is returned unchanged so receivers do
    /// their own concealment instead of us guessing at a larger cut.
    ///
    /// Note: structural validity is necessary but not sufficient for decoding (the codebook still
    /// has to parse), and mode 0 / VBR packets with trailing garbage stay structurally valid, so
    /// this only recovers what the TOC length rules can prove wrong — which covers the CBR defect.
    static std::size_t validPrefixLength(const std::uint8_t *data, std::size_t size);

    /// True when [data, data + size) passes those same structural checks as-is (no stripping).
    /// Distinguishes a well-formed packet from one that is invalid and unrecoverable, which
    /// validPrefixLength() reports the same way.
    static bool structurallyValid(const std::uint8_t *data, std::size_t size);

private:
    OpusPayloadSanitizer() = delete;
};

} // namespace CBridge