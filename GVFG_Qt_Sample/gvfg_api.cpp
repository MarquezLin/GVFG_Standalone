#include "gvfg_api.h"

GvfgClient::~GvfgClient() { destroy(); }
gvfg_status_t GvfgClient::create() { return handle_ ? GVFG_OK : gvfg_create(&handle_); }
gvfg_status_t GvfgClient::destroy()
{
    if (!handle_) return GVFG_OK;
    const gvfg_status_t status = gvfg_destroy(handle_);
    handle_ = nullptr;
    return status;
}
gvfg_status_t GvfgClient::openChannel(int device, int channel) { return gvfg_open_channel(handle_, device, channel); }
gvfg_status_t GvfgClient::setZeroCopy(int channel, int enabled) { return gvfg_set_channel_zero_copy_enabled(handle_, channel, enabled); }
gvfg_status_t GvfgClient::setVideoFormat(int channel, gvfg_pixel_format_t format) { return gvfg_set_channel_video_format(handle_, channel, format); }
gvfg_status_t GvfgClient::setAudioEnabled(int channel, int enabled) { return gvfg_set_channel_audio_enabled(handle_, channel, enabled); }
gvfg_status_t GvfgClient::getAudioFormat(int channel, gvfg_audio_format_t *format) const { return gvfg_get_channel_audio_format(handle_, channel, format); }
gvfg_status_t GvfgClient::startChannel(int channel) { return gvfg_start_channel(handle_, channel); }
gvfg_status_t GvfgClient::stopChannel(int channel) { return gvfg_stop_channel(handle_, channel); }
gvfg_status_t GvfgClient::readFrame(int channel, gvfg_frame_t *frame, uint32_t timeoutMs) { return gvfg_read_channel_frame(handle_, channel, frame, timeoutMs); }
gvfg_status_t GvfgClient::releaseFrame(int channel, const gvfg_frame_t *frame) { return gvfg_release_channel_frame(handle_, channel, frame); }
gvfg_status_t GvfgClient::readAudioFrame(int channel, gvfg_audio_frame_t *frame, uint32_t timeoutMs) { return gvfg_read_channel_audio_frame(handle_, channel, frame, timeoutMs); }
gvfg_status_t GvfgClient::releaseAudioFrame(int channel, const gvfg_audio_frame_t *frame) { return gvfg_release_channel_audio_frame(handle_, channel, frame); }
gvfg_status_t GvfgClient::pollEvent(int channel, gvfg_event_t *event, uint32_t timeoutMs) { return gvfg_poll_channel_event(handle_, channel, event, timeoutMs); }
gvfg_status_t GvfgClient::getSignalStatus(int channel, gvfg_signal_status_t *status) const { return gvfg_get_channel_signal_status(handle_, channel, status); }
gvfg_status_t GvfgClient::getRuntimeInfo(int channel, gvfg_runtime_info_t *info) const { return gvfg_get_channel_runtime_info(handle_, channel, info); }
gvfg_status_t GvfgClient::getLastError(int channel, char *message, uint32_t messageSize) const { return gvfg_get_channel_last_error_detail(handle_, channel, message, messageSize); }
gvfg_status_t GvfgClient::getDebugStats(int channel, gvfg_debug_backend_stats_t *stats) const { return gvfg_debug_get_channel_backend_stats(handle_, channel, stats); }

namespace gvfg_api
{
int enumerateDevices(gvfg_device_info_t *devices, int capacity) { return gvfg_enumerate_devices(devices, capacity); }
const char *version() { return gvfg_get_version(); }
const char *pixelFormatName(int format) { return gvfg_pixel_format_name(format); }
const char *statusText(gvfg_status_t status) { return gvfg_strerror(status); }

gvfg_preview_status_t previewCreate(gvfg_preview_handle *handle) { return gvfg_preview_create(handle); }
gvfg_preview_status_t previewDestroy(gvfg_preview_handle handle) { return gvfg_preview_destroy(handle); }
gvfg_preview_status_t previewAttachWindow(gvfg_preview_handle handle, void *window) { return gvfg_preview_attach_window(handle, window); }
gvfg_preview_status_t previewPrepare(gvfg_preview_handle handle, int width, int height, int bitDepth) { return gvfg_preview_prepare(handle, width, height, bitDepth); }
gvfg_preview_status_t previewRenderFrame(gvfg_preview_handle handle, const gvfg_preview_frame_t *frame) { return gvfg_preview_render_frame(handle, frame); }
gvfg_preview_status_t previewClear(gvfg_preview_handle handle) { return gvfg_preview_clear(handle); }
gvfg_preview_status_t previewWaitIdle(gvfg_preview_handle handle, uint32_t timeoutMs) { return gvfg_preview_wait_idle(handle, timeoutMs); }
gvfg_preview_status_t previewGetInfo(gvfg_preview_handle handle, gvfg_preview_info_t *info) { return gvfg_preview_get_info(handle, info); }
gvfg_preview_status_t previewGetStats(gvfg_preview_handle handle, gvfg_preview_stats_t *stats) { return gvfg_preview_get_stats(handle, stats); }
gvfg_preview_status_t previewGetDeliveryStats(gvfg_preview_handle handle, gvfg_preview_delivery_stats_t *stats) { return gvfg_preview_get_delivery_stats(handle, stats); }
const char *previewStatusText(gvfg_preview_status_t status) { return gvfg_preview_strerror(status); }
}
