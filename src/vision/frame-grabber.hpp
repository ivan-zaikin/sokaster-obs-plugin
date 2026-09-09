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

#include <obs.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sokaster {

/* One captured frame, BGRA, tightly packed (stride == width * 4). */
struct Frame {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> data;
};

/*
 * Captures a single source on demand.
 *
 * Not obs_add_raw_video_callback: that hands us every frame at the canvas rate,
 * and at one frame per few seconds it would throw away 99.99% of the work. Here
 * the graphics thread is asked for one frame only when a frame is wanted.
 *
 * The selected source is held weakly. A strong reference would keep a source
 * the streamer deleted alive and stall the scene it belonged to.
 */
class FrameGrabber {
public:
	enum class Status {
		Ok,
		/* Nothing selected yet. */
		NoSource,
		/* The selection was deleted, or its UUID no longer resolves. */
		SourceGone,
		/* Alive but has no picture right now: zero size. */
		NotReady,
		/* The graphics thread refused: lost device, surface allocation. */
		RenderFailed,
	};

	FrameGrabber() = default;
	~FrameGrabber();

	FrameGrabber(const FrameGrabber &) = delete;
	FrameGrabber &operator=(const FrameGrabber &) = delete;

	/* Takes a weak reference; pass nullptr to clear the selection. */
	void select(obs_source_t *source);

	bool has_selection() const;
	std::string selected_uuid() const;
	std::string selected_name() const;

	/* Downscales to fit within max_width x max_height, on the GPU, for free.
	 * Blocks until the graphics thread has run the capture. */
	Status grab(uint32_t max_width, uint32_t max_height, Frame &out);

	/* Frees GPU resources. Must run while the graphics subsystem is alive. */
	void shutdown();

private:
	struct RenderTask;
	static void stage_on_graphics_thread(void *param);
	static void map_on_graphics_thread(void *param);
	static void render_with_context(RenderTask *task);
	static void map_with_context(RenderTask *task);
	static void warn_if_over_budget(const char *stage, uint64_t elapsed_ns);

	mutable std::mutex mutex_;
	obs_weak_source_t *weak_source_ = nullptr;
	std::string uuid_;

	/* Touched only from the graphics thread, or from shutdown() under the
	 * graphics context. */
	gs_texrender_t *texrender_ = nullptr;
	gs_stagesurf_t *stagesurf_ = nullptr;
	uint32_t staged_width_ = 0;
	uint32_t staged_height_ = 0;

	/* True while a source is held visible on our behalf, so the balancing
	 * dec_showing happens exactly once. */
	bool showing_ = false;
};

} // namespace sokaster
