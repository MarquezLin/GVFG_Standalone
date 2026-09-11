#pragma once

#include <gvfg_capture.h>
#include <gvfg_debug.h>
#include <gvfg_preview.h>

class GvfgClient final
{
public:
    GvfgClient() = default;
    ~GvfgClient();
    GvfgClient(const GvfgClient &) = delete;
    GvfgClient &operator=(const GvfgClient &) = delete;

    bool valid() const { return handle_ != nullptr; }
    gvfg_status_t create();
    gvfg_status_t destroy();
    gvfg_status_t openChannel(int device, int channel);
    gvfg_status_t setZeroCopy(int channel, int enabled);
    gvfg_status_t setVideoFormat(int channel, gvfg_pixel_format_t format);
    gvfg_status_t setAudioEnabled(int channel, int enabled);
    gvfg_status_t getAudioFormat(int channel, gvfg_audio_format_t *format) const;
    gvfg_status_t startChannel(int channel);
    gvfg_status_t stopChannel(int channel);
    gvfg_status_t readFrame(int channel, gvfg_frame_t *frame, uint32_t timeoutMs);
    gvfg_status_t releaseFrame(int channel, const gvfg_frame_t *frame);
    gvfg_status_t readAudioFrame(int channel, gvfg_audio_frame_t *frame, uint32_t timeoutMs);
    gvfg_status_t releaseAudioFrame(int channel, const gvfg_audio_frame_t *frame);
    gvfg_status_t pollEvent(int channel, gvfg_event_t *event, uint32_t timeoutMs);
    gvfg_status_t getSignalStatus(int channel, gvfg_signal_status_t *status) const;
    gvfg_status_t getRuntimeInfo(int channel, gvfg_runtime_info_t *info) const;
    gvfg_status_t getLastError(int channel, char *message, uint32_t messageSize) const;
    gvfg_status_t getDebugStats(int channel, gvfg_debug_backend_stats_t *stats) const;

private:
    gvfg_handle handle_ = nullptr;
};

// This namespace is the only application module that calls the public GVFG C
// ABI directly. UI and capture-control code depend on these adapters instead.
namespace gvfg_api
{
int enumerateDevices(gvfg_device_info_t *devices, int capacity);
const char *version();
const char *pixelFormatName(int format);
const char *statusText(gvfg_status_t status);

gvfg_preview_status_t previewCreate(gvfg_preview_handle *handle);
gvfg_preview_status_t previewDestroy(gvfg_preview_handle handle);
gvfg_preview_status_t previewAttachWindow(gvfg_preview_handle handle, void *window);
gvfg_preview_status_t previewPrepare(gvfg_preview_handle handle, int width, int height, int bitDepth);
gvfg_preview_status_t previewRenderFrame(gvfg_preview_handle handle, const gvfg_preview_frame_t *frame);
gvfg_preview_status_t previewClear(gvfg_preview_handle handle);
gvfg_preview_status_t previewWaitIdle(gvfg_preview_handle handle, uint32_t timeoutMs);
gvfg_preview_status_t previewGetInfo(gvfg_preview_handle handle, gvfg_preview_info_t *info);
gvfg_preview_status_t previewGetStats(gvfg_preview_handle handle, gvfg_preview_stats_t *stats);
gvfg_preview_status_t previewGetDeliveryStats(gvfg_preview_handle handle, gvfg_preview_delivery_stats_t *stats);
const char *previewStatusText(gvfg_preview_status_t status);
}
