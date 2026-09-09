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

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sokaster {

/*
 * The ears: a track OBS mixes for us, a worker that turns it into utterances,
 * and a second worker that posts them.
 *
 * Two threads rather than one because an upload takes as long as the network
 * takes, and the VAD must not stop listening while it happens.
 */
class AudioLoop {
public:
	static AudioLoop &instance();

	void start();
	void stop();

	bool running() const { return running_; }

	/* The OBS track the co-host listens to, 0-based. Valid once started. */
	int track() const { return track_; }

	/* Picks a track and remembers it, or returns the remembered one.
	 * Frontend-free, but touches sources, so not before FINISHED_LOADING. */
	int resolve_track();

	/* False while no source is ticked: the co-host hears silence, and the
	 * panel should say so rather than imply it is listening. */
	bool hearing_anything() const;

private:
	AudioLoop() = default;
	~AudioLoop() = default;

	AudioLoop(const AudioLoop &) = delete;
	AudioLoop &operator=(const AudioLoop &) = delete;

	void run_vad();
	void run_sender();

	void enqueue(AudioSegment &&segment);

	AudioTap tap_;
	BackendClient client_;

	std::thread vad_worker_;
	std::thread send_worker_;

	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	int track_ = -1;

	/* Wakes the VAD worker when it is waiting for samples. */
	std::mutex vad_mutex_;
	std::condition_variable vad_cv_;

	/* Newest-wins queue: five segments is already ten seconds of backlog,
	 * and a co-host reacting to ten-second-old speech is worse than one that
	 * missed it. */
	static constexpr size_t kQueueLimit = 5;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<AudioSegment> queue_;
};

} // namespace sokaster
