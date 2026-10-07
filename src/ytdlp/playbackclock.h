// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace CBridge {

// One clock shared by audio and video. Downloads may arrive in segment-sized bursts;
// delivery must follow decode time rather than network arrival or reordered video PTS.
class PlaybackClock
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void reset() { m_started = false; }
    void delay(Clock::duration duration) { m_wall += duration; }

    TimePoint deadline(std::int64_t mediaUs, TimePoint now)
    {
        if (!m_started) {
            m_started = true;
            m_mediaUs = mediaUs;
            m_wall = now;
        }
        auto due = m_wall + std::chrono::microseconds(mediaUs - m_mediaUs);
        // After a network stall, resume at normal speed instead of flooding the queue
        // with every overdue packet. Also recover from a discontinuous input timeline.
        if (now - due > std::chrono::milliseconds(250)
            || due - now > std::chrono::seconds(2)) {
            m_mediaUs = mediaUs;
            m_wall = now;
            due = now;
        }
        return due;
    }

private:
    bool m_started = false;
    std::int64_t m_mediaUs = 0;
    TimePoint m_wall {};
};

} // namespace CBridge
