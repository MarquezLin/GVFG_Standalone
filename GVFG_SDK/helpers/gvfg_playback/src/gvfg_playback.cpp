#include "gvfg_playback.h"
#include "audio_output_internal.h"
#include "preview_output_internal.h"
#include "playback_timing.h"
#include "signal_card.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

using namespace gvfg_playback_detail;
namespace {
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// HWND owner may be joining a worker inside DXGI. Pump only synchronous sent
// messages, matching the preview helper's teardown contract.
void service_messages() {
    MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
    MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}
}

struct gvfg_playback_t {
    struct Video { gvfg_playback_video_frame_t frame{}; std::vector<uint8_t> data; };
    struct Audio { uint64_t pts = 0, end = 0; std::vector<uint8_t> data; };
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::atomic<bool> exited{true};
    bool stopping = false, active = false;
    bool sync_enabled = true;
    bool audio_output_failed = false;
    float gain = 1.0f;
    void *hwnd = nullptr;
    SignalCard signal_card;
    gvfg_preview_handle preview = nullptr;
    gvfg_audio_player audio = nullptr;
    gvfg_playback_config_t config{};
    gvfg_playback_stats_t stats{};
    std::deque<Video> videos;
    std::deque<Audio> audios;
    uint64_t video_bytes = 0, audio_bytes = 0;
    uint64_t first_video = 0, last_video = 0, first_audio = 0, audio_end = 0;
    uint64_t last_audio_delivery = 0;
    uint64_t start_wall = 0, buffering_since = 0;
    uint64_t last_video_arrival = 0, last_audio_arrival = 0;
    uint64_t stable_since = 0;
    RecoveryRate recovery_rate;
    bool fallback_audio_primed = false;
    uint64_t audio_wait_since = 0;
    AudioSourceClock audio_source_clock;
    Clock external, video_clock;
    DriftFilter drift;
    AudioTimeline audio_timeline;
    void drop_audio() {
        stats.audio_dropped_frames += audio_bytes / (config.audio_format.channels * 2);
        audios.clear(); audio_bytes = 0;
    }
    void reset_audio() {
        if (!has_audio()) return;
        drop_audio(); audio_source_clock = {}; audio_timeline = {};
        first_audio = audio_end = 0;
        audio_output_pause(audio, true); audio_output_rate(audio, 1.0);
        if (gvfg_audio_player_reset_timeline(audio) != GVFG_AUDIO_PLAYBACK_OK)
            fail_audio();
    }
    void degrade(uint64_t now, bool count_fallback = true) {
        stable_since = 0; recovery_rate = {};
        if (stats.sync_degraded) return;
        stats.sync_degraded = 1;
        if (count_fallback) ++stats.sync_fallbacks;
        stats.buffering = 0;
        const uint64_t live = last_video ? last_video : (audio_end ? audio_end : now);
        external.set(live, now, false); video_clock.set(live, now, false);
        drift = {}; stats.audio_rate_ratio = 1;
        fallback_audio_primed = false; audio_wait_since = 0;
        reset_audio();
    }
    bool has_video() const { return (config.mode & GVFG_PLAYBACK_VIDEO_ONLY) != 0; }
    bool has_audio() const { return (config.mode & GVFG_PLAYBACK_AUDIO_ONLY) != 0 && !audio_output_failed; }
    void fail_audio() {
        if (!has_video()) { fail(GVFG_PLAYBACK_EOUTPUT); return; }
        if (audio_output_failed) return;
        audio_output_failed = true;
        drop_audio();
        gvfg_audio_player_destroy(audio); audio = nullptr;
        audio_source_clock = {}; audio_timeline = {};
        first_audio = audio_end = 0;
        stats.audio_timestamp_ns = 0; stats.av_difference_ns = 0;
        stats.audio_rate_ratio = 1;
        // Retain video packets and move off a failed audio master. Monitoring
        // remains disabled across resets; a fresh handle may retry the device.
        degrade(now_ns());
    }
    void fail(gvfg_playback_status_t error) {
        stats.last_error = error; stats.buffering = 1;
        audio_output_pause(audio, true);
    }
    uint64_t master_time(uint64_t now) {
        if (stats.sync_degraded) return external.get(now);
        if (config.master == GVFG_PLAYBACK_MASTER_AUDIO && audio) {
            const auto pos = audio_output_position(audio);
            const auto pts = audio_timeline.position(pos.played_frames);
            if (pts) return pts;
        }
        return config.master == GVFG_PLAYBACK_MASTER_VIDEO ? video_clock.get(now) : external.get(now);
    }
    void retire_late_video(uint64_t master) {
        // FFplay-style automatic dropping for external/audio master. Keep the
        // newest due frame, never a future frame, and account every retirement.
        if (stats.buffering || (!stats.sync_degraded && config.master == GVFG_PLAYBACK_MASTER_VIDEO)) return;
        while (videos.size() > 1 && (stats.sync_degraded || videos[1].frame.timestamp_ns <= master)) {
            video_bytes -= videos.front().data.size(); videos.pop_front();
            ++stats.video_processed; ++stats.video_dropped;
        }
    }
    void loop();
};

