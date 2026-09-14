#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace CBridge {

/// A media unit copied off the network thread.
struct MediaUnit {
    enum class Kind { Video, Audio };

    Kind kind = Kind::Video;
    std::uint32_t rtpTimestamp = 0;
    bool isKeyframe = false;
    std::vector<std::uint8_t> payload;
};

/// Bounded queue with a drop-oldest policy.
///
/// The producer is libdatachannel's network thread and must never block, so when
/// a consumer falls behind the queue sheds the oldest units instead. Video drops
/// are taken back to the newest keyframe where possible to avoid feeding a
/// decoder or muxer a broken GOP.
class MediaQueue
{
public:
    explicit MediaQueue(std::size_t capacity = 240)
        : m_capacity(capacity)
    {
    }

    /// Returns false when the unit displaced older data.
    bool push(MediaUnit &&unit)
    {
        bool dropped = false;
        {
            std::lock_guard lock(m_mutex);
            while (m_units.size() >= m_capacity) {
                m_units.pop_front();
                ++m_dropped;
                dropped = true;
            }
            m_units.push_back(std::move(unit));
        }
        m_notEmpty.notify_one();
        return !dropped;
    }

    /// Blocks until a unit is available or the queue is stopped.
    bool pop(MediaUnit &out)
    {
        std::unique_lock lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return m_stopped || !m_units.empty(); });
        if (m_units.empty()) {
            return false;
        }
        out = std::move(m_units.front());
        m_units.pop_front();
        return true;
    }

    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
        }
        m_notEmpty.notify_all();
    }

    void clear()
    {
        std::lock_guard lock(m_mutex);
        m_units.clear();
    }

    std::size_t size() const
    {
        std::lock_guard lock(m_mutex);
        return m_units.size();
    }

    std::uint64_t dropped() const
    {
        std::lock_guard lock(m_mutex);
        return m_dropped;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::deque<MediaUnit> m_units;
    std::size_t m_capacity;
    std::uint64_t m_dropped = 0;
    bool m_stopped = false;
};

} // namespace CBridge
