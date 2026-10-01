#include "gvfg_audio_playback.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

struct gvfg_audio_player_t
{
    struct Packet
    {
        std::vector<uint8_t> pcm;
        uint64_t timestamp_ns = 0;
    };

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable queued;
    std::deque<Packet> packets;
    std::thread worker;
    gvfg_audio_playback_format_t format{};
    gvfg_audio_playback_status_t start_status = GVFG_AUDIO_PLAYBACK_ESTATE;
    size_t queued_bytes = 0;
    size_t max_queued_bytes = 0;
    bool start_complete = false;
    bool running = false;
    bool stop_requested = false;
    bool flush_requested = false;
    std::atomic<uint64_t> latest_video_timestamp_ns{0};
    std::atomic<uint64_t> timeline_generation{0};
    std::atomic<float> volume{1.0f};
};

namespace
{
    constexpr DWORD kPollingSleepMs = 2;
    constexpr uint32_t kMaxQueuedAudioMs = 500;
    constexpr uint32_t kMillisecondsPerSecond = 1000;
    constexpr float kMinimumVolume = 0.0f;
    constexpr float kMaximumVolume = 2.0f;

    bool valid_format(const gvfg_audio_playback_format_t &format)
    {
        return format.sample_rate > 0 && format.channels > 0 &&
               format.bits_per_sample == 16;
    }

    uint32_t block_align(const gvfg_audio_playback_format_t &format)
    {
        return static_cast<uint32_t>(format.channels) *
               (static_cast<uint32_t>(format.bits_per_sample) / 8u);
    }

    void apply_pcm16_gain(std::vector<uint8_t> &pcm, float gain)
    {
        if (gain == 1.0f)
            return;
        for (size_t offset = 0; offset + sizeof(int16_t) <= pcm.size(); offset += sizeof(int16_t))
        {
            int16_t sample = 0;
            std::memcpy(&sample, pcm.data() + offset, sizeof(sample));
            const int amplified = static_cast<int>(std::lround(static_cast<float>(sample) * gain));
            sample = static_cast<int16_t>((std::max)(static_cast<int>((std::numeric_limits<int16_t>::min)()),
                                                     (std::min)(static_cast<int>((std::numeric_limits<int16_t>::max)()),
                                                                amplified)));
            std::memcpy(pcm.data() + offset, &sample, sizeof(sample));
        }
    }

    void playback_worker(gvfg_audio_player_t *player)
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        gvfg_audio_playback_status_t startStatus = GVFG_AUDIO_PLAYBACK_EIO;

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        UINT32 bufferFrames = 0;

