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
#include <vector>

namespace sokaster {
namespace {

/* How much the VAD worker takes off the ring at a time. A quarter of a second
 * is far more than ever accumulates; it only bounds the copy. */
constexpr size_t kReadChunkSamples = 4096;

/* Nothing to do until the audio thread has produced another block, which it
 * does about every 21 ms. */
constexpr int kIdleWaitMs = 10;

/* Used when a 429 arrives without a Retry-After we can read. */
constexpr int kDefaultRateLimitSeconds = 10;

} // namespace

AudioLoop &AudioLoop::instance()
{
	static AudioLoop loop;
	return loop;
}

int AudioLoop::resolve_track()
{
	Config cfg = config::get();

	if (cfg.audio_track >= 0) {
		track_ = cfg.audio_track;
		return track_;
	}

	uint32_t used = 0;
	const int free_track = tracks::find_free(&used);
	if (free_track < 0) {
		/* Six tracks, all spoken for. Taking one anyway would push game
		 * audio into somebody's finished recording. */
		obs_log(LOG_WARNING, "audio: every OBS track is in use, nothing to listen on");
		track_ = -1;
		return -1;
	}

	track_ = free_track;
	cfg.audio_track = free_track;
	config::set(cfg);

	obs_log(LOG_INFO, "audio: took track %d (tracks in use: 0x%02x)", free_track + 1, used);
	return track_;
}

bool AudioLoop::hearing_anything() const
{
	return track_ >= 0 && tracks::any_source_heard(static_cast<size_t>(track_));
}

void AudioLoop::start()
{
	if (running_)
		return;

	const Config cfg = config::get();
	if (!cfg.configured()) {
		obs_log(LOG_INFO, "audio: not configured, staying idle");
		return;
	}

	if (resolve_track() < 0)
		return;

	if (!hearing_anything()) {
		/* Deliberately not falling back to the main mix. Transcribing
		 * everything the streamer never agreed to is the exact problem
		 * this track exists to avoid. */
		obs_log(LOG_WARNING, "audio: no source is routed to track %d, the co-host hears nothing", track_ + 1);
	}

	client_.configure(cfg.backend_url, cfg.access_key);
	client_.clear_cancel();

	if (!tap_.start(static_cast<size_t>(track_)))
		return;

	stop_requested_ = false;
	running_ = true;

	vad_worker_ = std::thread([this] { run_vad(); });
	send_worker_ = std::thread([this] { run_sender(); });
}

void AudioLoop::stop()
{
	if (!running_)
		return;

	stop_requested_ = true;

	/* Order matters: the callback goes first so no sample can arrive after
	 * the workers are told to leave. */
	tap_.stop();

	client_.cancel();
	vad_cv_.notify_all();
	queue_cv_.notify_all();

	if (vad_worker_.joinable())
		vad_worker_.join();
	if (send_worker_.joinable())
		send_worker_.join();

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		queue_.clear();
	}

	running_ = false;
	obs_log(LOG_INFO, "audio: stopped");
}

void AudioLoop::enqueue(AudioSegment &&segment)
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		while (queue_.size() >= kQueueLimit) {
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
		obs_log(LOG_ERROR, "audio: without the VAD model the co-host cannot hear, stopping");
		/* Wake the sender so it does not sit waiting for segments that will
		 * never come; stop() still joins both. */
		stop_requested_ = true;
		queue_cv_.notify_all();
		return;
	}

	VadSegmenter segmenter(vad, [this](AudioSegment &&segment) { enqueue(std::move(segment)); });

	std::vector<float> chunk(kReadChunkSamples);

	while (!stop_requested_) {
		const size_t got = tap_.read(chunk.data(), chunk.size());
		if (got == 0) {
			std::unique_lock<std::mutex> lock(vad_mutex_);
			vad_cv_.wait_for(lock, std::chrono::milliseconds(kIdleWaitMs),
					 [this] { return stop_requested_.load(); });
			continue;
		}

		segmenter.push(chunk.data(), got);
	}

	/* Half an utterance at the end of a stream is not worth an upload. */
	segmenter.discard();

	if (vad.predictions() > 0)
		obs_log(LOG_INFO, "audio: vad worker exit, %llu windows, %.2f ms each",
			static_cast<unsigned long long>(vad.predictions()),
			vad.inference_ns() / 1000000.0 / static_cast<double>(vad.predictions()));
}

void AudioLoop::run_sender()
{
	os_set_thread_name("sokaster-audio-send");

	uint64_t backoff_until_ns = 0;
	uint64_t sent = 0;

	while (true) {
		AudioSegment segment;

		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait(lock, [this] { return stop_requested_ || !queue_.empty(); });

			if (stop_requested_)
				break;

			segment = std::move(queue_.front());
			queue_.pop_front();
		}

		const uint64_t now = os_gettime_ns();
		if (now < backoff_until_ns) {
			obs_log(LOG_DEBUG, "audio: rate limited, dropping a %d ms segment", segment.duration_ms);
			continue;
		}

		const HttpResult result = client_.post_audio(segment.wav, segment.duration_ms, segment.captured_at);
		if (stop_requested_)
			break;

		if (result.ok()) {
			++sent;
			obs_log(LOG_DEBUG, "audio: segment #%llu sent, %d ms", static_cast<unsigned long long>(sent),
				segment.duration_ms);
			continue;
		}

		if (result.status == 403) {
			/* The account has speech turned off. Not a failure to retry:
			 * it is a setting, and it will not change mid-stream. */
			obs_log(LOG_INFO, "audio: speech is disabled for this account, the co-host stops listening");
			stop_requested_ = true;
			vad_cv_.notify_all();
			break;
		}

		if (result.status == 401) {
			obs_log(LOG_WARNING, "audio: server rejected the access key, stopping");
			stop_requested_ = true;
			vad_cv_.notify_all();
			break;
		}

		if (result.rate_limited()) {
			const int seconds = result.retry_after_seconds > 0 ? result.retry_after_seconds
									   : kDefaultRateLimitSeconds;
			backoff_until_ns = os_gettime_ns() + static_cast<uint64_t>(seconds) * 1000000000ull;
			obs_log(LOG_WARNING, "audio: rate limited, pausing %d s", seconds);
			continue;
		}

		/* Anything else: speech is perishable, so it is dropped rather than
		 * retried. The next sentence is along in a moment. */
		obs_log(LOG_WARNING, "audio: send failed (%ld), segment dropped", result.status);
	}

	obs_log(LOG_INFO, "audio: sender exit after %llu segments", static_cast<unsigned long long>(sent));
}

} // namespace sokaster
