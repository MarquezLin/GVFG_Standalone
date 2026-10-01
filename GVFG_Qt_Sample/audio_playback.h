#pragma once

#include <gvfg_capture.h>
#include <gvfg_audio_playback.h>

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class AudioPlayback final
{
public:
    struct Statistics
    {
        uint64_t receivedFrames = 0;
        uint64_t releaseFailedFrames = 0;
        uint64_t outputFailedFrames = 0;
        uint64_t queueFullFrames = 0;
        uint64_t retryAttempts = 0;
        uint64_t recoveries = 0;
    };

    using LogCallback = std::function<void(const QString &)>;

    AudioPlayback() = default;
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback &) = delete;
    AudioPlayback &operator=(const AudioPlayback &) = delete;

    void start(int channel, const gvfg_audio_format_t &format, LogCallback logCallback);
    void stop();
    bool enqueue(std::vector<uint8_t> pcm, uint64_t timestampNs);
    void updateVideoTimestamp(uint64_t timestampNs);
    void resetTimeline(const QString &reason);
    void recordReceivedFrame();
    void recordReleaseFailure();
    Statistics statistics() const;

private:
    gvfg_audio_playback_status_t startPlayerLocked();

    int channel_ = 0;
    gvfg_audio_format_t format_{};
    LogCallback logCallback_;
    mutable std::mutex mutex_;
    gvfg_audio_player player_ = nullptr;
    bool active_ = false;
    std::chrono::steady_clock::time_point nextRetry_{};
    Statistics statistics_{};
};
