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

#pragma once

#include "audio-tap.hpp"
#include "vad-segmenter.hpp"
#include "net/backend-client.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace sokaster {

/* What the ears are doing, for the dock. Stays put after the workers give up,
 * so the panel can say why the co-host stopped listening. */
struct AudioHealth {
	enum class State {
		/* Not running: OBS is not streaming, or nothing is configured. */
		Idle,
		Listening,
		/* 403: speech is off for this account. A setting, not a fault. */
		SpeechDisabled,
		/* silero_vad.onnx did not load. */
		VadFailed,
		/* 429 or a failed upload; retry_seconds counts down. */
		Retrying,
		KeyRejected,
	};

	State state = State::Idle;
	int retry_seconds = 0;
};

/*
 * The ears: an OBS track mixed for us, a worker that cuts it into utterances,
 * and a second worker that posts them — an upload takes as long as the
 * network takes, and the VAD must not stop listening meanwhile.
 */
class AudioLoop {
public:
	static AudioLoop &instance();

	/* UI thread. No-op when unconfigured or without a track. */
	void start();

	/* UI thread. Joins both workers. Safe to call when not running. */
	void stop();

	/* The OBS track the co-host listens to, 0-based, or -1. */
	int track() const { return track_; }

	/* Keeps the remembered track while no output sends it, or takes a free
	 * one. Does nothing until signed in: an unconfigured plugin touches no
	 * settings. Reads the profile and edits sources, so UI thread, after
	 * OBS_FRONTEND_EVENT_FINISHED_LOADING. */
	int resolve_track();

	/* False while no source is ticked: the co-host hears silence. */
	bool hearing_anything() const;

	AudioHealth health() const;

private:
	AudioLoop() = default;

	void run_vad();
	void run_sender();
	void enqueue(AudioSegment &&segment);

	/* From a worker: the co-host stops listening until the next start(). */
	void give_up(AudioHealth::State state);
	void set_health(AudioHealth::State state, int retry_seconds = 0);

	AudioTap tap_;
	BackendClient client_;
	std::thread vad_worker_;
	std::thread send_worker_;

	/* UI thread only. */
	int track_ = -1;

	mutable std::mutex mutex_;
	std::condition_variable vad_cv_;
	std::condition_variable queue_cv_;
	bool stop_ = false;
	std::deque<AudioSegment> queue_;
	AudioHealth::State health_ = AudioHealth::State::Idle;
	uint64_t retry_until_ns_ = 0;
};

} // namespace sokaster
