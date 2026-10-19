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

#include "source-picker.hpp"
#include "plugin-config.hpp"

#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <QBuffer>
#include <QImage>

#include <algorithm>
#include <chrono>

namespace sokaster {
namespace {

constexpr uint32_t kMaxWidth = 1280;
constexpr uint32_t kMaxHeight = 720;
constexpr int kJpegQuality = 80;

/* Until the server names the tempo, and the bounds on what it may name. */
constexpr int kDefaultIntervalMs = 5000;
constexpr int kMinIntervalMs = 1000;
constexpr int kMaxIntervalMs = 600000;

/* Failures back off from the tempo up to a minute; a 429 waits half a minute. */
constexpr int kMaxBackoffMs = 60000;
constexpr int kRateLimitBackoffMs = 30000;

/* The tempo is set by an admin and changes rarely: re-read it about once a
 * minute rather than every frame. */
constexpr int kFramesBetweenTempoSync = 12;

constexpr long kUploadTimeoutSeconds = 30;
/* Sent while OBS may be closing: a silent server must not hold up the exit. */
constexpr long kSessionStopTimeoutSeconds = 3;

/* Qt's encoder: obs-deps carries no libjpeg, and Qt is linked for the dock
 * anyway. Format_RGB32 on a little-endian machine is exactly the BGRA byte
 * order libobs staged; JPEG drops the alpha. */
bool encode_jpeg(const Frame &frame, std::vector<uint8_t> &out)
{
	const QImage image(frame.data.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
			   static_cast<qsizetype>(frame.width) * 4, QImage::Format_RGB32);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	if (image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", kJpegQuality)) {
		obs_log(LOG_WARNING, "vision: jpeg encode failed");
		return false;
	}

	out.assign(encoded.constData(), encoded.constData() + encoded.size());
	return true;
}

} // namespace

VisionLoop &VisionLoop::instance()
{
	static VisionLoop loop;
	return loop;
}

void VisionLoop::start()
{
	if (worker_.joinable())
		return;

	const Config cfg = config::get();
	if (!cfg.configured())
		return;

	client_.configure(cfg.base_url(), cfg.access_key);
	interval_ms_ = kDefaultIntervalMs;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = false;
		session_open_ = false;
	}

	grabber_.set_active(true);
	worker_ = std::thread([this] { run(); });
}

void VisionLoop::stop()
{
	if (!worker_.joinable())
		return;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	/* Abort the upload on the wire too, or the stop waits out its timeout. */
	client_.cancel();
	cv_.notify_all();
	worker_.join();

	grabber_.set_active(false);
	set_health(VisionHealth::State::Idle);
}

void VisionLoop::reopen_session()
{
	std::lock_guard<std::mutex> lock(mutex_);
	session_open_ = false;
}

HttpResult VisionLoop::open_session()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (session_open_)
			return {200, {}};
	}

	/* The server keeps sessions in memory: after a restart it has forgotten
	 * ours, and without this the co-host stays silent until the stream is
	 * restarted. */
	HttpResult result = client_.post_json("/api/session/start");
	if (result.ok()) {
		obs_log(LOG_INFO, "vision: session open");
		std::lock_guard<std::mutex> lock(mutex_);
		session_open_ = true;
	}
	return result;
}

void VisionLoop::shutdown()
{
	stop();
	grabber_.select(nullptr);
	grabber_.release_gpu();
}

VisionHealth VisionLoop::health() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint64_t now = os_gettime_ns();

	VisionHealth result;
	result.state = health_;
	/* Counted down at read time and rounded up: "in 0 s" reads as a bug. */
	result.retry_seconds =
		retry_until_ns_ > now ? static_cast<int>((retry_until_ns_ - now + 999999999ull) / 1000000000ull) : 0;
	return result;
}

void VisionLoop::set_health(VisionHealth::State state, int retry_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	health_ = state;
	retry_until_ns_ = retry_ms > 0 ? os_gettime_ns() + static_cast<uint64_t>(retry_ms) * 1000000ull : 0;
}

