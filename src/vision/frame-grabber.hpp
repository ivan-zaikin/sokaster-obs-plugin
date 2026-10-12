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
 * Not obs_add_raw_video_callback: that hands over every frame at the canvas
 * rate, and at one frame per few seconds nearly all of that work would be
 * thrown away. Here the graphics thread renders one frame only when asked.
 *
 * The selected source is held weakly: a strong reference would keep a source
 * the streamer deleted alive.
 */
class FrameGrabber {
public:
	enum class Status {
		Ok,
		/* Nothing selected, or the selection was deleted. */
		NoSource,
		/* Alive but without a picture right now: zero size, or the graphics
		 * thread could not render it. */
		NotReady,
	};

	FrameGrabber() = default;

	FrameGrabber(const FrameGrabber &) = delete;
	FrameGrabber &operator=(const FrameGrabber &) = delete;

	/* UI thread. nullptr clears the selection. */
	void select(obs_source_t *source);
	std::string selected_uuid() const;

	/*
	 * UI thread. While active, the selection is held "showing", so a capture
	 * that is not in the program scene keeps producing frames. Only while
	 * capturing: an idle plugin must not keep a game hooked.
	 */
	void set_active(bool active);

	/* Worker thread. Downscales on the GPU to fit max_width x max_height and
	 * blocks until the graphics thread has run the capture. */
	Status grab(uint32_t max_width, uint32_t max_height, Frame &out);

	/* Frees GPU resources. Must run while the graphics subsystem is alive. */
	void release_gpu();

private:
	struct RenderTask;
	static void stage(void *param);
	static void map(void *param);

	/* A strong reference to the selection, or nullptr. */
	obs_source_t *source() const;

	mutable std::mutex mutex_;
	obs_weak_source_t *weak_source_ = nullptr;
	std::string uuid_;

	/* UI thread only. */
	bool active_ = false;
	obs_weak_source_t *shown_ = nullptr;

	/* Graphics thread only, or release_gpu() under the graphics context. */
	gs_texrender_t *texrender_ = nullptr;
	gs_stagesurf_t *stagesurf_ = nullptr;
	uint32_t staged_width_ = 0;
	uint32_t staged_height_ = 0;
};

} // namespace sokaster
