/*
sokaster — an AI co-host inside OBS Studio
Copyright (C) 2026 sokaster <tnixton@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "vision-loop.hpp"

#include "jpeg-encoder.hpp"
#include "source-picker.hpp"
#include "plugin-config.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>

namespace sokaster {
namespace {

/* Failures back off from the normal tempo up to a minute: a backend that is
 * down stays down for a while, and a plugin that keeps knocking every five
 * seconds is a plugin that shows up in someone's firewall log. */
constexpr int kMaxBackoffMs = 60000;

/* A 429 means the server has already told us the rate; the desktop client
 * settled on half a minute and nothing suggests a better number. */
constexpr int kRateLimitBackoffMs = 30000;

/* Roughly a minute of normal ticks between tempo checks. The tempo is set by
 * an admin and changes rarely, so polling it hard buys nothing. */
constexpr int kTicksBetweenTempoSync = 12;

int clamp_interval(int value, int fallback)
{
	return (value >= 1000 && value <= 600000) ? value : fallback;
}

} // namespace

VisionLoop &VisionLoop::instance()
{
	static VisionLoop loop;
	return loop;
}

void VisionLoop::start()
{
	if (running_)
		return;

	const Config cfg = config::get();
	if (!cfg.configured()) {
		obs_log(LOG_INFO, "vision: not configured, staying idle");
		return;
	}

	client_.configure(cfg.backend_url, cfg.access_key);
	interval_ms_ = cfg.vision_interval_ms;

	stop_requested_ = false;
	running_ = true;
	worker_ = std::thread([this] { run(); });
}

void VisionLoop::stop()
{
	if (!running_)
		return;

	stop_requested_ = true;

	/* Abort whatever is on the wire, then wake the sleeping tick. Without the
	 * first, shutdown could sit through a 30 second upload timeout. */
	client_.cancel();
	wait_cv_.notify_all();

	if (worker_.joinable())
		worker_.join();

	running_ = false;
	obs_log(LOG_INFO, "vision: stopped");
}

bool VisionLoop::wait_for(int milliseconds)
{
	std::unique_lock<std::mutex> lock(wait_mutex_);
	wait_cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stop_requested_.load(); });
	return !stop_requested_;
}

void VisionLoop::sync_tempo()
{
	const HttpResult result = client_.get_settings();
	if (!result.ok())
		return;

	obs_data_t *data = obs_data_create_from_json(result.body.c_str());
	if (!data) {
		obs_log(LOG_WARNING, "vision: settings response was not JSON");
		return;
	}

	/* ASP.NET serialises camelCase; the PascalCase fallback costs one line and
	 * survives a serialiser setting changing under us. */
	long long interval = obs_data_get_int(data, "effectiveVisionIntervalMs");
	if (interval == 0)
		interval = obs_data_get_int(data, "EffectiveVisionIntervalMs");
	obs_data_release(data);

	const int next = clamp_interval(static_cast<int>(interval), interval_ms_);
	if (next != interval_ms_) {
		obs_log(LOG_INFO, "vision: tempo %d ms -> %d ms", interval_ms_, next);
		interval_ms_ = next;
	}
}

void VisionLoop::run()
{
	os_set_thread_name("sokaster-vision");
	obs_log(LOG_INFO, "vision: started, tempo %d ms", interval_ms_);

	client_.post_session_start();
	sync_tempo();

	Frame frame;
	std::vector<uint8_t> jpeg;
	int consecutive_failures = 0;
	int ticks_since_tempo_sync = 0;
	uint64_t frame_number = 0;

	while (!stop_requested_) {
		const Config cfg = config::get();
		const uint64_t tick_started = os_gettime_ns();

		const FrameGrabber::Status status = grabber_.grab(cfg.max_width, cfg.max_height, frame);
		if (status != FrameGrabber::Status::Ok) {
			/* None of these are our failure to report loudly every tick:
			 * the streamer is between scenes, or has not picked a source
			 * yet. The dock will say so; the log says it once per tick at
			 * debug level and moves on. */
			obs_log(LOG_DEBUG, "vision: no frame (status %d)", static_cast<int>(status));
			if (!wait_for(interval_ms_))
				break;
			continue;
		}

		if (!encode_jpeg(frame, cfg.jpeg_quality, jpeg)) {
			if (!wait_for(interval_ms_))
				break;
			continue;
		}

		const HttpResult result = client_.post_frame(jpeg);
		if (stop_requested_)
			break;

		if (result.ok()) {
			consecutive_failures = 0;
			++frame_number;
			obs_log(LOG_DEBUG, "vision: frame #%llu, %ux%u, %zu kB, %.0f ms",
				static_cast<unsigned long long>(frame_number), frame.width, frame.height,
				jpeg.size() / 1024, (os_gettime_ns() - tick_started) / 1000000.0);

			if (++ticks_since_tempo_sync >= kTicksBetweenTempoSync) {
				ticks_since_tempo_sync = 0;
				sync_tempo();
			}

			if (!wait_for(interval_ms_))
				break;
			continue;
		}

		if (result.status == 401 || result.status == 403) {
			/* A rejected key will stay rejected. Retrying forever would
			 * only fill the streamer's log with the same line. */
			obs_log(LOG_WARNING, "vision: server rejected the access key, stopping");
			break;
		}

		++consecutive_failures;

		int backoff = interval_ms_;
		if (result.rate_limited()) {
			backoff = std::max(interval_ms_, kRateLimitBackoffMs);
			obs_log(LOG_WARNING, "vision: rate limited, waiting %d ms", backoff);
		} else {
			/* Doubling, capped: 5s, 10s, 20s, 40s, 60s, 60s... */
			const int shift = std::min(consecutive_failures - 1, 16);
			const long long scaled = static_cast<long long>(interval_ms_) << shift;
			backoff = static_cast<int>(std::min<long long>(scaled, kMaxBackoffMs));
			obs_log(LOG_WARNING, "vision: send failed (%ld), retry in %d ms", result.status, backoff);
		}

		if (!wait_for(backoff))
			break;
	}

	/* The abort raised by stop() was for the frame in flight; the backend
	 * still needs to hear that the session ended. */
	client_.clear_cancel();
	client_.post_session_stop();
	obs_log(LOG_INFO, "vision: worker exit after %llu frames", static_cast<unsigned long long>(frame_number));
}

void VisionLoop::select_source(obs_source_t *source)
{
	grabber_.select(source);
	config::set_source_uuid(source ? obs_source_get_uuid(source) : "");
}

void VisionLoop::restore_selection()
{
	const Config cfg = config::get();

	if (!cfg.source_uuid.empty()) {
		if (obs_source_t *source = obs_get_source_by_uuid(cfg.source_uuid.c_str())) {
			grabber_.select(source);
			obs_source_release(source);
			return;
		}
		obs_log(LOG_INFO, "vision: saved source is gone, picking a new one");
	}

	if (obs_source_t *source = pick_default_source()) {
		select_source(source);
		obs_source_release(source);
		return;
	}

	obs_log(LOG_INFO, "vision: no capture source in the current scene");
}

} // namespace sokaster