bool VisionLoop::stopping() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stop_;
}

bool VisionLoop::wait_ms(int ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	return !cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop_; });
}

void VisionLoop::sync_tempo()
{
	const HttpResult result = client_.get("/api/user/me/settings");
	if (!result.ok())
		return;

	obs_data_t *data = obs_data_create_from_json(result.body.c_str());
	const long long interval = data ? obs_data_get_int(data, "effectiveVisionIntervalMs") : 0;
	obs_data_release(data);

	if (interval >= kMinIntervalMs && interval <= kMaxIntervalMs && interval != interval_ms_) {
		obs_log(LOG_INFO, "vision: tempo %d ms -> %lld ms", interval_ms_, interval);
		interval_ms_ = static_cast<int>(interval);
	}
}

void VisionLoop::run()
{
	os_set_thread_name("sokaster-vision");
	obs_log(LOG_INFO, "vision: started");

	sync_tempo();

	Frame frame;
	std::vector<uint8_t> jpeg;
	int failures = 0;
	uint64_t frames = 0;

	for (;;) {
		int wait = interval_ms_;

		/* The session first and apart from the picture: viewers can order
		 * while the game is still minimised. A failed start backs off like a
		 * failed upload. */
		HttpResult result = open_session();
		bool sent = !result.ok();

		if (result.ok()) {
			const FrameGrabber::Status status = grabber_.grab(kMaxWidth, kMaxHeight, frame);
			if (status != FrameGrabber::Status::Ok) {
				/* Between scenes, or nothing picked yet: the dock says so. */
				set_health(status == FrameGrabber::Status::NotReady ? VisionHealth::State::Empty
										    : VisionHealth::State::NoSource);
			} else if (encode_jpeg(frame, jpeg)) {
				result = client_.post_file("/api/frame", "frame", "frame.jpg", "image/jpeg", jpeg, {},
							   kUploadTimeoutSeconds);
				sent = true;
			}
		}

		if (stopping())
			break;

		if (sent) {
			if (result.ok()) {
				failures = 0;
				set_health(VisionHealth::State::Sending);
				if (++frames % kFramesBetweenTempoSync == 0)
					sync_tempo();
			} else if (result.status == 401 || result.status == 403) {
				/* A rejected key stays rejected; retrying only fills the log. */
				obs_log(LOG_WARNING, "vision: server rejected the access key, stopping");
				set_health(VisionHealth::State::KeyRejected);
				break;
			} else if (result.status == 429) {
				wait = std::max(interval_ms_, kRateLimitBackoffMs);
				set_health(VisionHealth::State::RateLimited, wait);
			} else {
				/* Doubling, capped: 5 s, 10 s, 20 s, 40 s, 60 s... */
				++failures;
				wait = static_cast<int>(std::min<long long>(static_cast<long long>(interval_ms_)
										    << std::min(failures - 1, 16),
									    kMaxBackoffMs));
				obs_log(LOG_WARNING, "vision: send failed (%ld), retry in %d ms", result.status, wait);
				set_health(VisionHealth::State::Retrying, wait);
			}
		}

		if (!wait_ms(wait))
			break;
	}

	/* The cancel raised by stop() was for the frame in flight; the server
	 * still needs to hear that the session ended. */
	client_.clear_cancel();
	client_.post_json("/api/session/stop", "{}", kSessionStopTimeoutSeconds);
	obs_log(LOG_INFO, "vision: stopped after %llu frames", static_cast<unsigned long long>(frames));
}

void VisionLoop::select_source(obs_source_t *source)
{
	grabber_.select(source);

	Config cfg = config::get();
	cfg.source_uuid = source ? obs_source_get_uuid(source) : "";
	config::set(cfg);
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
	}

	if (obs_source_t *source = pick_default_source()) {
		select_source(source);
		obs_source_release(source);
	}
}

} // namespace sokaster
