/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/streamsource.h"

namespace CBridge {

StreamSource::StreamSource(QObject *parent)
    : QObject(parent)
{
}

StreamSource::~StreamSource() = default;

void StreamSource::setVideoCallback(MediaFrameCallback callback)
{
    m_onVideo = std::move(callback);
}

void StreamSource::setAudioCallback(MediaFrameCallback callback)
{
    m_onAudio = std::move(callback);
}

StreamState StreamSource::state() const
{
    return m_state.load(std::memory_order_relaxed);
}

void StreamSource::setState(StreamState state)
{
    const StreamState previous = m_state.exchange(state, std::memory_order_relaxed);
    if (previous != state) {
        Q_EMIT stateChanged(state);
    }
}

} // namespace CBridge