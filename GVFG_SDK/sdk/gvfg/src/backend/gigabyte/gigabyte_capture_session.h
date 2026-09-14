#pragma once

#include "gigabyte_backend_types.h"
#include "gigabyte_device.h"
#include "gvfg_error_state.h"
#include "gvfgsdkapi.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gvfg::internal
{
    struct PcieS2mmDeviceConnection
    {
        std::wstring interface_path;
    };

    class PcieS2mmCaptureSession
    {
    public:
        explicit PcieS2mmCaptureSession(ChannelErrorState &errorState);
        PcieS2mmCaptureSession(const PcieS2mmCaptureSession &) = delete;
        PcieS2mmCaptureSession &operator=(const PcieS2mmCaptureSession &) = delete;
        ~PcieS2mmCaptureSession();

        pcies2mm_status_t open_device_index(size_t deviceIndex);
        pcies2mm_status_t open_device_connection(const std::shared_ptr<PcieS2mmDeviceConnection> &connection);
        std::shared_ptr<PcieS2mmDeviceConnection> device_connection() const { return device_connection_; }
        pcies2mm_status_t close();
        pcies2mm_status_t set_channel(uint32_t channel);
        pcies2mm_status_t set_zero_copy_enabled(bool enabled);
        bool zero_copy_enabled() const { return zero_copy_enabled_; }
        pcies2mm_status_t set_audio_enabled(bool enabled);
        pcies2mm_status_t set_video_format(pcies2mm_pixel_format_t format);
        pcies2mm_status_t get_signal_status(pcies2mm_signal_status_t &out) const;
        pcies2mm_status_t get_audio_format(pcies2mm_audio_format_t &out) const;
        pcies2mm_status_t set_event_callback(pcies2mm_event_callback_t callback, void *user, uint32_t eventMask);
        pcies2mm_status_t configure_stream(const pcies2mm_stream_desc_t &desc);
        pcies2mm_status_t start_stream();
        pcies2mm_status_t stop_stream();
        pcies2mm_status_t wait_frame(uint32_t timeoutMs, pcies2mm_frame_t &out);
        pcies2mm_status_t wait_audio(uint32_t timeoutMs, void *destination,
                                    uint32_t destinationCapacity, uint32_t &outBytes);
        pcies2mm_status_t release_frame(const pcies2mm_frame_t &frame);
        void get_debug_stats(pcies2mm_stream_stats_t &outStats, uint64_t &outWaitTimeouts,
                             pcies2mm_debug_state_t &outDebugState) const;
        pcies2mm_status_t debug_read_register(uint32_t offset, uint32_t &outValue) const;
        pcies2mm_status_t debug_write_register(uint32_t offset, uint32_t value) const;

    private:
        pcies2mm_status_t open_device(const GigabyteDevice &device);
        pcies2mm_status_t ensure_vendor_channel_open() const;
        void close_vendor_channel() const;
        void start_event_monitoring();
        void stop_event_monitoring();
        void event_thread_proc();
        void emit_event(pcies2mm_event_type_t type) const;
        pcies2mm_status_t refresh_video_info() const;
        pcies2mm_status_t from_vendor(GVFG_HRESULT result, const char *operation) const;
        pcies2mm_status_t reject(pcies2mm_status_t status, const char *message) const;
        void record_get_frame_timing(double elapsedUs);
        size_t frame_size_bytes() const;

        ChannelErrorState &error_state_;
        std::shared_ptr<PcieS2mmDeviceConnection> device_connection_;
        mutable HANDLE device_handle_ = INVALID_HANDLE_VALUE;
        mutable PGVFG_CONTEXT context_ = nullptr;
        mutable GVFG_VIDEO_CHN_EVENT events_{};
        mutable HANDLE stop_event_ = nullptr;
        mutable bool events_created_ = false;
        mutable bool channel_open_ = false;
        pcies2mm_stream_desc_t stream_desc_{};
        mutable pcies2mm_signal_status_t cached_signal_{};
        mutable GVFG_AUDIO_INFO audio_info_{};
        uint32_t channel_ = 0;
        bool configured_ = false;
        bool zero_copy_enabled_ = false;
        bool audio_enabled_ = false;
        std::atomic<bool> running_{false};
        std::atomic<bool> monitoring_{false};
        std::thread event_thread_;
        mutable std::mutex callback_mutex_;
        pcies2mm_event_callback_t event_callback_ = nullptr;
        void *event_callback_user_ = nullptr;
        uint32_t event_mask_filter_ = PCIES2MM_EVENT_MASK_DEFAULT;
        mutable std::mutex state_mutex_;
        std::vector<uint8_t> copy_buffer_;
        pcies2mm_frame_t held_frame_{};
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
