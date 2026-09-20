/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/opuspayloadsanitizer.h"

namespace CBridge {

namespace {

/// Port of parse_size() from libopus src/opus.c: reads the one- or two-byte frame size prefix
/// that starts a VBR frame, returning how many bytes the prefix consumed (-1 if truncated).
int parseSize(const std::uint8_t *data, int len, int &size) {
    if (len < 1) {
        size = -1;
        return -1;
    }
    if (data[0] < 252) {
        size = data[0];
        return 1;
    }
    if (len < 2) {
        size = -1;
        return -1;
    }
    size = 4 * int(data[1]) + int(data[0]);
    return 2;
}

/// Port of opus_packet_get_samples_per_frame() at Fs = 48000 (the only rate Opus supports).
int samplesPerFrame(std::uint8_t toc) {
    const int fs = 48000;
    if (toc & 0x80) { // hybrid mode: SILK + CELT, frame length in ms from the config bits
        int audiosize = (toc >> 3) & 0x3;
        return (fs << audiosize) / 400;
    }
    if ((toc & 0x60) == 0x60) { // CELT-only: 2.5 ms or 5 ms frames
        return (toc & 0x08) ? fs / 50 : fs / 100;
    }
    int audiosize = (toc >> 3) & 0x3;
    if (audiosize == 3) {
        return fs * 60 / 1000; // 60 ms
    }
    return (fs << audiosize) / 100; // 20, 40 or 80 ms
}

/// Structural validity of a non-self-delimited RFC 6716 Opus packet: exactly the length checks
/// opus_packet_parse_impl() performs before it will decode anything.
bool passesStructuralChecks(const std::uint8_t *data, int len) {
    if (len < 1) {
        return false;
    }

    const int framesize = samplesPerFrame(data[0]);
    const unsigned char toc = data[0];
    int rest = len - 1; // bytes after the TOC byte

    switch (toc & 3) {
    case 0: // one frame: everything after the TOC is that frame's payload
        return rest <= 1275;

    case 1: // two CBR frames of equal size
        if (rest & 1) {
            return false;
        }
        return rest / 2 <= 1275;

    case 2: { // two VBR frames with explicit sizes
        int firstSize = -1;
        const int bytes = parseSize(data + 1, rest, firstSize);
        if (firstSize < 0) {
            return false;
        }
        rest -= bytes;
        if (firstSize > rest) {
            return false;
        }
        return rest - firstSize <= 1275;
    }

    default: { // multiple frames, 0-120 ms total
        if (rest < 1) {
            return false;
        }
        const unsigned char ch = data[1];
        const int count = ch & 0x3F;
        if (count <= 0 || framesize * count > 5760) {
            return false;
        }
        rest -= 1;

        // Optional padding length: one or more bytes, each 254 max.
        const std::uint8_t *cursor = data + 2;
        if (ch & 0x40) {
            int padByte = 0;
            do {
                if (rest <= 0) {
                    return false;
                }
                padByte = *cursor++;
                --rest;
                rest -= (padByte == 255 ? 254 : padByte);
            } while (padByte == 255);
        }
        if (rest < 0) {
            return false;
        }

        int lastSize;
        if (ch & 0x80) { // VBR: count-1 explicit size prefixes, the last frame absorbs the rest
            lastSize = rest;
            for (int i = 0; i < count - 1; ++i) {
                int size = -1;
                const int bytes = parseSize(cursor, rest, size);
                if (size < 0) {
                    return false;
                }
                rest -= bytes;
                if (size > rest) {
                    return false;
                }
                cursor += bytes;
                lastSize -= bytes + size;
            }
            if (lastSize < 0) {
                return false;
            }
        } else { // CBR: all frames the same size, so the remainder must divide evenly
            lastSize = rest / count;
            if (lastSize * count != rest) {
                return false;
            }
        }
        return lastSize <= 1275;
    }
    }
}

} // namespace

std::size_t OpusPayloadSanitizer::validPrefixLength(const std::uint8_t *data, std::size_t size) {
    if (size == 0 || data == nullptr) {
        return 0;
    }
    for (std::size_t cut = 0; cut <= kMaxTrailingGarbage && cut < size; ++cut) {
        const std::size_t candidate = size - cut;
        if (passesStructuralChecks(data, int(candidate))) {
            return candidate;
        }
    }
    // Nothing recoverable within the allowed strip: hand back the original payload untouched.
    return size;
}

bool OpusPayloadSanitizer::structurallyValid(const std::uint8_t *data, std::size_t size) {
    if (size == 0 || data == nullptr) {
        return false;
    }
    return passesStructuralChecks(data, int(size));
}

} // namespace CBridge