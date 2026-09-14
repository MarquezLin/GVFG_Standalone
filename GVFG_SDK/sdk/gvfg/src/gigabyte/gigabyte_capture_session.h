#pragma once

#include "gigabyte_device.h"
#include "gvfg_error_state.h"
#include "gvfg_capture.h"
#include "gvfg_debug.h"
#include "gvfgsdkapi.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gvfg::internal
{
    struct GigabyteAudioInfo
    {
        uint32_t sample_rate = 0;
        uint32_t channels = 0;
        uint32_t bits_per_sample = 0;
        uint32_t frames_per_second = 0;
        uint32_t frame_bytes = 0;
    };

    using GigabyteEventCallback = void (*)(gvfg_event_type_t event, void *user);

    class GigabyteCaptureSession
    {
    public:
        explicit GigabyteCaptureSession(ChannelErrorState &errorState);
        GigabyteCaptureSession(const GigabyteCaptureSession &) = delete;
        GigabyteCaptureSession &operator=(const GigabyteCaptureSession &) = delete;
        ~GigabyteCaptureSession();

        gvfg_status_t open_device_index(size_t deviceIndex);
        gvfg_status_t open_device_path(const std::wstring &devicePath);
        const std::wstring &device_path() const { return device_path_; }
        gvfg_status_t close();
        gvfg_status_t set_channel(uint32_t channel);
        gvfg_status_t set_zero_copy_enabled(bool enabled);
        bool zero_copy_enabled() const { return zero_copy_enabled_; }
        gvfg_status_t set_audio_enabled(bool enabled);
        gvfg_status_t set_video_format(gvfg_pixel_format_t format);
        gvfg_status_t get_signal_status(gvfg_signal_status_t &out) const;
        gvfg_status_t get_audio_format(GigabyteAudioInfo &out) const;
        gvfg_status_t set_event_callback(GigabyteEventCallback callback, void *user, uint32_t eventMask);
        gvfg_status_t configure_stream();
        gvfg_status_t start_stream();
        gvfg_status_t stop_stream();
        gvfg_status_t wait_frame(uint32_t timeoutMs, gvfg_frame_t &out);
        gvfg_status_t wait_audio(uint32_t timeoutMs, void *destination,
                                    uint32_t destinationCapacity, uint32_t &outBytes);
        gvfg_status_t release_frame();
        void fill_debug_stats(gvfg_debug_backend_stats_t &out) const;
        gvfg_status_t debug_read_register(uint32_t offset, uint32_t &outValue) const;
        gvfg_status_t debug_write_register(uint32_t offset, uint32_t value) const;

    private:
        gvfg_status_t open_device(const GigabyteDevice &device);
        gvfg_status_t ensure_vendor_channel_open() const;
        void close_vendor_channel() const;
        void start_event_monitoring();
        void stop_event_monitoring();
        void event_thread_proc();
        void emit_event(gvfg_event_type_t type) const;
        gvfg_status_t refresh_video_info() const;
        gvfg_status_t from_vendor(GVFG_HRESULT result, const char *operation) const;
        gvfg_status_t reject(gvfg_status_t status, const char *message) const;
        void record_get_frame_timing(double elapsedUs);
        size_t frame_size_bytes() const;

        ChannelErrorState &error_state_;
        std::wstring device_path_;
        mutable HANDLE device_handle_ = INVALID_HANDLE_VALUE;
        mutable PGVFG_CONTEXT context_ = nullptr;
        mutable GVFG_VIDEO_CHN_EVENT events_{};
        mutable HANDLE stop_event_ = nullptr;
        mutable bool events_created_ = false;
        mutable bool channel_open_ = false;
        mutable gvfg_signal_status_t cached_signal_{};
        mutable GVFG_AUDIO_INFO audio_info_{};
        uint32_t channel_ = 0;
        bool configured_ = false;
        bool zero_copy_enabled_ = false;
        bool audio_enabled_ = false;
        std::atomic<bool> running_{false};
        std::atomic<bool> monitoring_{false};
        std::thread event_thread_;
        mutable std::mutex callback_mutex_;
        GigabyteEventCallback event_callback_ = nullptr;
        void *event_callback_user_ = nullptr;
        uint32_t event_mask_filter_ = GVFG_EVENT_MASK_ALL;
        mutable std::mutex state_mutex_;
        std::vector<uint8_t> copy_buffer_;
        bool frame_held_ = false;
        uint64_t frame_id_ = 0;
        uint64_t audio_frames_from_driver_ = 0;
        uint64_t audio_bytes_from_driver_ = 0;
        uint64_t video_event_wakes_ = 0;
        uint64_t audio_event_wakes_ = 0;
        uint64_t extra_audio_event_wakes_ = 0;
        uint64_t wait_timeout_count_ = 0;
        uint64_t get_frame_timing_samples_ = 0;
        uint32_t get_frame_timing_window_samples_ = 0;
        double get_frame_timing_total_us_ = 0.0;
        double get_frame_timing_window_max_us_ = 0.0;
        double get_frame_timing_last_max300_us_ = 0.0;
        double get_frame_timing_lifetime_max_us_ = 0.0;
    };
}
