#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace gvfg_playback_detail {
constexpr uint64_t second = 1'000'000'000ULL;
inline uint64_t sample_time(uint64_t frames, uint32_t rate) {
    return frames / rate * second + frames % rate * second / rate;
}
inline int64_t difference(uint64_t a, uint64_t b) {
    return a >= b ? static_cast<int64_t>(a - b) : -static_cast<int64_t>(b - a);
}
// PCM sample count is nominal media time; SDK video timestamps are delivery
// time. Track slow source-clock differences without assigning callback jitter
// directly to every packet. This cannot recover missing samples or determine
// hardware capture latency. The resulting spans remain contiguous.
struct AudioSourceClock {
    uint64_t first = 0, frames = 0, end = 0;
    int64_t offset = 0;
    double filtered_error = 0;
    struct Packet { uint64_t pts, end; };
    Packet advance(uint64_t delivery, uint64_t count, uint32_t rate) {
        const uint64_t previous = sample_time(frames, rate);
        const uint64_t total = sample_time(frames + count, rate);
        const uint64_t duration = total - previous;
        if (!end) {
            first = delivery - duration;
            frames += count; end = delivery;
            return {first, end};
        }
        const uint64_t nominal = first + total;
        const uint64_t predicted = offset >= 0 ? nominal + offset : nominal - static_cast<uint64_t>(-offset);
        const double alpha = (std::min)(1.0, static_cast<double>(duration) / 500'000'000.0);
        filtered_error += alpha * (static_cast<double>(difference(delivery, predicted)) - filtered_error);
        constexpr double tolerance = 5'000'000; // Ignore small delivery jitter.
        const double phase = filtered_error > tolerance ? filtered_error - tolerance :
            (filtered_error < -tolerance ? filtered_error + tolerance : 0);
        // At most 0.25% source-time stretch per packet; output drift correction
        // remains responsible for resampling, independently of this mapping.
        const double limit = static_cast<double>(duration) / 400.0;
        offset += static_cast<int64_t>(std::clamp(phase, -limit, limit));
        const uint64_t begin = end;
        end = offset >= 0 ? nominal + offset : nominal - static_cast<uint64_t>(-offset);
        frames += count;
        return {begin, end};
    }
};
// Fit delivered nominal PCM duration against delivery time over a window.
// Using all observations avoids classifying one batched callback's endpoint
// jitter as a sustained source-rate mismatch during recovery.
struct RecoveryRate {
    uint64_t first = 0, frames = 0, count = 0;
    double x = 0, y = 0, xx = 0, xy = 0;
    bool stable = false;
    void observe(uint64_t delivery, uint64_t samples, uint32_t rate) {
        if (!first || delivery - first > 3'000'000'000) {
            *this = {}; first = delivery; return;
        }
        frames += samples;
        const uint64_t elapsed = delivery - first;
        const double t = static_cast<double>(elapsed) / second;
        const double media = static_cast<double>(frames) / rate;
        ++count; x += t; y += media; xx += t * t; xy += t * media;
        if (elapsed >= 2'000'000'000) {
            const double denominator = count * xx - x * x;
            stable = denominator > 0 && std::fabs((count * xy - x * y) / denominator - 1.0) <= .0025;
            first = delivery; frames = count = 0; x = y = xx = xy = 0;
        }
    }
};
// Source-time mapping belongs to the shared playback core. The backend only
// reports consumed PCM frames; accepted spans follow the same FIFO order.
struct AudioTimeline {
    struct Span { uint64_t end_frame, pts, frames, end_pts; };
    std::deque<Span> spans;
    uint64_t accepted_frames = 0;
    void append(uint64_t pts, uint64_t frames, uint64_t end_pts) {
        spans.push_back({accepted_frames + frames, pts, frames, end_pts});
        accepted_frames += frames;
    }
    void rollback() {
        accepted_frames -= spans.back().frames;
        spans.pop_back();
    }
    uint64_t position(uint64_t played) {
        while (spans.size() > 1 && played >= spans.front().end_frame) spans.pop_front();
        if (spans.empty()) return 0;
        const auto &span = spans.front();
        const uint64_t begin = span.end_frame - span.frames;
        const uint64_t offset = played > begin ? (std::min)(played - begin, span.frames) : 0;
        return span.pts + offset * (span.end_pts - span.pts) / span.frames;
    }
};
struct Clock {
    uint64_t pts = 0, wall = 0;
    bool paused = true;
    uint64_t get(uint64_t now) const { return pts + (!paused && now > wall ? now - wall : 0); }
    void set(uint64_t value, uint64_t now, bool stop) { pts = value; wall = now; paused = stop; }
};
// FFplay-style filtered A/V error, implemented independently. Audio output
// rate adjustment is delegated to Windows shared-mode resampling (no FFmpeg
// runtime dependency). Slew limit prevents audible abrupt speed changes.
struct DriftFilter {
    double average = 0, ratio = 1;
    unsigned count = 0;
    double update(int64_t error_ns) {
        const double error = static_cast<double>(error_ns) / second;
        if (std::fabs(error) > .250) { average = 0; count = 0; ratio = 1; return ratio; }
        average = count ? average * .9 + error * .1 : error;
        if (++count < 20) return ratio;
        const double target = std::fabs(average) < .003 ? 1.0 :
            std::clamp(1.0 - average / 2.0, .995, 1.005);
        ratio += std::clamp(target - ratio, -.0001, .0001);
        return ratio;
    }
};
}
