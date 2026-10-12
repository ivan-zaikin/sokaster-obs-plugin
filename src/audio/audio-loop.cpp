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

#include "audio-loop.hpp"

#include "silero-vad.hpp"
#include "plugin-config.hpp"

#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <chrono>
#include <string>
#include <vector>

namespace sokaster {
namespace {

static_assert(kListenSampleRate == SileroVad::kSampleRate, "the tap must deliver what the model expects");

/* How much the VAD worker takes off the ring at a time. */
constexpr size_t kReadChunkSamples = 4096;

/* The audio thread produces a block about every 21 ms. */
constexpr int kIdleWaitMs = 10;

/* A 429 pauses uploads, and a failed one is reported, for this long. */
constexpr int kRetrySeconds = 10;

/* Newest wins: five segments is already a long backlog, and a co-host reacting
 * to old speech is worse than one that missed it. */
constexpr size_t kQueueLimit = 5;

constexpr long kUploadTimeoutSeconds = 30;

} // namespace

AudioLoop &AudioLoop::instance()
{
	static AudioLoop loop;
	return loop;
}

int AudioLoop::resolve_track()
{
	Config cfg = config::get();
	if (!cfg.configured())
		return track_;

	const uint32_t taken = tracks::output_tracks();
	if (cfg.audio_track >= 0 && (taken & (1u << cfg.audio_track)) == 0) {
		track_ = cfg.audio_track;
		return track_;
	}
	if (cfg.audio_track >= 0) {
		/* The streamer has since put our track into a stream or a
		 * recording. The ticks on it are theirs now; we move out. */
		obs_log(LOG_WARNING, "audio: track %d is now sent by an output, picking another", cfg.audio_track + 1);
	}

	track_ = tracks::find_free(taken);
	if (track_ < 0) {
		obs_log(LOG_WARNING, "audio: every OBS track is sent by an output, nothing to listen on");
		return track_;
	}

	tracks::clear_track(static_cast<size_t>(track_));
	cfg.audio_track = track_;
	config::set(cfg);

	obs_log(LOG_INFO, "audio: took track %d (tracks sent by outputs: 0x%02x)", track_ + 1, taken);
	return track_;
}

bool AudioLoop::hearing_anything() const
{
	return track_ >= 0 && tracks::any_source_heard(static_cast<size_t>(track_));
}

AudioHealth AudioLoop::health() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint64_t now = os_gettime_ns();

	AudioHealth result;
	result.state = health_;
	result.retry_seconds =
		retry_until_ns_ > now ? static_cast<int>((retry_until_ns_ - now + 999999999ull) / 1000000000ull) : 0;

	/* A retry whose wait is over is listening again, as far as anyone can tell. */
	if (result.state == AudioHealth::State::Retrying && result.retry_seconds == 0)
		result.state = AudioHealth::State::Listening;
	return result;
}

void AudioLoop::set_health(AudioHealth::State state, int retry_seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	health_ = state;
	retry_until_ns_ = retry_seconds > 0 ? os_gettime_ns() + static_cast<uint64_t>(retry_seconds) * 1000000000ull
					    : 0;
}

void AudioLoop::start()
{
	if (vad_worker_.joinable())
		return;

	const Config cfg = config::get();
	if (!cfg.configured() || resolve_track() < 0)
		return;

	/* Deliberately no fallback to the main mix: transcribing what the
	 * streamer never ticked is exactly what the track exists to avoid. */
	if (!hearing_anything())
		obs_log(LOG_WARNING, "audio: no source is routed to track %d, the co-host hears nothing", track_ + 1);

	client_.configure(cfg.base_url(), cfg.access_key);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = false;
		queue_.clear();
	}
	set_health(AudioHealth::State::Listening);

	tap_.start(static_cast<size_t>(track_));
	vad_worker_ = std::thread([this] { run_vad(); });
	send_worker_ = std::thread([this] { run_sender(); });
}

void AudioLoop::stop()
{
	if (!vad_worker_.joinable())
		return;

	/* The callback goes first, so no sample arrives after the workers leave. */
	tap_.stop();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	client_.cancel();
	vad_cv_.notify_all();
	queue_cv_.notify_all();
	vad_worker_.join();
	send_worker_.join();

	set_health(AudioHealth::State::Idle);
}

void AudioLoop::give_up(AudioHealth::State state)
{
	set_health(state);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	vad_cv_.notify_all();
	queue_cv_.notify_all();
}

void AudioLoop::enqueue(AudioSegment &&segment)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (queue_.size() >= kQueueLimit) {
			obs_log(LOG_WARNING, "audio: sender is behind, dropping a %d ms segment",
				queue_.front().duration_ms);
			queue_.pop_front();
		}
		queue_.push_back(std::move(segment));
	}
	queue_cv_.notify_one();
}

void AudioLoop::run_vad()
{
	os_set_thread_name("sokaster-audio");

	SileroVad vad;
	if (!vad.load(silero_model_path())) {
		obs_log(LOG_ERROR, "audio: without the VAD model the co-host cannot hear");
		give_up(AudioHealth::State::VadFailed);
		return;
	}

	VadSegmenter segmenter(vad, [this](AudioSegment &&segment) { enqueue(std::move(segment)); });
	std::vector<float> chunk(kReadChunkSamples);

	std::unique_lock<std::mutex> lock(mutex_);
	while (!stop_) {
		lock.unlock();
		const size_t got = tap_.read(chunk.data(), chunk.size());
		if (got > 0)
			segmenter.push(chunk.data(), got);
		lock.lock();

		if (got == 0)
			vad_cv_.wait_for(lock, std::chrono::milliseconds(kIdleWaitMs), [this] { return stop_; });
	}
}

void AudioLoop::run_sender()
{
	os_set_thread_name("sokaster-audio-send");

	uint64_t paused_until_ns = 0;
	for (;;) {
		AudioSegment segment;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
			if (stop_)
				return;

			segment = std::move(queue_.front());
			queue_.pop_front();
		}

		/* Speech is perishable: what arrives during a pause is dropped, not
		 * kept for later. */
		if (os_gettime_ns() < paused_until_ns)
			continue;

		const HttpResult result = client_.post_file("/api/audio", "audio", "segment.wav", "audio/wav",
							    segment.wav,
							    {{"durationMs", std::to_string(segment.duration_ms)},
							     {"capturedAt", segment.captured_at},
							     {"source", "mixed"}},
							    kUploadTimeoutSeconds);

		if (result.ok()) {
			set_health(AudioHealth::State::Listening);
		} else if (result.status == 403) {
			obs_log(LOG_INFO, "audio: speech is disabled for this account");
			give_up(AudioHealth::State::SpeechDisabled);
			return;
		} else if (result.status == 401) {
			obs_log(LOG_WARNING, "audio: server rejected the access key, stopping");
			give_up(AudioHealth::State::KeyRejected);
			return;
		} else if (result.status == 429) {
			paused_until_ns = os_gettime_ns() + static_cast<uint64_t>(kRetrySeconds) * 1000000000ull;
			obs_log(LOG_WARNING, "audio: rate limited, pausing %d s", kRetrySeconds);
			set_health(AudioHealth::State::Retrying, kRetrySeconds);
		} else if (!client_.cancelled()) {
			obs_log(LOG_WARNING, "audio: send failed (%ld), segment dropped", result.status);
			set_health(AudioHealth::State::Retrying, kRetrySeconds);
		}
	}
}

} // namespace sokaster
