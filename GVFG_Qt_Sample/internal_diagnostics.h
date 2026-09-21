#pragma once

#include <gvfg_capture.h>

#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

class InternalDiagnostics final
{
public:
    void beginStart(int channel);
    void finishStartCall(int channel);
    void resetChannel(gvfg_handle handle, int channel);
    void recordVideoRead(int channel, double elapsedMs);
    QString takeStartupLatencyLine(int channel,
                                   double firstReadMs,
                                   double firstRenderMs,
                                   uint64_t frameId);
    void appendAudioStatusLine(QStringList &lines,
                               gvfg_handle handle,
                               int channel,
                               uint64_t audioReceived) const;
    void appendVideoStatusLines(QStringList &lines,
                                gvfg_handle handle,
                                int channel,
                                bool zeroCopy) const;

private:
    struct BackendBaseline
    {
        uint64_t videoDmaEventWakes = 0;
        uint64_t extraVideoEventWakes = 0;
        uint64_t videoFramesFromLibrary = 0;
        uint64_t audioDmaEventWakes = 0;
        uint64_t extraAudioEventWakes = 0;
        uint64_t audioFramesFromLibrary = 0;
    };

    struct ChannelMetrics
    {
        std::chrono::steady_clock::time_point startTime{};
        double startCallMs = 0.0;
        uint64_t successfulFrames = 0;
        uint64_t windowSamples = 0;
        double windowMaximumMs = 0.0;
        bool startupLatencyReported = false;
        std::atomic<uint64_t> videoReceived{0};
        std::atomic<uint64_t> readSamples{0};
        std::atomic<double> readAverageMs{0.0};
        std::atomic<double> readMaximumMs{0.0};
        std::atomic<double> readWindowMaximumMs{0.0};
    };

    std::array<BackendBaseline, 2> baselines_{};
    std::array<ChannelMetrics, 2> metrics_{};
};
