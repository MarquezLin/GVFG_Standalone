#include "audio_backend_internal.h"
#include "audio_output_internal.h"

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
    };

    uint64_t written_frames = 0, inflight_frames = 0;
    uint64_t played_frames = 0, device_padding = 0;
    uint64_t applied_generation = 0;
    std::atomic<bool> pause_requested{true};
    bool output_paused = false;
    std::atomic<double> rate_ratio{1.0};
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
    std::atomic<uint64_t> buffer_generation{0};
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
        struct ComScope {
            bool initialized;
            ~ComScope() { if (initialized) CoUninitialize(); }
        } comScope{SUCCEEDED(comResult)};
        gvfg_audio_playback_status_t startStatus = GVFG_AUDIO_PLAYBACK_EIO;

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioClockAdjustment> adjustment;
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
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                        AUDCLNT_STREAMFLAGS_RATEADJUST;
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                            kBufferDuration, 0, &wave, nullptr);
                    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
                        startStatus = GVFG_AUDIO_PLAYBACK_EFORMAT;
                    else if (SUCCEEDED(hr))
                        hr = client->GetBufferSize(&bufferFrames);
                    if (SUCCEEDED(hr) && bufferFrames > 0)
                        hr = client->GetService(IID_PPV_ARGS(&render));
                    if (SUCCEEDED(hr) && render)
                        hr = client->GetService(IID_PPV_ARGS(&adjustment));
                    if (SUCCEEDED(hr) && render && !player->pause_requested.load())
                        hr = client->Start();
                    player->output_paused = player->pause_requested.load();
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
            player->applied_generation = player->buffer_generation.load();
        }
        player->ready.notify_one();

        const uint32_t frameBytes = block_align(player->format);
        uint64_t observedGeneration = player->buffer_generation.load(std::memory_order_acquire);
        double appliedRate = 1.0;
        auto updateOutput = [&]() -> bool {
            const bool pause = player->pause_requested.load();
            if (pause != player->output_paused) {
                if (FAILED(pause ? client->Stop() : client->Start())) return false;
                std::lock_guard<std::mutex> lock(player->mutex);
                player->output_paused = pause;
            }
            const double rate = player->rate_ratio.load();
            if (rate != appliedRate && adjustment) {
                if (FAILED(adjustment->SetSampleRate(static_cast<float>(player->format.sample_rate * rate)))) return false;
                appliedRate = rate;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) return false;
            std::lock_guard<std::mutex> lock(player->mutex);
            player->device_padding = padding;
            player->played_frames = player->written_frames > padding ? player->written_frames - padding : 0;
            return true;
        };
        while (startStatus == GVFG_AUDIO_PLAYBACK_OK)
        {
            if (!updateOutput()) { startStatus = GVFG_AUDIO_PLAYBACK_EIO; break; }
            gvfg_audio_player_t::Packet packet;
            {
                std::unique_lock<std::mutex> lock(player->mutex);
                player->queued.wait_for(lock, std::chrono::milliseconds(kPollingSleepMs), [player]
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
                    if (SUCCEEDED(hr) && !player->pause_requested.load())
                        hr = client->Start();
                    {
                        std::lock_guard<std::mutex> stateLock(player->mutex);
                        player->written_frames = player->inflight_frames = player->device_padding = player->played_frames = 0;
                        player->output_paused = player->pause_requested.load();
                        player->applied_generation = player->buffer_generation.load();
                    }
                    if (FAILED(hr))
                        break;
                    observedGeneration = player->buffer_generation.load(std::memory_order_acquire);
                    continue;
                }
                if (player->packets.empty()) continue;
                packet = std::move(player->packets.front());
                player->packets.pop_front();
                player->queued_bytes -= packet.pcm.size();
                player->inflight_frames = packet.pcm.size() / frameBytes;
                observedGeneration = player->buffer_generation.load(std::memory_order_acquire);
            }

            apply_pcm16_gain(packet.pcm, player->volume.load(std::memory_order_acquire));

            uint32_t frameOffset = 0;
            const uint32_t totalFrames = static_cast<uint32_t>(packet.pcm.size() / frameBytes);
            while (frameOffset < totalFrames)
            {
                if (!updateOutput()) { startStatus = GVFG_AUDIO_PLAYBACK_EIO; break; }
                if (player->buffer_generation.load() != observedGeneration) break;
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
                {
                    std::lock_guard<std::mutex> lock(player->mutex);
                    player->written_frames += framesNow;
                    player->inflight_frames -= framesNow;
                }
                frameOffset += framesNow;
            }
        }

        if (client)
            client->Stop();
        adjustment.Reset();
        render.Reset();
        client.Reset();
        device.Reset();
        enumerator.Reset();

        std::lock_guard<std::mutex> lock(player->mutex);
        player->running = false;
    }
}

extern "C"
{
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
        player->written_frames = player->inflight_frames = player->device_padding = player->played_frames = 0;
        player->max_queued_bytes = static_cast<size_t>(format->sample_rate) *
                                   block_align(*format) * kMaxQueuedAudioMs /
                                   kMillisecondsPerSecond;
        player->start_status = GVFG_AUDIO_PLAYBACK_ESTATE;
        player->start_complete = false;
        player->stop_requested = false;
        player->flush_requested = false;
        player->buffer_generation.fetch_add(1, std::memory_order_acq_rel);

        try
        {
            player->worker = std::thread([player] {
                try { playback_worker(player); }
                catch (...) {
                    std::lock_guard<std::mutex> lock(player->mutex);
                    player->running = false;
                    player->start_status = GVFG_AUDIO_PLAYBACK_EIO;
                    player->start_complete = true;
                    player->ready.notify_one();
                }
            });
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

    gvfg_audio_playback_status_t gvfg_audio_player_reset_timeline(gvfg_audio_player player)
    {
        if (!player)
            return GVFG_AUDIO_PLAYBACK_EINVAL;
        {
            std::lock_guard<std::mutex> lock(player->mutex);
            player->packets.clear();
            player->queued_bytes = 0;
            player->flush_requested = true;
            player->buffer_generation.fetch_add(1, std::memory_order_acq_rel);
        }
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

}

// Private output controls: no knowledge of video or synchronization policy.
gvfg_audio_player audio_output_create_managed()
{
    return new (std::nothrow) gvfg_audio_player_t();
}
AudioOutputPosition audio_output_position(gvfg_audio_player p)
{
    if (!p) return {};
    std::lock_guard<std::mutex> lock(p->mutex);
    const auto align = block_align(p->format);
    return {p->played_frames, (align ? p->queued_bytes / align : 0) + p->inflight_frames + p->device_padding,
        p->running, p->output_paused,
        p->flush_requested || p->applied_generation != p->buffer_generation.load()};
}
void audio_output_pause(gvfg_audio_player p, bool paused)
{
    if (p) { p->pause_requested = paused; p->queued.notify_one(); }
}
void audio_output_rate(gvfg_audio_player p, double ratio)
{
    if (p) p->rate_ratio = ratio;
}
