#include "audio_playback.h"

#include <utility>

namespace
{
constexpr auto kPlaybackRetryDelay = std::chrono::milliseconds(500);
}

AudioPlayback::~AudioPlayback() { stop(); }

gvfg_audio_playback_status_t AudioPlayback::startPlayerLocked()
{
    gvfg_audio_player next = nullptr;
    gvfg_audio_playback_status_t status = gvfg_audio_player_create(&next);
    if (status == GVFG_AUDIO_PLAYBACK_OK)
    {
        gvfg_audio_playback_format_t outputFormat{};
        outputFormat.sample_rate = format_.sample_rate;
        outputFormat.channels = format_.channels;
        outputFormat.bits_per_sample = format_.bits_per_sample;
        status = gvfg_audio_player_start(next, &outputFormat);
    }
    if (status == GVFG_AUDIO_PLAYBACK_OK)
    {
        player_ = next;
        return status;
    }
    gvfg_audio_player_destroy(next);
    nextRetry_ = std::chrono::steady_clock::now() + kPlaybackRetryDelay;
    return status;
}

void AudioPlayback::start(int channel, const gvfg_audio_format_t &format,
                          LogCallback logCallback)
{
    stop();
    gvfg_audio_playback_status_t status;
    LogCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_ = channel;
        format_ = format;
        logCallback_ = std::move(logCallback);
        statistics_ = {};
        active_ = true;
        status = startPlayerLocked();
        callback = logCallback_;
    }
    if (!callback)
        return;
    if (status == GVFG_AUDIO_PLAYBACK_OK)
        callback(QStringLiteral("CH%1 [AUDIO HELPER] started | %2 Hz %3 ch %4-bit PCM")
                     .arg(channel).arg(format.sample_rate).arg(format.channels).arg(format.bits_per_sample));
    else
        callback(QStringLiteral("CH%1 [AUDIO HELPER] WARNING start failed | status=%2 error=%3 | retry_ms=500")
                     .arg(channel).arg(static_cast<int>(status))
                     .arg(QString::fromUtf8(gvfg_audio_player_strerror(status))));
}

void AudioPlayback::stop()
{
    gvfg_audio_player player = nullptr;
    LogCallback callback;
    Statistics stats;
    int channel = 0;
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasActive = active_;
        active_ = false;
        player = player_;
        player_ = nullptr;
        callback = logCallback_;
        logCallback_ = {};
        stats = statistics_;
        channel = channel_;
    }
    gvfg_audio_player_destroy(player);
    if (wasActive && callback)
        callback(QStringLiteral("CH%1 [AUDIO HELPER] stopped | received=%2 release_failed=%3 output_failed=%4 queue_full=%5 retries=%6 recoveries=%7")
                     .arg(channel)
                     .arg(static_cast<qulonglong>(stats.receivedFrames))
                     .arg(static_cast<qulonglong>(stats.releaseFailedFrames))
                     .arg(static_cast<qulonglong>(stats.outputFailedFrames))
                     .arg(static_cast<qulonglong>(stats.queueFullFrames))
                     .arg(static_cast<qulonglong>(stats.retryAttempts))
                     .arg(static_cast<qulonglong>(stats.recoveries)));
}

bool AudioPlayback::enqueue(std::vector<uint8_t> pcm, uint64_t timestampNs)
{
    QString message;
    LogCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
            return false;
        if (!player_ && std::chrono::steady_clock::now() >= nextRetry_)
        {
            ++statistics_.retryAttempts;
            const auto status = startPlayerLocked();
            if (status == GVFG_AUDIO_PLAYBACK_OK)
            {
                ++statistics_.recoveries;
                message = QStringLiteral("CH%1 [AUDIO HELPER] output recovered | retry=%2")
                              .arg(channel_).arg(static_cast<qulonglong>(statistics_.retryAttempts));
            }
            else
            {
                message = QStringLiteral("CH%1 [AUDIO HELPER] retry failed | retry=%2 status=%3 error=%4")
                              .arg(channel_).arg(static_cast<qulonglong>(statistics_.retryAttempts))
                              .arg(static_cast<int>(status))
                              .arg(QString::fromUtf8(gvfg_audio_player_strerror(status)));
            }
        }
        if (player_)
        {
            const auto status = gvfg_audio_player_write_timed(
                player_, pcm.data(), static_cast<uint32_t>(pcm.size()), timestampNs);
            if (status != GVFG_AUDIO_PLAYBACK_OK)
            {
                ++statistics_.outputFailedFrames;
                if (status == GVFG_AUDIO_PLAYBACK_EQUEUE_FULL)
                {
                    ++statistics_.queueFullFrames;
                    message = QStringLiteral("CH%1 [AUDIO HELPER] queue full | dropped=%2 bytes=%3 timestamp_ns=%4")
                                  .arg(channel_)
                                  .arg(static_cast<qulonglong>(statistics_.queueFullFrames))
                                  .arg(pcm.size())
                                  .arg(static_cast<qulonglong>(timestampNs));
                }
                else
                {
                    gvfg_audio_player failed = player_;
                    player_ = nullptr;
                    gvfg_audio_player_destroy(failed);
                    nextRetry_ = std::chrono::steady_clock::now() + kPlaybackRetryDelay;
                    message = QStringLiteral("CH%1 [AUDIO HELPER] playback failed | status=%2 error=%3 bytes=%4 timestamp_ns=%5 | retry_ms=500")
                                  .arg(channel_).arg(static_cast<int>(status))
                                  .arg(QString::fromUtf8(gvfg_audio_player_strerror(status)))
                                  .arg(pcm.size()).arg(static_cast<qulonglong>(timestampNs));
                }
            }
        }
        callback = logCallback_;
    }
    if (!message.isEmpty() && callback)
        callback(message);
    return true;
}

void AudioPlayback::updateVideoTimestamp(uint64_t timestampNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_)
        gvfg_audio_player_update_video_timestamp(player_, timestampNs);
}

void AudioPlayback::resetTimeline(const QString &reason)
{
    LogCallback callback;
    gvfg_audio_playback_status_t status = GVFG_AUDIO_PLAYBACK_OK;
    int channel = 0;
    bool attempted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (player_)
        {
            attempted = true;
            status = gvfg_audio_player_reset_timeline(player_);
        }
        callback = logCallback_;
        channel = channel_;
    }
    if (attempted && callback)
        callback(QStringLiteral("CH%1 [AUDIO HELPER] timeline reset | reason=%2 status=%3 error=%4")
                     .arg(channel).arg(reason).arg(static_cast<int>(status))
                     .arg(QString::fromUtf8(gvfg_audio_player_strerror(status))));
}

void AudioPlayback::recordReceivedFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++statistics_.receivedFrames;
}

void AudioPlayback::recordReleaseFailure()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++statistics_.releaseFailedFrames;
}

AudioPlayback::Statistics AudioPlayback::statistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}
