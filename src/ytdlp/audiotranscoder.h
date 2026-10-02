/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "media/avwrappers.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace CBridge {

/// Transcodes source audio (AAC from YouTube, or whatever codec the feed carries) to Opus
/// 48 kHz stereo, the pipeline's passthrough contract. YouTube never serves H.264+Opus in
/// one stream, so a YouTube source receives AAC and must convert it before any sink can
/// carry it; keeping the transcode inside the source leaves the pipeline, queue and every
/// sink untouched (see YTDLP_SOURCE_PLAN.md).
///
/// The class is not thread-safe: feed packets from one thread only (the source's worker).
class AudioTranscoder
{
public:
    /// Receives one encoded Opus frame (a raw RFC 6716 packet) after each transcode() call.
    using PacketCallback = std::function<void(const std::uint8_t *, std::size_t)>;

    AudioTranscoder();
    ~AudioTranscoder();

    AudioTranscoder(const AudioTranscoder &) = delete;
    AudioTranscoder &operator=(const AudioTranscoder &) = delete;

    /// Prepares the decoder for `codecId` audio at `sampleRate` Hz with `channels` channels
    /// (1 or 2) and the Opus encoder at `bitrateKbps`. The output is always Opus 48 kHz
    /// stereo, whatever the input. `extradata` carries the decoder's private configuration
    /// when the feed delivers raw (headerless) AAC — an AudioSpecificConfig from the HLS
    /// init segment. TS/ADTS feeds leave it empty and the decoder self-syncs on the header.
    bool open(AVCodecID codecId, int sampleRate, int channels, int bitrateKbps, QString *error,
              const QByteArray &extradata = {});

    void close();
    bool isOpen() const { return m_encoder != nullptr; }

    /// Feeds one AAC packet and delivers every resulting Opus frame through `callback`.
    /// Samples that do not fill a full 20 ms Opus frame are buffered for the next call.
    bool transcode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                   const PacketCallback &callback, QString *error);

private:
    /// Resamples `frame` to 48 kHz stereo planar float and appends it to the sample buffer.
    bool appendDecodedFrame(AVFrame *frame, QString *error);

    /// Encodes as many full Opus frames as the sample buffer holds, delivering each through
    /// `callback`. Returns true with no callback when fewer than one frame is buffered.
    bool encodeBuffered(const PacketCallback &callback, QString *error);

    CodecContextPtr m_decoder;  // whatever codec the feed carries (AAC on YouTube)
    CodecContextPtr m_encoder;  // libopus @ 48 kHz stereo

    /// Copy of the decoder extradata handed to open(); the decoder's pointer refers into
    /// this buffer, which must stay alive (and owned by us) for the decoder's lifetime.
    QByteArray m_extradata;

    /// Sample format the Opus encoder accepts: S16 for the libopus wrapper, FLTP for the
    /// native encoder. Chosen once in open() because it is fixed per codec instance.
    int m_encFormat = -1;
    ResamplerPtr m_resampler;   // decoded layout/rate -> FLTP 48 kHz stereo
    FramePtr m_frame;           // decoder output scratch
    FramePtr m_fltpFrame;       // resampled scratch
    FramePtr m_encFrame;        // encoder input, exactly frame_size samples
    PacketPtr m_packet;         // decoder input scratch
    PacketPtr m_encPacket;      // encoder output scratch

    /// Planar float sample buffer (one plane per channel) holding the decoded audio that has
    /// not yet filled a full Opus frame.
    std::vector<float> m_planes[2];

    int m_inFormat = -1;        // last decoded format fed to the resampler
    int m_inChannels = 0;
    int m_inRate = 0;
    std::int64_t m_encodedSamples = 0; // encoder-side sample clock for frame pts
};

} // namespace CBridge
