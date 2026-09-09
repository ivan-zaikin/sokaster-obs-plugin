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

#include "frame-grabber.hpp"
#include "net/backend-client.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sokaster {

/*
 * The eyes: one worker thread that ticks, captures the chosen source, encodes a
 * JPEG and posts it to the backend.
 *
 * Everything expensive happens here, off both the graphics thread and the UI
 * thread. The only thing asked of OBS is a single rendered frame per tick.
 */
class VisionLoop {
public:
	static VisionLoop &instance();

	/* No-op when unconfigured: an unconfigured plugin does no periodic work. */
	void start();

	/* Joins the worker before returning. Safe to call when not running. */
	void stop();

	bool running() const { return running_; }

	/* Restores the source from the saved UUID, or guesses one when there is
	 * nothing saved. Frontend API inside, so call from the UI thread. */
	void restore_selection();

	/* Changes what the co-host looks at and remembers it. */
	void select_source(obs_source_t *source);

	FrameGrabber &grabber() { return grabber_; }

private:
	VisionLoop() = default;
	~VisionLoop() = default;

	VisionLoop(const VisionLoop &) = delete;
	VisionLoop &operator=(const VisionLoop &) = delete;

	void run();

	/* Interruptible sleep; returns false when a stop was requested. */
	bool wait_for(int milliseconds);

	/* Pulls the tempo the backend dictates. Failure keeps the current one. */
	void sync_tempo();

	FrameGrabber grabber_;
	BackendClient client_;

	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};

	std::mutex wait_mutex_;
	std::condition_variable wait_cv_;

	/* Owned by the worker; the backend may move it at any tick. */
	int interval_ms_ = 5000;
};

} // namespace sokaster
