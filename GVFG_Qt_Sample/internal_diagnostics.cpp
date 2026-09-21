#include "internal_diagnostics.h"

#include <algorithm>

#if GVFG_INTERNAL_DIAGNOSTICS
#include <gvfg_debug.h>
#endif

void InternalDiagnostics::beginStart(int channel)
{
    if (channel < 0 || channel >= static_cast<int>(metrics_.size()))
        return;
    metrics_[channel].startTime = std::chrono::steady_clock::now();
}

void InternalDiagnostics::finishStartCall(int channel)
{
    if (channel < 0 || channel >= static_cast<int>(metrics_.size()))
        return;
    ChannelMetrics &metrics = metrics_[channel];
    metrics.startCallMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - metrics.startTime)
                              .count();
}

void InternalDiagnostics::resetChannel(gvfg_handle handle, int channel)
{
    if (channel < 0 || channel >= static_cast<int>(baselines_.size()))
        return;

    baselines_[channel] = {};
    ChannelMetrics &metrics = metrics_[channel];
    metrics.successfulFrames = 0;
    metrics.windowSamples = 0;
    metrics.windowMaximumMs = 0.0;
    metrics.startupLatencyReported = false;
    metrics.videoReceived.store(0, std::memory_order_relaxed);
    metrics.readSamples.store(0, std::memory_order_relaxed);
    metrics.readAverageMs.store(0.0, std::memory_order_relaxed);
    metrics.readMaximumMs.store(0.0, std::memory_order_relaxed);
    metrics.readWindowMaximumMs.store(0.0, std::memory_order_relaxed);

#if GVFG_INTERNAL_DIAGNOSTICS
    gvfg_debug_backend_stats_t stats{};
    if (handle != nullptr &&
        gvfg_debug_get_channel_backend_stats(handle, channel, &stats) == GVFG_OK)
    {
        baselines_[channel] = {
            stats.video_dma_event_wakes,
            stats.extra_video_event_wakes,
            stats.video_frames_from_lib,
            stats.audio_dma_event_wakes,
            stats.extra_audio_event_wakes,
            stats.audio_frames_from_lib};
    }
#else
    (void)handle;
#endif
}

void InternalDiagnostics::recordVideoRead(int channel, double elapsedMs)
{
    constexpr uint64_t kTimingWarmupFrames = 30;
    constexpr uint64_t kTimingWindowFrames = 300;

    if (channel < 0 || channel >= static_cast<int>(metrics_.size()))
        return;

    ChannelMetrics &metrics = metrics_[channel];
    metrics.videoReceived.fetch_add(1, std::memory_order_relaxed);
    if (++metrics.successfulFrames <= kTimingWarmupFrames)
        return;

    ++metrics.windowSamples;
    metrics.windowMaximumMs = (std::max)(metrics.windowMaximumMs, elapsedMs);
    const uint64_t totalSamples = metrics.readSamples.fetch_add(1, std::memory_order_relaxed) + 1;
    const double previousAverage = metrics.readAverageMs.load(std::memory_order_relaxed);
    metrics.readAverageMs.store(
        previousAverage + (elapsedMs - previousAverage) / static_cast<double>(totalSamples),
        std::memory_order_relaxed);
    double observedMaximum = metrics.readMaximumMs.load(std::memory_order_relaxed);
    while (elapsedMs > observedMaximum &&
           !metrics.readMaximumMs.compare_exchange_weak(
               observedMaximum, elapsedMs, std::memory_order_relaxed))
    {
    }
    if (totalSamples <= kTimingWindowFrames)
        metrics.readWindowMaximumMs.store(metrics.windowMaximumMs, std::memory_order_relaxed);
    if (metrics.windowSamples >= kTimingWindowFrames)
    {
        metrics.readWindowMaximumMs.store(metrics.windowMaximumMs, std::memory_order_relaxed);
        metrics.windowSamples = 0;
        metrics.windowMaximumMs = 0.0;
    }
}

QString InternalDiagnostics::takeStartupLatencyLine(int channel,
                                                    double firstReadMs,
                                                    double firstRenderMs,
                                                    uint64_t frameId)
{
    if (channel < 0 || channel >= static_cast<int>(metrics_.size()))
        return {};

    ChannelMetrics &metrics = metrics_[channel];
    if (metrics.startupLatencyReported)
        return {};
    metrics.startupLatencyReported = true;

    const double totalMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - metrics.startTime)
                               .count();
    return QStringLiteral("CH%1 Startup latency | Start -> start_channel return=%2 ms | "
                          "first read_frame call -> return=%3 ms | "
                          "first render_frame call -> return=%4 ms | "
                          "Start -> first render_frame return=%5 ms | SDK_frame_id=%6")
        .arg(channel)
        .arg(metrics.startCallMs, 0, 'f', 3)
        .arg(firstReadMs, 0, 'f', 3)
        .arg(firstRenderMs, 0, 'f', 3)
        .arg(totalMs, 0, 'f', 3)
        .arg(static_cast<qulonglong>(frameId));
}

