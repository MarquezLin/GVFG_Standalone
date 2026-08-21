#pragma once

#include "pcies2mm_backend_types.h"
#include "pcies2mm_device.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gvfg::internal
{
    class PcieS2mmCaptureSession
    {
    public:
        PcieS2mmCaptureSession();
        PcieS2mmCaptureSession(const PcieS2mmCaptureSession &) = delete;
        PcieS2mmCaptureSession &operator=(const PcieS2mmCaptureSession &) = delete;
        ~PcieS2mmCaptureSession();

        pcies2mm_status_t open_device_index(size_t deviceIndex);
        pcies2mm_status_t open_shared_device(const PcieS2mmCaptureSession &source,
                                             uint32_t channel);
        pcies2mm_status_t close();

        pcies2mm_status_t set_channel(uint32_t channel);
        pcies2mm_status_t set_zero_copy_enabled(bool enabled);
        bool zero_copy_enabled() const { return zero_copy_enabled_; }
        pcies2mm_status_t set_video_format(pcies2mm_pixel_format_t format);
        pcies2mm_status_t get_signal_status(pcies2mm_signal_status_t &out) const;

        pcies2mm_status_t set_event_callback(pcies2mm_event_callback_t callback, void *user, uint32_t eventMask);
        pcies2mm_status_t configure_stream(const pcies2mm_stream_desc_t &desc);
        pcies2mm_status_t start_stream();
        pcies2mm_status_t stop_stream();
        pcies2mm_status_t wait_frame(uint32_t timeoutMs, pcies2mm_frame_t &out);
        pcies2mm_status_t release_frame(const pcies2mm_frame_t &frame);

        const char *last_error() const;
        void get_debug_stats(pcies2mm_stream_stats_t &outStats,
                             uint64_t &outWaitTimeouts,
                             pcies2mm_debug_state_t &outDebugState) const;
        pcies2mm_status_t debug_read_register(uint32_t offset, uint32_t &outValue) const;
        pcies2mm_status_t debug_write_register(uint32_t offset, uint32_t value) const;

    private:
        struct SharedDevice
        {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~SharedDevice();
        };

        pcies2mm_status_t open_device(const PcieS2mmDevice &device);
        void close_handles();

        bool read_reg(uint32_t offset, uint32_t &out) const;
        bool write_reg(uint32_t offset, uint32_t value) const;
        bool start_video(uint32_t channelIndex) const;
        bool stop_video(uint32_t channelIndex) const;
        bool acquire_zero_copy_frame(uint32_t channelIndex, const uint8_t *&outData) const;
        bool release_zero_copy_frame(uint32_t channelIndex) const;
        pcies2mm_status_t release_all_zero_copy_frames(bool includeInUse);
        bool register_event(uint32_t channelIndex, uint32_t eventType, HANDLE eventHandle);
        void unregister_event(uint32_t channelIndex, uint32_t eventType);
        bool create_and_register_events(uint32_t channelIndex);
        bool start_event_monitoring();
        void stop_event_monitoring();
        void unregister_events(uint32_t channelIndex);
        void close_event_handles();
        int get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const;

        void capture_thread_proc();
        void handle_format_change_event(uint32_t channel);
        void handle_plugin_event(uint32_t channel);
        void handle_unplug_event(uint32_t channel);
        bool refresh_stream_from_registers(bool resizeBuffer);
        void emit_event(pcies2mm_event_type_t type) const;

        pcies2mm_status_t fail(pcies2mm_status_t status, const char *where, DWORD winerr = GetLastError()) const;
        void set_last_error(const std::string &message) const;
        void clear_last_error() const;

        uint32_t active_channel() const;
        uint32_t video_event_mask() const;
        uint32_t video_base() const;
        uint32_t video_irq_mask_bit() const;
        size_t frame_size_bytes() const;
        void record_get_frame_timing(double elapsedUs);
        void record_event_wait_timing(double elapsedUs);
        void record_sdk_processing_timing(double elapsedUs);

        std::wstring base_path_;
        std::wstring friendly_name_;

        HANDLE device_ = INVALID_HANDLE_VALUE;
        std::shared_ptr<SharedDevice> shared_device_;
        HANDLE dma_event_ = nullptr;
        HANDLE format_change_event_ = nullptr;
        HANDLE plug_in_event_ = nullptr;
        HANDLE plug_out_event_ = nullptr;

        pcies2mm_stream_desc_t stream_desc_{};
        uint32_t stream_bit_depth_ = 8;
        uint32_t channel_ = 0;
        bool opened_ = false;
        bool configured_ = false;
        bool zero_copy_enabled_ = false;

        std::atomic<bool> running_{false};
        std::atomic<bool> monitoring_{false};
        std::atomic<bool> capture_active_{false};
        std::atomic<bool> reader_ready_{false};
        std::atomic<bool> signal_probe_active_{false};
        std::atomic<bool> stream_ready_pending_{false};
        mutable std::atomic<bool> signal_presence_known_{false};
        mutable std::atomic<bool> signal_present_{false};
        mutable bool signal_metadata_valid_ = false;
        mutable pcies2mm_signal_status_t cached_signal_{};
        std::thread capture_thread_;

        mutable std::mutex mutex_;
        std::condition_variable read_finished_cv_;
        mutable std::mutex event_callback_mutex_;
        pcies2mm_event_callback_t event_callback_ = nullptr;
        void *event_callback_user_ = nullptr;
        uint32_t event_mask_filter_ = PCIES2MM_EVENT_MASK_DEFAULT;
        uint64_t latest_sequence_ = 0;
        uint64_t delivered_sequence_ = 0;
        uint64_t wait_timeout_count_ = 0;
        uint64_t get_frame_timing_samples_ = 0;
        uint32_t get_frame_timing_window_samples_ = 0;
        double get_frame_timing_total_us_ = 0.0;
        double get_frame_timing_window_max_us_ = 0.0;
        double get_frame_timing_last_max300_us_ = 0.0;
        double get_frame_timing_lifetime_max_us_ = 0.0;
        uint64_t event_wait_timing_samples_ = 0;
        uint32_t event_wait_timing_window_samples_ = 0;
        double event_wait_timing_total_us_ = 0.0;
        double event_wait_timing_window_max_us_ = 0.0;
        double event_wait_timing_last_max300_us_ = 0.0;
        double event_wait_timing_lifetime_max_us_ = 0.0;
        uint64_t sdk_processing_timing_samples_ = 0;
        uint32_t sdk_processing_timing_window_samples_ = 0;
        double sdk_processing_timing_total_us_ = 0.0;
        double sdk_processing_timing_window_max_us_ = 0.0;
        double sdk_processing_timing_last_max300_us_ = 0.0;
        double sdk_processing_timing_lifetime_max_us_ = 0.0;
        std::vector<uint8_t> copy_buffer_;
        pcies2mm_frame_t held_frame_{};
        bool frame_held_ = false;
        bool read_in_progress_ = false;
        std::chrono::steady_clock::time_point active_delivery_started_{};
        bool stream_error_ = false;
        pcies2mm_stream_stats_t stats_{};
        mutable std::string last_error_;
    };
}