void gvfg_playback_t::loop() {
    uint64_t last_control = 0;
    bool started = false, preview_ready = false;
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping) break;
        if (stats.last_error != GVFG_PLAYBACK_OK) {
            changed.wait_for(lock, std::chrono::milliseconds(2)); continue;
        }
        // Warm up the GPU while outputs are still paused. Shader/resource
        // creation must not consume the small running audio lead.
        if (!preview_ready && hwnd && !videos.empty()) {
            const auto image = videos.front().frame.image;
            lock.unlock();
            const bool ready = gvfg_preview_prepare(preview, image.width, image.height, image.bit_depth) == GVFG_PREVIEW_OK;
            lock.lock();
            if (!ready) { fail(GVFG_PLAYBACK_EOUTPUT); continue; }
            preview_ready = true;
        }
        auto position = audio_output_position(audio);
        if (has_audio() && !position.running) { fail_audio(); continue; }
        uint64_t audio_pts = has_audio() ? audio_timeline.position(position.played_frames) : 0;
        const uint64_t now = now_ns();
        if (stats.sync_degraded) started = true;
        uint64_t horizon = (std::numeric_limits<uint64_t>::max)();
        if (has_video()) horizon = last_video; // Keep one-frame lookahead.
        if (has_audio()) horizon = (std::min)(horizon, audio_end);
        if ((!has_video() || first_video) && (!has_audio() || first_audio)) {
            if (!started) {
                const uint64_t origin = has_video() && has_audio() ? (std::min)(first_video, first_audio) :
                    (has_video() ? first_video : first_audio);
                external.set(origin, now, true);
                video_clock.set(origin, now, true);
                started = true;
            }
        }
        uint64_t master = external.get(now);
        if (!stats.sync_degraded && started && config.master == GVFG_PLAYBACK_MASTER_AUDIO && audio_pts)
            master = audio_pts;
        else if (!stats.sync_degraded && started && config.master == GVFG_PLAYBACK_MASTER_VIDEO)
            master = video_clock.get(now);

        // Shared buffering: pause both outputs on starvation, preserve accepted
        // data, and resume only after both enabled streams have coverage.
        const uint64_t preroll = static_cast<uint64_t>(config.preroll_ms) * 1'000'000;
        // An audio clock stops at its last consumed sample. Integer sample/ns
        // rounding can leave it just below the horizon forever; starvation
        // must also be detected from actual queued frames and feed eligibility.
        const bool audio_starved = has_audio() && audio_pts &&
            !position.queued_frames && (audios.empty() ||
                (has_video() && audios.front().end > horizon));
        if (!stats.sync_degraded && started && stats.buffering && horizon >= master && horizon - master >= preroll) {
            stats.buffering = 0;
            buffering_since = 0;
            external.set(master, now, false);
            video_clock.set(master, now, false);
        } else if (!stats.sync_degraded && started && !stats.buffering && (master >= horizon || audio_starved)) {
            ++stats.rebuffer_count; stats.buffering = 1;
            buffering_since = now;
            master = (std::min)(master, horizon);
            external.set(master, now, true); video_clock.set(master, now, true);
        }
        const uint64_t patience = (std::max)(250'000'000ULL, preroll * 2);
        if (!stats.sync_degraded &&
            ((stats.buffering && now - (buffering_since ? buffering_since : start_wall) >= patience) ||
             (has_audio() && last_audio_delivery && audio_end &&
              std::llabs(difference(last_audio_delivery, audio_end)) > 250'000'000) ||
             (has_audio() && audio_pts && started && !stats.buffering &&
              std::llabs(difference(audio_pts, master)) > 250'000'000))) {
            degrade(now); started = true;
            position = audio_output_position(audio); audio_pts = 0;
        }
        if (stats.sync_degraded) {
            master = external.get(now);
            const bool fresh_video = !has_video() ||
                (last_video_arrival && now - last_video_arrival < 100'000'000);
            const bool fresh_audio = !has_audio() ||
                (recovery_rate.stable && last_audio_arrival && now - last_audio_arrival < 100'000'000 &&
                 !position.resetting && audio_pts &&
                 (!has_video() || std::llabs(difference(audio_pts, master)) < 150'000'000));
            if (sync_enabled && fresh_video && fresh_audio) {
                if (!stable_since) stable_since = now;
                if (now - stable_since >= 2'000'000'000) {
                    // Outputs already run independently. Adopt their current
                    // common position without flushing or stopping capture.
                    stats.sync_degraded = 0; stats.buffering = 0; buffering_since = 0;
                    master = has_audio() ? (std::min)(master, audio_pts) : master;
                    external.set(master, now, false); video_clock.set(master, now, false);
                    drift = {}; stable_since = 0;
                }
            } else stable_since = 0;
        }
        if (!started || stats.buffering) {
            audio_output_pause(audio, true);
            changed.wait_for(lock, std::chrono::milliseconds(2)); continue;
        }

        // Refill the complete output lead, not one packet per GPU iteration.
        // SDK audio packets can be shorter than a single video presentation.
        // A one-packet limit throttles 10 ms audio to the video frame rate.
        const uint64_t audio_lead_ns = (std::min)(40'000'000ULL,
            static_cast<uint64_t>(config.max_audio_ms) * 1'000'000);
        uint64_t queued_frames = position.queued_frames;
        while (has_audio() && !position.resetting && !audios.empty() &&
               sample_time(queued_frames, config.audio_format.sample_rate) < audio_lead_ns) {
            const Audio &packet = audios.front();
            if (!stats.sync_degraded &&
                (packet.pts > master + audio_lead_ns || (has_video() && packet.end > horizon))) break;
            const uint64_t frames = packet.data.size() / (config.audio_format.channels * 2);
            // Allocate the mapping before accepting PCM, then undo on rejection.
            audio_timeline.append(packet.pts, frames, packet.end);
            const auto status = gvfg_audio_player_write(audio, packet.data.data(),
                static_cast<uint32_t>(packet.data.size()));
            if (status != GVFG_AUDIO_PLAYBACK_OK) {
                audio_timeline.rollback();
                if (status != GVFG_AUDIO_PLAYBACK_EQUEUE_FULL) {
                    fail_audio(); audio_pts = 0;
                }
                break;
            }
            queued_frames += frames;
            audio_bytes -= packet.data.size(); audios.pop_front();
        }
        if (stats.last_error != GVFG_PLAYBACK_OK) continue;
        // Starting a stopped device with an empty queue would advance the
        // system clock without content. Wait for a packet to actually be fed.
        const auto fed_position = audio_output_position(audio);
        bool wait_audio = fed_position.resetting || !fed_position.queued_frames;
        if (stats.sync_degraded && has_audio() && !fed_position.resetting) {
            if (!position.queued_frames) fallback_audio_primed = false;
            if (fed_position.queued_frames && !audio_wait_since) audio_wait_since = now;
            const uint64_t reserve = (std::min)(20'000'000ULL, audio_lead_ns);
            if (sample_time(fed_position.queued_frames, config.audio_format.sample_rate) >= reserve ||
                (audio_wait_since && now - audio_wait_since >= 40'000'000)) fallback_audio_primed = true;
            wait_audio = !fed_position.queued_frames || !fallback_audio_primed;
            if (!fed_position.queued_frames) audio_wait_since = 0;
        }
        audio_output_pause(audio, has_audio() && wait_audio);
        stats.audio_timestamp_ns = audio_pts;
        if (has_audio() && audio_pts && now - last_control >= 20'000'000) {
            last_control = now;
            const int64_t error = difference(audio_pts, master);
            const double ratio = stats.sync_degraded || config.master == GVFG_PLAYBACK_MASTER_AUDIO ? 1.0 : drift.update(error);
            stats.audio_rate_ratio = ratio;
            audio_output_rate(audio, ratio);
        }

        retire_late_video(master);
        if (has_video() && !videos.empty() && (stats.sync_degraded || videos.front().frame.timestamp_ns <= master)) {
            Video packet = std::move(videos.front()); videos.pop_front();
            packet.frame.image.data = packet.data.data();
            const uint64_t pts = packet.frame.timestamp_ns;
            void *target = hwnd;
            lock.unlock();
            bool ok = true, presented = false;
            if (target) {
                gvfg_preview_delivery_stats_t before{}, after{};
                gvfg_preview_get_delivery_stats(preview, &before);
                ok = gvfg_preview_render_frame(preview, &packet.frame.image) == GVFG_PREVIEW_OK &&
                    gvfg_preview_wait_idle(preview, 2000) == GVFG_PREVIEW_OK;
                gvfg_preview_get_delivery_stats(preview, &after);
                // A busy Present or an expired backend frame is a counted
                // preview skip, not a capture/output failure.
                ok = ok && after.failed == before.failed;
                presented = after.presented == before.presented + 1;
            }
            lock.lock();
            video_bytes -= packet.data.size();
            if (!ok) { fail(GVFG_PLAYBACK_EOUTPUT); continue; }
            ++stats.video_processed;
            if (presented) ++stats.video_presented;
            else if (target) ++stats.video_dropped;
            if (presented || !target) {
                stats.video_timestamp_ns = pts;
                video_clock.set(pts, now_ns(), false);
                if (stats.sync_degraded) external.set(pts, now_ns(), false);
            }
        }
        if (stats.audio_timestamp_ns && stats.video_timestamp_ns)
            stats.av_difference_ns = difference(stats.audio_timestamp_ns, video_clock.get(now_ns()));
        // Rendering already waited for the GPU. Do not add a timer wait when
        // another video frame is due; coarse Windows timers can reduce FPS.
        const uint64_t due_clock = stats.sync_degraded ? external.get(now_ns()) :
            config.master == GVFG_PLAYBACK_MASTER_AUDIO && audio_pts ? audio_pts :
            (config.master == GVFG_PLAYBACK_MASTER_VIDEO ? video_clock.get(now_ns()) : external.get(now_ns()));
        if (!videos.empty() && videos.front().frame.timestamp_ns <= due_clock) continue;
        changed.wait_for(lock, std::chrono::milliseconds(1));
    }
}

