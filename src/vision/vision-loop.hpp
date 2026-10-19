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

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace sokaster {

/* What the eyes are doing, for the dock. */
struct VisionHealth {
	enum class State {
		/* Not running: OBS is not streaming, or nothing is configured. */
		Idle,
		Sending,
		/* The source is alive but has no picture (minimised game). */
		Empty,
		/* The selection was deleted or never made. */
		NoSource,
		/* Network or 5xx; retry_seconds counts down to the next attempt. */
		Retrying,
		/* The server asked for fewer frames. */
		RateLimited,
		/* 401/403: the key will not start working on its own. */
		KeyRejected,
	};

	State state = State::Idle;
	int retry_seconds = 0;
};

/*
 * The eyes: a worker that captures the chosen source at the tempo the server
 * sets, encodes a JPEG and posts it. The only thing asked of OBS is a single
 * rendered frame per tick; everything else happens on the worker.
 */
class VisionLoop {
public:
	static VisionLoop &instance();

	/* UI thread. No-op when unconfigured. */
	void start();

	/* UI thread. Joins the worker. Safe to call when not running. */
	void stop();

	/* stop(), plus letting go of the source and the GPU resources. On
	 * OBS_FRONTEND_EVENT_EXIT, while the graphics thread is still alive. */
	void shutdown();

	/* The server no longer sees our session — it restarted, say. The worker
	 * opens it again on its next tick. Any thread; no-op when idle. */
	void reopen_session();

	VisionHealth health() const;

	/* The saved source, or a guess when there is none. UI thread, after
	 * OBS_FRONTEND_EVENT_FINISHED_LOADING. */
	void restore_selection();

	/* Changes what the co-host looks at and remembers it. UI thread. */
	void select_source(obs_source_t *source);
	std::string selected_uuid() const { return grabber_.selected_uuid(); }

private:
	VisionLoop() = default;

	void run();
	void sync_tempo();
	/* POST /api/session/start unless the session is already open; a 200 when it is. */
	HttpResult open_session();
	void set_health(VisionHealth::State state, int retry_ms = 0);

	/* Interruptible sleep; false once a stop was requested. */
	bool wait_ms(int ms);
	bool stopping() const;

	FrameGrabber grabber_;
	BackendClient client_;
	std::thread worker_;

	/* Worker-owned; the server may move it at any tick. */
	int interval_ms_ = 0;

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_ = false;
	/* Until POST /api/session/start succeeds the server drops our frames. */
	bool session_open_ = false;
	VisionHealth::State health_ = VisionHealth::State::Idle;
	uint64_t retry_until_ns_ = 0;
};

} // namespace sokaster