        if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
        {
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                          CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr))
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr) || !device)
            {
                startStatus = GVFG_AUDIO_PLAYBACK_ENODEV;
            }
            else
            {
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void **>(client.GetAddressOf()));
                if (SUCCEEDED(hr) && client)
                {
                    WAVEFORMATEX wave{};
                    wave.wFormatTag = WAVE_FORMAT_PCM;
                    wave.nChannels = player->format.channels;
                    wave.nSamplesPerSec = player->format.sample_rate;
                    wave.wBitsPerSample = player->format.bits_per_sample;
                    wave.nBlockAlign = static_cast<WORD>(block_align(player->format));
                    wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;

                    constexpr REFERENCE_TIME kBufferDuration = 1000000; // 100 ms
                    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                            kBufferDuration, 0, &wave, nullptr);
                    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
                        startStatus = GVFG_AUDIO_PLAYBACK_EFORMAT;
                    else if (SUCCEEDED(hr))
                        hr = client->GetBufferSize(&bufferFrames);
                    if (SUCCEEDED(hr) && bufferFrames > 0)
                        hr = client->GetService(IID_PPV_ARGS(&render));
                    if (SUCCEEDED(hr) && render)
                        hr = client->Start();
                    if (SUCCEEDED(hr) && render)
                        startStatus = GVFG_AUDIO_PLAYBACK_OK;
                    else if (startStatus != GVFG_AUDIO_PLAYBACK_EFORMAT)
                        startStatus = GVFG_AUDIO_PLAYBACK_EIO;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(player->mutex);
            player->start_status = startStatus;
            player->start_complete = true;
            player->running = startStatus == GVFG_AUDIO_PLAYBACK_OK;
        }
        player->ready.notify_one();

        constexpr int64_t kLeadLimitNs = 40'000'000;
        constexpr int64_t kLagDropNs = 80'000'000;
        constexpr auto kMaxHold = std::chrono::milliseconds(100);
        const uint32_t frameBytes = block_align(player->format);
        uint64_t observedGeneration = player->timeline_generation.load(std::memory_order_acquire);
        bool syncBaseValid = false;
        int64_t syncBaseOffsetNs = 0;
        while (startStatus == GVFG_AUDIO_PLAYBACK_OK)
        {
            gvfg_audio_player_t::Packet packet;
            {
                std::unique_lock<std::mutex> lock(player->mutex);
                player->queued.wait(lock, [player]
                                    { return player->stop_requested || player->flush_requested ||
                                             !player->packets.empty(); });
                if (player->stop_requested)
                    break;
                if (player->flush_requested)
                {
                    player->flush_requested = false;
                    lock.unlock();
                    HRESULT hr = client->Stop();
                    if (SUCCEEDED(hr))
                        hr = client->Reset();
                    if (SUCCEEDED(hr))
                        hr = client->Start();
                    if (FAILED(hr))
                        break;
                    observedGeneration = player->timeline_generation.load(std::memory_order_acquire);
                    syncBaseValid = false;
                    continue;
                }
                packet = std::move(player->packets.front());
                player->packets.pop_front();
                player->queued_bytes -= packet.pcm.size();
            }

            const uint64_t generation = player->timeline_generation.load(std::memory_order_acquire);
            if (generation != observedGeneration)
            {
                observedGeneration = generation;
                syncBaseValid = false;
            }
            uint64_t videoTimestampNs = player->latest_video_timestamp_ns.load(std::memory_order_acquire);
            if (packet.timestamp_ns != 0 && videoTimestampNs != 0)
            {
                const int64_t rawOffsetNs = static_cast<int64_t>(packet.timestamp_ns) -
                                            static_cast<int64_t>(videoTimestampNs);
                if (!syncBaseValid)
                {
                    syncBaseOffsetNs = rawOffsetNs;
                    syncBaseValid = true;
                }
                int64_t driftNs = rawOffsetNs - syncBaseOffsetNs;
                if (driftNs < -kLagDropNs)
                    continue;
                const auto holdDeadline = std::chrono::steady_clock::now() + kMaxHold;
                while (driftNs > kLeadLimitNs &&
                       std::chrono::steady_clock::now() < holdDeadline)
                {
                    {
                        std::lock_guard<std::mutex> lock(player->mutex);
                        if (player->stop_requested)
                            break;
                    }
                    if (player->timeline_generation.load(std::memory_order_acquire) != observedGeneration)
                    {
                        syncBaseValid = false;
                        break;
                    }
                    Sleep(kPollingSleepMs);
                    videoTimestampNs = player->latest_video_timestamp_ns.load(std::memory_order_acquire);
                    driftNs = static_cast<int64_t>(packet.timestamp_ns) -
                              static_cast<int64_t>(videoTimestampNs) - syncBaseOffsetNs;
                }
                if (!syncBaseValid || driftNs > kLeadLimitNs)
                    continue;
            }

            apply_pcm16_gain(packet.pcm, player->volume.load(std::memory_order_acquire));

            uint32_t frameOffset = 0;
            const uint32_t totalFrames = static_cast<uint32_t>(packet.pcm.size() / frameBytes);
            while (frameOffset < totalFrames)
            {
                {
                    std::lock_guard<std::mutex> lock(player->mutex);
                    if (player->stop_requested)
                        break;
                }

                UINT32 padding = 0;
                HRESULT hr = client->GetCurrentPadding(&padding);
                if (FAILED(hr))
                {
                    startStatus = GVFG_AUDIO_PLAYBACK_EIO;
                    break;
                }
                const UINT32 available = padding < bufferFrames ? bufferFrames - padding : 0;
                if (available == 0)
                {
                    Sleep(kPollingSleepMs);
                    continue;
                }

                const UINT32 framesNow = (std::min)(available, totalFrames - frameOffset);
                BYTE *destination = nullptr;
                hr = render->GetBuffer(framesNow, &destination);
                if (FAILED(hr) || !destination)
                {
                    startStatus = GVFG_AUDIO_PLAYBACK_EIO;
                    break;
                }
                std::memcpy(destination,
                            packet.pcm.data() + static_cast<size_t>(frameOffset) * frameBytes,
                            static_cast<size_t>(framesNow) * frameBytes);
                hr = render->ReleaseBuffer(framesNow, 0);
                if (FAILED(hr))
                {
                    startStatus = GVFG_AUDIO_PLAYBACK_EIO;
                    break;
                }
                frameOffset += framesNow;
            }
        }

        if (client)
            client->Stop();
        render.Reset();
        client.Reset();
        device.Reset();
        enumerator.Reset();
        if (uninitializeCom)
            CoUninitialize();

        std::lock_guard<std::mutex> lock(player->mutex);
        player->running = false;
    }
}

