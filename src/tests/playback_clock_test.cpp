// SPDX-License-Identifier: GPL-3.0-or-later
#include "ytdlp/playbackclock.h"
#include <cstdio>

using namespace CBridge;
using namespace std::chrono;

int main()
{
    PlaybackClock clock;
    const auto start = PlaybackClock::TimePoint {} + seconds(10);
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        failures += !ok;
    };
    check(clock.deadline(1000000, start) == start, "first packet anchors the clock");
    check(clock.deadline(1033333, start) == start + microseconds(33333),
          "burst video waits for its decode timestamp");
    check(clock.deadline(1020000, start) == start + milliseconds(20),
          "audio shares the video timeline");
    clock.delay(seconds(5));
    check(clock.deadline(1066666, start + seconds(5)) == start + seconds(5) + microseconds(66666),
          "pause shifts deadlines without catch-up");
    check(clock.deadline(1100000, start + seconds(8)) == start + seconds(8),
          "network stall reanchors instead of bursting overdue packets");
    check(clock.deadline(1133333, start + seconds(8)) == start + seconds(8) + microseconds(33333),
          "normal cadence resumes after a stall");
    clock.reset();
    check(clock.deadline(90000000, start) == start, "seek starts a fresh timeline");
    check(clock.deadline(0, start) == start, "backward discontinuity recovers");
    return failures ? 1 : 0;
}
