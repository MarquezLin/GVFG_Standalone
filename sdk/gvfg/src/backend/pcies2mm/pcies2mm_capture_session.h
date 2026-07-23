#pragma once

#include "pcies2mm_backend_types.h"
#include "pcies2mm_device.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
        pcies2mm_status_t close();

        pcies2mm_status_t set_channel(uint32_t channel);
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

    private:
        pcies2mm_status_t open_device(const PcieS2mmDevice &device);
        void close_handles();

        bool read_reg(uint32_t offset, uint32_t &out) const;
        bool write_reg(uint32_t offset, uint32_t value) const;
        bool register_event(uint32_t channelIndex, uint32_t eventType, HANDLE eventHandle);
        void unregister_event(uint32_t channelIndex, uint32_t eventType);
        bool create_and_register_events(uint32_t channelIndex);
        void unregister_events(uint32_t channelIndex);
        void close_event_handles();
        bool get_video_done_index(uint32_t channelIndex, uint32_t &doneIndex) const;
        int get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const;

        void capture_thread_proc();
        void handle_dma_event(uint32_t channel);
        void handle_format_change_event(uint32_t channel);
        void handle_plugin_event(uint32_t channel);
        void handle_unplug_event(uint32_t channel);
        bool resume_capture_from_signal(uint32_t channel);
        bool refresh_stream_from_signal(bool resizeRing);
        void publish_frame(size_t slotIndex, size_t bytes);
        void emit_event(pcies2mm_event_type_t type, uint32_t irqBit, uint32_t irqMask) const;

        pcies2mm_status_t fail(pcies2mm_status_t status, const char *where, DWORD winerr = GetLastError()) const;
        void set_last_error(const std::string &message) const;
        void clear_last_error() const;

        uint32_t active_channel() const;
        uint32_t video_event_mask() const;
        uint32_t video_base() const;
        uint32_t video_irq_mask_bit() const;
        size_t frame_size_bytes() const;

        struct FrameSlot
        {
            std::vector<uint8_t> data;  // Frame byte storage for this ring slot.
            size_t bytes = 0;           // Number of valid bytes read into data.
            uint64_t sequence = 0;      // Monotonic frame sequence assigned on publish.
            bool ready = false;         // True after the data thread publishes a frame.
            bool in_use = false;        // True while writing or while the caller holds the slot.
        };

        std::wstring base_path_;
        std::wstring friendly_name_;

        HANDLE device_ = INVALID_HANDLE_VALUE;
        HANDLE dma_event_ = nullptr;
        HANDLE format_change_event_ = nullptr;
        HANDLE plug_in_event_ = nullptr;
        HANDLE plug_out_event_ = nullptr;

        pcies2mm_stream_desc_t stream_desc_{};
        uint32_t stream_bit_depth_ = 8;
        uint32_t channel_ = 0;
        bool opened_ = false;
        bool configured_ = false;

        std::atomic<bool> running_{false};
        std::atomic<bool> capture_active_{false};
        mutable std::atomic<bool> signal_presence_known_{false};
        mutable std::atomic<bool> signal_present_{false};
        std::thread capture_thread_;

        mutable std::mutex mutex_;
        mutable std::mutex event_callback_mutex_;
        std::condition_variable frame_cv_;
        std::condition_variable data_cv_;
        pcies2mm_event_callback_t event_callback_ = nullptr;
        void *event_callback_user_ = nullptr;
        uint32_t event_mask_filter_ = PCIES2MM_EVENT_MASK_DEFAULT;
        uint32_t pending_events_ = 0;
        std::vector<FrameSlot> frame_ring_;
        size_t next_write_slot_ = 0;
        size_t active_delivery_slot_ = static_cast<size_t>(-1);
        uint64_t latest_sequence_ = 0;
        uint64_t delivered_sequence_ = 0;
        uint64_t wait_timeout_count_ = 0;
        bool stream_error_ = false;
        pcies2mm_stream_stats_t stats_{};
        mutable std::string last_error_;
    };
}