extern "C"
{
    gvfg_audio_playback_status_t gvfg_audio_player_create(gvfg_audio_player *out_player)
    {
        if (!out_player)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        *out_player = new (std::nothrow) gvfg_audio_player_t();
        return *out_player ? GVFG_AUDIO_PLAYBACK_OK : GVFG_AUDIO_PLAYBACK_EIO;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_start(
        gvfg_audio_player player, const gvfg_audio_playback_format_t *format)
    {
        if (!player || !format || !valid_format(*format))
            return GVFG_AUDIO_PLAYBACK_EINVAL;

        std::unique_lock<std::mutex> lock(player->mutex);
        if (player->worker.joinable() || player->running)
            return GVFG_AUDIO_PLAYBACK_ESTATE;

        player->format = *format;
        player->packets.clear();
        player->queued_bytes = 0;
        player->max_queued_bytes = static_cast<size_t>(format->sample_rate) *
                                   block_align(*format) * kMaxQueuedAudioMs /
                                   kMillisecondsPerSecond;
        player->start_status = GVFG_AUDIO_PLAYBACK_ESTATE;
        player->start_complete = false;
        player->stop_requested = false;
        player->flush_requested = false;
        player->latest_video_timestamp_ns.store(0, std::memory_order_release);
        player->timeline_generation.fetch_add(1, std::memory_order_acq_rel);

        try
        {
            player->worker = std::thread(playback_worker, player);
        }
        catch (...)
        {
            return GVFG_AUDIO_PLAYBACK_EIO;
        }

        player->ready.wait(lock, [player]
                           { return player->start_complete; });
        const gvfg_audio_playback_status_t status = player->start_status;
        lock.unlock();
        if (status != GVFG_AUDIO_PLAYBACK_OK && player->worker.joinable())
            player->worker.join();
        return status;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_write(
        gvfg_audio_player player, const void *data, uint32_t data_size)
    {
        return gvfg_audio_player_write_timed(player, data, data_size, 0);
    }

    gvfg_audio_playback_status_t gvfg_audio_player_write_timed(
        gvfg_audio_player player, const void *data, uint32_t data_size,
        uint64_t audio_timestamp_ns)
    {
        if (!player || !data || data_size == 0)
            return GVFG_AUDIO_PLAYBACK_EINVAL;

        std::lock_guard<std::mutex> lock(player->mutex);
        const uint32_t frameBytes = block_align(player->format);
        if (!player->running || player->stop_requested)
            return GVFG_AUDIO_PLAYBACK_ESTATE;
        if (frameBytes == 0 || data_size % frameBytes != 0)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        if (data_size > player->max_queued_bytes ||
            player->queued_bytes > player->max_queued_bytes - data_size)
            return GVFG_AUDIO_PLAYBACK_EQUEUE_FULL;

        try
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            gvfg_audio_player_t::Packet packet;
            packet.pcm.assign(bytes, bytes + data_size);
            packet.timestamp_ns = audio_timestamp_ns;
            player->packets.push_back(std::move(packet));
        }
        catch (...)
        {
            return GVFG_AUDIO_PLAYBACK_EIO;
        }
        player->queued_bytes += data_size;
        player->queued.notify_one();
        return GVFG_AUDIO_PLAYBACK_OK;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_update_video_timestamp(
        gvfg_audio_player player, uint64_t video_timestamp_ns)
    {
        if (!player)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        player->latest_video_timestamp_ns.store(video_timestamp_ns, std::memory_order_release);
        return GVFG_AUDIO_PLAYBACK_OK;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_reset_timeline(gvfg_audio_player player)
    {
        if (!player)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        {
            std::lock_guard<std::mutex> lock(player->mutex);
            player->packets.clear();
            player->queued_bytes = 0;
            player->flush_requested = true;
        }
        player->latest_video_timestamp_ns.store(0, std::memory_order_release);
        player->timeline_generation.fetch_add(1, std::memory_order_acq_rel);
        player->queued.notify_one();
        return GVFG_AUDIO_PLAYBACK_OK;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_set_volume(
        gvfg_audio_player player, float volume)
    {
        if (!player || !std::isfinite(volume) ||
            volume < kMinimumVolume || volume > kMaximumVolume)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        player->volume.store(volume, std::memory_order_release);
        return GVFG_AUDIO_PLAYBACK_OK;
    }

    gvfg_audio_playback_status_t gvfg_audio_player_stop(gvfg_audio_player player)
    {
        if (!player)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        {
            std::lock_guard<std::mutex> lock(player->mutex);
            player->stop_requested = true;
            player->packets.clear();
            player->queued_bytes = 0;
        }
        player->queued.notify_one();
        if (player->worker.joinable())
            player->worker.join();
        return GVFG_AUDIO_PLAYBACK_OK;
    }

    void gvfg_audio_player_destroy(gvfg_audio_player player)
    {
        if (!player)
            return;
        gvfg_audio_player_stop(player);
        delete player;
    }

    const char *gvfg_audio_player_strerror(gvfg_audio_playback_status_t status)
    {
        switch (status)
        {
        case GVFG_AUDIO_PLAYBACK_OK:
            return "ok";
        case GVFG_AUDIO_PLAYBACK_EINVAL:
            return "invalid argument";
        case GVFG_AUDIO_PLAYBACK_ESTATE:
            return "invalid state";
        case GVFG_AUDIO_PLAYBACK_ENODEV:
            return "default audio output unavailable";
        case GVFG_AUDIO_PLAYBACK_EFORMAT:
            return "PCM format unsupported";
        case GVFG_AUDIO_PLAYBACK_EQUEUE_FULL:
            return "playback queue full";
        case GVFG_AUDIO_PLAYBACK_EIO:
            return "audio playback failure";
        default:
            return "unknown audio playback status";
        }
    }
}