extern "C" {
void gvfg_playback_default_config(gvfg_playback_config_t *c) {
    if (!c) return;
    *c = {}; c->struct_size = sizeof(*c); c->mode = GVFG_PLAYBACK_AUDIO_VIDEO;
    c->master = GVFG_PLAYBACK_MASTER_EXTERNAL;
    c->audio_format = {48000, 2, 16}; c->preroll_ms = 100;
    c->max_video_frames = 32; c->max_audio_ms = 1000; c->max_video_bytes = 256ULL * 1024 * 1024;
}
gvfg_playback_status_t gvfg_playback_create(gvfg_playback *out) {
    if (!out) return GVFG_PLAYBACK_EINVAL;
    *out = nullptr;
    try { *out = new gvfg_playback_t(); } catch (...) { return GVFG_PLAYBACK_EOUTPUT; }
    return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_attach_window(gvfg_playback p, void *hwnd) {
    if (!p) return GVFG_PLAYBACK_EINVAL;
    if (!p->signal_card.on_window_thread() || (hwnd &&
        GetWindowThreadProcessId(static_cast<HWND>(hwnd), nullptr) != GetCurrentThreadId()))
        return GVFG_PLAYBACK_EINVAL;
    // Attach/configure may send HWND messages: no playback mutex held here.
    gvfg_preview_handle preview = nullptr;
    { std::lock_guard<std::mutex> lock(p->mutex); preview = p->preview; }
    const bool created = hwnd && !preview;
    if (created && gvfg_preview_create(&preview) != GVFG_PREVIEW_OK)
        return GVFG_PLAYBACK_EOUTPUT;
    if (hwnd && gvfg_preview_attach_window(preview, hwnd) != GVFG_PREVIEW_OK) {
        if (created) gvfg_preview_destroy(preview);
        return GVFG_PLAYBACK_EOUTPUT;
    }
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        p->preview = preview; p->hwnd = hwnd;
    }
    return p->signal_card.attach(static_cast<HWND>(hwnd)) ? GVFG_PLAYBACK_OK : GVFG_PLAYBACK_EOUTPUT;
}

gvfg_playback_status_t gvfg_playback_set_signal_connected(gvfg_playback p, int connected) {
    if (!p || (connected != 0 && connected != 1)) return GVFG_PLAYBACK_EINVAL;
    if (!p->signal_card.on_window_thread()) return GVFG_PLAYBACK_EINVAL;
    return p->signal_card.set_connected(connected != 0) ? GVFG_PLAYBACK_OK : GVFG_PLAYBACK_EOUTPUT;
}
gvfg_playback_status_t gvfg_playback_start(gvfg_playback p, const gvfg_playback_config_t *c) {
    if (!p || !c || c->struct_size != sizeof(*c) || c->mode < 1 || c->mode > 3 ||
        c->master < 0 || c->master > 2 || !c->max_video_frames || !c->max_video_bytes ||
        !c->preroll_ms || c->preroll_ms > 1000 || c->max_audio_ms < c->preroll_ms ||
        c->max_audio_ms > 10000 || c->max_video_frames > 600 ||
        ((c->mode & 2) && (!c->audio_format.sample_rate || c->audio_format.sample_rate > 384000 ||
         !c->audio_format.channels || c->audio_format.channels > 32 || c->audio_format.bits_per_sample != 16)))
        return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (p->active) return GVFG_PLAYBACK_ESTATE;
    if (p->audio_output_failed && c->mode == GVFG_PLAYBACK_AUDIO_ONLY)
        return GVFG_PLAYBACK_EOUTPUT;
    p->config = *c;
    if (c->mode == GVFG_PLAYBACK_VIDEO_ONLY) p->config.master = GVFG_PLAYBACK_MASTER_EXTERNAL;
    if (c->mode == GVFG_PLAYBACK_AUDIO_ONLY) p->config.master = GVFG_PLAYBACK_MASTER_AUDIO;
    p->stats = {}; p->stats.struct_size = sizeof(p->stats); p->stats.buffering = 1;
    p->stats.running = 1; p->stats.audio_rate_ratio = 1;
    p->stopping = false; p->first_video = p->last_video = p->first_audio = p->audio_end = 0;
    p->last_audio_delivery = p->video_bytes = p->audio_bytes = 0;
    p->audio_source_clock = {};
    p->start_wall = now_ns(); p->buffering_since = 0;
    p->last_video_arrival = p->last_audio_arrival = 0;
    p->stable_since = p->audio_wait_since = 0;
    p->fallback_audio_primed = false;
    p->recovery_rate = {};
    p->videos.clear(); p->audios.clear(); p->external = {}; p->video_clock = {}; p->drift = {}; p->audio_timeline = {};
    if (p->has_audio()) {
        try { p->audio = audio_output_create_managed(); }
        catch (...) { p->audio = nullptr; }
        if (!p->audio || gvfg_audio_player_start(p->audio, &c->audio_format) != GVFG_AUDIO_PLAYBACK_OK) {
            if (!p->has_video()) {
                gvfg_audio_player_destroy(p->audio); p->audio = nullptr; p->stats.running = 0;
                p->stats.last_error = GVFG_PLAYBACK_EOUTPUT; return GVFG_PLAYBACK_EOUTPUT;
            }
            p->fail_audio();
        }
        if (p->audio) gvfg_audio_player_set_volume(p->audio, p->gain);
    }
    p->active = true; p->exited = false;
    if (!p->sync_enabled || p->audio_output_failed) p->degrade(now_ns(), false);
    try {
        p->worker = std::thread([p] {
            try { p->loop(); } catch (...) { std::lock_guard<std::mutex> lock(p->mutex); p->fail(GVFG_PLAYBACK_EOUTPUT); }
            p->exited = true;
        });
    } catch (...) {
        p->exited = true; p->active = false; p->stats.running = 0;
        gvfg_audio_player_destroy(p->audio); p->audio = nullptr; return GVFG_PLAYBACK_EOUTPUT;
    }
    return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_submit_video(gvfg_playback p, const gvfg_playback_video_frame_t *f) {
    if (!p || !f || !f->timestamp_ns || f->timestamp_ns > INT64_MAX || !f->image.data ||
        f->image.width <= 0 || f->image.height <= 0 || f->image.row_bytes <= 0) return GVFG_PLAYBACK_EINVAL;
    const auto &im = f->image;
    const uint64_t row = im.pixel_format == GVFG_PREVIEW_PIXFMT_RGBA16 ? static_cast<uint64_t>(im.width) * 8 :
        im.pixel_format == GVFG_PREVIEW_PIXFMT_Y210 ? static_cast<uint64_t>(im.width) * 4 :
        im.pixel_format == GVFG_PREVIEW_PIXFMT_GRAY16 ? static_cast<uint64_t>(im.width) * 2 :
        im.pixel_format == GVFG_PREVIEW_PIXFMT_YUY2 ? (static_cast<uint64_t>(im.width) + 1) / 2 * 4 :
        im.pixel_format == GVFG_PREVIEW_PIXFMT_DICOM_YBR_FULL_422 ? (static_cast<uint64_t>(im.width) + 1) / 2 * 8 : 0;
    const uint64_t size = static_cast<uint64_t>(im.row_bytes) * im.height;
    if (!row || row > static_cast<uint64_t>(im.row_bytes) || size > im.data_size || size > SIZE_MAX)
        return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (!p->active || p->stopping || !p->has_video()) return GVFG_PLAYBACK_ESTATE;
    if (p->stats.last_error) return p->stats.last_error;
    if (p->last_video && f->timestamp_ns <= p->last_video) return GVFG_PLAYBACK_EDISCONTINUITY;
    if (size > p->config.max_video_bytes) return GVFG_PLAYBACK_EINVAL;
    const uint64_t now = now_ns();
    p->retire_late_video(p->master_time(now));
    if (p->videos.size() >= p->config.max_video_frames ||
        p->video_bytes > p->config.max_video_bytes - size) {
        p->degrade(now);
        while (!p->videos.empty() && (p->videos.size() >= p->config.max_video_frames ||
               p->video_bytes > p->config.max_video_bytes - size)) {
            p->video_bytes -= p->videos.front().data.size(); p->videos.pop_front();
            ++p->stats.video_processed; ++p->stats.video_dropped;
        }
        if (p->video_bytes > p->config.max_video_bytes - size) {
            // An in-flight GPU copy owns the budget. Skip this incoming preview
            // rather than allocate beyond the limit or stop the capture source.
            p->last_video = f->timestamp_ns; p->last_video_arrival = now;
            ++p->stats.video_submitted; ++p->stats.video_processed; ++p->stats.video_dropped;
            p->changed.notify_one(); return GVFG_PLAYBACK_OK;
        }
    }
    try {
        gvfg_playback_t::Video v; v.frame = *f;
        const auto *begin = static_cast<const uint8_t *>(im.data);
        v.data.assign(begin, begin + static_cast<size_t>(size));
        p->videos.push_back(std::move(v));
    } catch (...) { return GVFG_PLAYBACK_EOUTPUT; }
    p->video_bytes += size; if (!p->first_video) p->first_video = f->timestamp_ns;
    p->last_video = f->timestamp_ns; p->last_video_arrival = now;
    ++p->stats.video_submitted; p->changed.notify_one();
    return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_present_image(gvfg_playback p, const gvfg_preview_frame_t *image) {
    if (!p || !image || !image->data || image->width <= 0 || image->height <= 0)
        return GVFG_PLAYBACK_EINVAL;
    gvfg_preview_handle preview;
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (p->active || !p->hwnd || !p->preview) return GVFG_PLAYBACK_ESTATE;
        preview = p->preview;
    }
    // Like attach/start/stop, callers serialize this control operation. Do not
    // hold the playback mutex while DXGI can send synchronous HWND messages.
    if (gvfg_preview_wait_idle(preview, 2000) != GVFG_PREVIEW_OK ||
        gvfg_preview_prepare(preview, image->width, image->height, image->bit_depth) != GVFG_PREVIEW_OK ||
        gvfg_preview_render_frame(preview, image) != GVFG_PREVIEW_OK ||
        gvfg_preview_wait_idle(preview, 2000) != GVFG_PREVIEW_OK)
        return GVFG_PLAYBACK_EOUTPUT;
    return GVFG_PLAYBACK_OK;
}

gvfg_playback_status_t gvfg_playback_submit_audio(gvfg_playback p, const void *pcm, uint32_t bytes, uint64_t ts) {
    if (!p || !pcm || !bytes || !ts || ts > INT64_MAX) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (!p->active || p->stopping) return GVFG_PLAYBACK_ESTATE;
    if (p->stats.last_error) return p->stats.last_error;
    if (p->audio_output_failed) return GVFG_PLAYBACK_EAUDIO;
    if (!p->has_audio()) return GVFG_PLAYBACK_ESTATE;
    const uint32_t align = p->config.audio_format.channels * 2;
    if (bytes % align) return GVFG_PLAYBACK_EINVAL;
    if (p->last_audio_delivery && ts <= p->last_audio_delivery) return GVFG_PLAYBACK_EDISCONTINUITY;
    const uint64_t limit = static_cast<uint64_t>(p->config.audio_format.sample_rate) * align * p->config.max_audio_ms / 1000;
    if (bytes > limit) return GVFG_PLAYBACK_EINVAL;
    const uint64_t frames = bytes / align, duration = sample_time(frames, p->config.audio_format.sample_rate);
    if (duration > 100'000'000) return GVFG_PLAYBACK_EINVAL;
    if (ts <= duration) return GVFG_PLAYBACK_EINVAL;
    const uint64_t now = now_ns();
    auto position = audio_output_position(p->audio);
    if ((p->last_audio_delivery && ts - p->last_audio_delivery > 250'000'000 + duration) ||
        p->audio_bytes + position.queued_frames * align > limit - bytes)
        p->degrade(now);
    if (p->audio_output_failed) return GVFG_PLAYBACK_EAUDIO;
    position = audio_output_position(p->audio);
    if (p->stats.sync_degraded) {
        const uint64_t device_bytes = position.resetting ? 0 : position.queued_frames * align;
        if (device_bytes > limit - bytes) p->reset_audio();
        if (p->audio_output_failed) return GVFG_PLAYBACK_EAUDIO;
        const uint64_t recent = (std::max)(static_cast<uint64_t>(bytes),
            static_cast<uint64_t>(p->config.audio_format.sample_rate) * align * p->config.preroll_ms / 1000);
        const uint64_t budget = (std::min)(limit - (device_bytes > limit - bytes ? 0 : device_bytes), recent);
        while (!p->audios.empty() && p->audio_bytes > budget - bytes) {
            p->stats.audio_dropped_frames += p->audios.front().data.size() / align;
            p->audio_bytes -= p->audios.front().data.size(); p->audios.pop_front();
            p->stable_since = 0; p->recovery_rate.stable = false;
        }
    }
    // Commit clock state only after the packet has actually been accepted.
    auto source_clock = p->stats.sync_degraded ? AudioSourceClock{} : p->audio_source_clock;
    auto span = source_clock.advance(ts, frames, p->config.audio_format.sample_rate);
    if (!p->stats.sync_degraded && std::llabs(difference(ts, span.end)) > 250'000'000) {
        p->degrade(now); source_clock = {};
        if (p->audio_output_failed) return GVFG_PLAYBACK_EAUDIO;
        span = source_clock.advance(ts, frames, p->config.audio_format.sample_rate);
    }
    try {
        gvfg_playback_t::Audio a; a.pts = span.pts;
        a.end = span.end;
        const auto *begin = static_cast<const uint8_t *>(pcm); a.data.assign(begin, begin + bytes);
        p->audios.push_back(std::move(a));
    } catch (...) { return GVFG_PLAYBACK_EOUTPUT; }
    p->audio_source_clock = source_clock;
    p->first_audio = source_clock.first;
    p->audio_end = span.end;
    p->last_audio_delivery = ts; p->audio_bytes += bytes; ++p->stats.audio_submitted;
    p->last_audio_arrival = now;
    if (p->stats.sync_degraded) {
        p->recovery_rate.observe(ts, frames, p->config.audio_format.sample_rate);
    }
    p->changed.notify_one(); return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_set_volume(gvfg_playback p, float gain) {
    if (!p || !std::isfinite(gain) || gain < 0 || gain > 2) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    p->gain = gain;
    if (p->audio && gvfg_audio_player_set_volume(p->audio, gain) != GVFG_AUDIO_PLAYBACK_OK) {
        p->fail_audio();
        return p->audio_output_failed ? GVFG_PLAYBACK_EAUDIO : GVFG_PLAYBACK_EOUTPUT;
    }
    return GVFG_PLAYBACK_OK;
}

gvfg_playback_status_t gvfg_playback_set_sync_enabled(gvfg_playback p, int enabled) {
    if (!p || (enabled != 0 && enabled != 1)) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (p->sync_enabled == (enabled != 0)) return GVFG_PLAYBACK_OK;
    p->sync_enabled = enabled != 0;
    p->stable_since = 0; p->recovery_rate = {};
    if (p->active && !p->sync_enabled) p->degrade(now_ns(), false);
    return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_stop(gvfg_playback p) {
    if (!p) return GVFG_PLAYBACK_EINVAL;
    { std::lock_guard<std::mutex> lock(p->mutex); p->stopping = true; p->changed.notify_all(); }
    while (!p->exited.load()) service_messages();
    if (p->worker.joinable()) p->worker.join();
    std::lock_guard<std::mutex> lock(p->mutex);
    gvfg_audio_player_destroy(p->audio); p->audio = nullptr;
    p->active = false; p->stats.running = 0; p->videos.clear(); p->audios.clear();
    p->video_bytes = p->audio_bytes = 0;
    return GVFG_PLAYBACK_OK;
}
gvfg_playback_status_t gvfg_playback_reset_timeline(gvfg_playback p) {
    if (!p) return GVFG_PLAYBACK_EINVAL;
    const bool active = p->active; const auto config = p->config;
    gvfg_playback_stop(p);
    if (p->preview && gvfg_preview_clear(p->preview) != GVFG_PREVIEW_OK) return GVFG_PLAYBACK_EOUTPUT;
    return active ? gvfg_playback_start(p, &config) : GVFG_PLAYBACK_OK;
}
void gvfg_playback_destroy(gvfg_playback p) {
    if (!p) return;
    gvfg_playback_stop(p); p->signal_card.attach(nullptr);
    gvfg_preview_destroy(p->preview); delete p;
}
gvfg_playback_status_t gvfg_playback_get_stats(gvfg_playback p, gvfg_playback_stats_t *s) {
    // Keep 2.0.0/2.0.1 callers with the original struct size ABI-compatible.
    constexpr uint32_t legacy_size = offsetof(gvfg_playback_stats_t, video_dropped);
    constexpr uint32_t previous_size = offsetof(gvfg_playback_stats_t, sync_degraded);
    if (!p || !s || (s->struct_size != sizeof(*s) && s->struct_size != legacy_size &&
                    s->struct_size != previous_size)) return GVFG_PLAYBACK_EINVAL;
    const uint32_t requested_size = s->struct_size;
    std::lock_guard<std::mutex> lock(p->mutex);
    auto result = p->stats; result.struct_size = requested_size;
    result.video_queue_bytes = p->video_bytes; result.video_queue_frames = static_cast<uint32_t>(p->videos.size());
    if (p->has_audio() && p->audio) {
        const auto pos = audio_output_position(p->audio);
        result.audio_timestamp_ns = p->audio_timeline.position(pos.played_frames);
        result.audio_queue_ms = (static_cast<double>(p->audio_bytes) / (p->config.audio_format.channels * 2) + pos.queued_frames) *
            1000.0 / p->config.audio_format.sample_rate;
    }
    std::memcpy(s, &result, requested_size);
    return GVFG_PLAYBACK_OK;
}

gvfg_playback_status_t gvfg_playback_get_preview_info(gvfg_playback p, gvfg_preview_info_t *s) {
    if (!p || !s) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    *s = {}; return !p->preview || gvfg_preview_get_info(p->preview, s) == GVFG_PREVIEW_OK ? GVFG_PLAYBACK_OK : GVFG_PLAYBACK_EOUTPUT;
}
gvfg_playback_status_t gvfg_playback_get_preview_stats(gvfg_playback p, gvfg_preview_stats_t *s) {
    if (!p || !s) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    *s = {}; return !p->preview || gvfg_preview_get_stats(p->preview, s) == GVFG_PREVIEW_OK ? GVFG_PLAYBACK_OK : GVFG_PLAYBACK_EOUTPUT;
}

gvfg_playback_status_t gvfg_playback_get_delivery_stats(gvfg_playback p, gvfg_preview_delivery_stats_t *s) {
    if (!p || !s) return GVFG_PLAYBACK_EINVAL;
    std::lock_guard<std::mutex> lock(p->mutex);
    *s = {}; return !p->preview || gvfg_preview_get_delivery_stats(p->preview, s) == GVFG_PREVIEW_OK ? GVFG_PLAYBACK_OK : GVFG_PLAYBACK_EOUTPUT;
}
const char *gvfg_playback_strerror(gvfg_playback_status_t s) {
    switch (s) {
    case GVFG_PLAYBACK_OK: return "ok";
    case GVFG_PLAYBACK_EINVAL: return "invalid playback argument or format";
    case GVFG_PLAYBACK_ESTATE: return "playback is not running or stream disabled";
    case GVFG_PLAYBACK_EOUTPUT: return "playback output failed; inspect device/window and restart";
    case GVFG_PLAYBACK_EQUEUE_FULL: return "playback buffer limit reached; packet not accepted";
    case GVFG_PLAYBACK_EDISCONTINUITY: return "timestamp discontinuity; reset timeline";
    case GVFG_PLAYBACK_EAUDIO: return "audio output disabled; video continues; recreate playback to retry audio";
    default: return "unknown playback error";
    }
}
}