void InternalDiagnostics::appendAudioStatusLine(QStringList &lines,
                                                gvfg_handle handle,
                                                int channel,
                                                uint64_t audioReceived) const
{
#if GVFG_INTERNAL_DIAGNOSTICS
    if (handle == nullptr || channel < 0 || channel >= static_cast<int>(baselines_.size()))
        return;

    gvfg_debug_backend_stats_t stats{};
    if (gvfg_debug_get_channel_backend_stats(handle, channel, &stats) != GVFG_OK)
        return;

    const BackendBaseline &baseline = baselines_[channel];
    lines << QStringLiteral("CH%1 Audio Debug | LIB events DMA=%2 Extra=%3 | LIB->SDK %4 frames | SDK->APP %5 frames")
                 .arg(channel)
                 .arg(static_cast<qulonglong>(stats.audio_dma_event_wakes - baseline.audioDmaEventWakes))
                 .arg(static_cast<qulonglong>(stats.extra_audio_event_wakes - baseline.extraAudioEventWakes))
                 .arg(static_cast<qulonglong>(stats.audio_frames_from_lib - baseline.audioFramesFromLibrary))
                 .arg(static_cast<qulonglong>(audioReceived));
#else
    (void)lines;
    (void)handle;
    (void)channel;
    (void)audioReceived;
#endif
}

void InternalDiagnostics::appendVideoStatusLines(QStringList &lines,
                                                 gvfg_handle handle,
                                                 int channel,
                                                 bool zeroCopy) const
{
#if GVFG_INTERNAL_DIAGNOSTICS
    if (handle == nullptr || channel < 0 || channel >= static_cast<int>(baselines_.size()))
        return;

    const ChannelMetrics &metrics = metrics_[channel];
    const uint64_t readSamples = metrics.readSamples.load(std::memory_order_relaxed);
    lines << (readSamples >= 300
                  ? QStringLiteral("CH%1 Read | avg=%2 max300=%3 max=%4 ms samples=%5")
                        .arg(channel)
                        .arg(metrics.readAverageMs.load(std::memory_order_relaxed), 0, 'f', 3)
                        .arg(metrics.readWindowMaximumMs.load(std::memory_order_relaxed), 0, 'f', 3)
                        .arg(metrics.readMaximumMs.load(std::memory_order_relaxed), 0, 'f', 3)
                        .arg(static_cast<qulonglong>(readSamples))
                  : QStringLiteral("CH%1 Read | measuring").arg(channel));

    gvfg_debug_backend_stats_t stats{};
    if (gvfg_debug_get_channel_backend_stats(handle, channel, &stats) != GVFG_OK)
        return;

    const BackendBaseline &baseline = baselines_[channel];

    lines << (stats.get_frame_timing_samples >= 300
                  ? QStringLiteral("CH%1 %2 | avg=%3 max300=%4 max=%5 us samples=%6")
                        .arg(channel)
                        .arg(zeroCopy
                                 ? QStringLiteral("[LIB] GvfgGetVideoFrameZeroCopy")
                                 : QStringLiteral("[LIB] GvfgGetVideoFrame"))
                        .arg(stats.get_frame_timing_average_us, 0, 'f', 3)
                        .arg(stats.get_frame_timing_max300_us, 0, 'f', 3)
                        .arg(stats.get_frame_timing_max_us, 0, 'f', 3)
                        .arg(static_cast<qulonglong>(stats.get_frame_timing_samples))
                  : QStringLiteral("CH%1 [LIB] GetVideoFrame | measuring").arg(channel));
    lines << QStringLiteral("CH%1 Video Debug | LIB events DMA=%2 Extra=%3 | LIB->SDK %4 frames | SDK->APP %5 frames")
                 .arg(channel)
                 .arg(static_cast<qulonglong>(stats.video_dma_event_wakes - baseline.videoDmaEventWakes))
                 .arg(static_cast<qulonglong>(stats.extra_video_event_wakes - baseline.extraVideoEventWakes))
                 .arg(static_cast<qulonglong>(stats.video_frames_from_lib - baseline.videoFramesFromLibrary))
                 .arg(static_cast<qulonglong>(metrics.videoReceived.load(std::memory_order_relaxed)));
#else
    (void)lines;
    (void)handle;
    (void)channel;
    (void)zeroCopy;
#endif
}
