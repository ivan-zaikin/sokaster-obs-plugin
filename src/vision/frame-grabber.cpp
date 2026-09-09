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

#include "frame-grabber.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>

namespace sokaster {
namespace {

/* What matters is not the absolute cost but the share of a frame interval it
 * takes: at 60 fps the graphics thread has 16.7 ms, and measured on a 1280x720
 * readback this work costs 1-2 ms once per capture interval. Five milliseconds
 * is where it starts being a real share of somebody's frame, so that is where
 * the log speaks up. */
constexpr uint64_t kGraphicsBudgetNs = 5 * 1000 * 1000;

/* Some sources report odd sizes; JPEG does not care, but keeping the target
 * even avoids surprises in chroma subsampling downstream. */
uint32_t round_even(uint32_t value)
{
	return value < 2 ? 2 : value & ~1u;
}

} // namespace

struct FrameGrabber::RenderTask {
	FrameGrabber *self;
	obs_source_t *source;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t out_width;
	uint32_t out_height;
	Frame *out;
	Status status;

	/* Set by the staging pass so the mapping pass knows the texture is on its
	 * way down from the GPU. */
	bool staged;
};

FrameGrabber::~FrameGrabber()
{
	select(nullptr);
	shutdown();
}

void FrameGrabber::select(obs_source_t *source)
{
	obs_weak_source_t *previous_weak = nullptr;
	bool release_previous_showing = false;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		previous_weak = weak_source_;
		release_previous_showing = showing_;

		weak_source_ = source ? obs_source_get_weak_source(source) : nullptr;
		uuid_ = source ? obs_source_get_uuid(source) : "";
		showing_ = false;
	}

	/* Balance the previous selection outside the lock: dec_showing runs source
	 * signal handlers, and holding our mutex through foreign code invites the
	 * kind of deadlock that only shows up on someone else's machine. */
	if (previous_weak) {
		if (release_previous_showing) {
			obs_source_t *previous = obs_weak_source_get_source(previous_weak);
			if (previous) {
				obs_source_dec_showing(previous);
				obs_source_release(previous);
			}
		}
		obs_weak_source_release(previous_weak);
	}

	if (!source)
		return;

	/* A capture source sitting in a scene that is not on air produces nothing
	 * at all. The streamer picked it, so we ask OBS to keep it running rather
	 * than silently shipping black frames. */
	obs_source_inc_showing(source);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		showing_ = true;
	}

	obs_log(LOG_INFO, "vision: watching '%s'", obs_source_get_name(source));
}

bool FrameGrabber::has_selection() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return weak_source_ != nullptr;
}

std::string FrameGrabber::selected_uuid() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return uuid_;
}

std::string FrameGrabber::selected_name() const
{
	obs_weak_source_t *weak = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!weak_source_)
			return {};
		weak = weak_source_;
		obs_weak_source_addref(weak);
	}

	std::string name;
	if (obs_source_t *source = obs_weak_source_get_source(weak)) {
		const char *value = obs_source_get_name(source);
		name = value ? value : "";
		obs_source_release(source);
	}
	obs_weak_source_release(weak);
	return name;
}

FrameGrabber::Status FrameGrabber::grab(uint32_t max_width, uint32_t max_height, Frame &out)
{
	obs_weak_source_t *weak = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!weak_source_)
			return Status::NoSource;
		weak = weak_source_;
		obs_weak_source_addref(weak);
	}

	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return Status::SourceGone;

	/* Asked every tick on purpose: a streamer can change the game's resolution
	 * mid-broadcast, and a stale size would stretch or crop the frame. */
	const uint32_t src_width = obs_source_get_width(source);
	const uint32_t src_height = obs_source_get_height(source);
	if (src_width == 0 || src_height == 0) {
		obs_source_release(source);
		return Status::NotReady;
	}

	const double scale = std::min(
		{1.0, static_cast<double>(max_width) / src_width, static_cast<double>(max_height) / src_height});

	RenderTask task{};
	task.self = this;
	task.source = source;
	task.src_width = src_width;
	task.src_height = src_height;
	task.out_width = round_even(static_cast<uint32_t>(src_width * scale));
	task.out_height = round_even(static_cast<uint32_t>(src_height * scale));
	task.out = &out;
	task.status = Status::RenderFailed;

	/* Blocking on purpose: this runs on the vision worker, never on a thread
	 * OBS needs, and it keeps the frame's lifetime trivially correct.
	 *
	 * Two passes, one frame apart: the second is queued only after the first
	 * has returned, so it lands on the graphics thread's next turn and the
	 * readback has had a frame to finish. */
	obs_queue_task(OBS_TASK_GRAPHICS, stage_on_graphics_thread, &task, true);
	if (task.staged)
		obs_queue_task(OBS_TASK_GRAPHICS, map_on_graphics_thread, &task, true);

	obs_source_release(source);
	return task.status;
}

/*
 * libobs runs the graphics task queue on the graphics thread but outside
 * gs_enter_context, so every gs_* call would otherwise find no active context
 * and quietly do nothing. Entering is cheap and reentrant on the thread that
 * already owns the device.
 */
void FrameGrabber::stage_on_graphics_thread(void *param)
{
	auto *task = static_cast<RenderTask *>(param);
	const uint64_t started = os_gettime_ns();

	obs_enter_graphics();
	render_with_context(task);
	obs_leave_graphics();

	warn_if_over_budget("stage", os_gettime_ns() - started);
}

void FrameGrabber::map_on_graphics_thread(void *param)
{
	auto *task = static_cast<RenderTask *>(param);
	const uint64_t started = os_gettime_ns();

	obs_enter_graphics();
	map_with_context(task);
	obs_leave_graphics();

	warn_if_over_budget("readback", os_gettime_ns() - started);
}

void FrameGrabber::warn_if_over_budget(const char *stage, uint64_t elapsed_ns)
{
	if (elapsed_ns > kGraphicsBudgetNs)
		obs_log(LOG_WARNING, "vision: %s took %.2f ms on the graphics thread", stage, elapsed_ns / 1000000.0);
}

void FrameGrabber::render_with_context(RenderTask *task)
{
	FrameGrabber *self = task->self;

	if (!self->texrender_)
		self->texrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!self->texrender_)
		return;

	gs_texrender_reset(self->texrender_);
	if (!gs_texrender_begin(self->texrender_, task->out_width, task->out_height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	/* The viewport is the scaled size and the projection is the source size,
	 * so the downscale happens on the GPU during the draw. */
	gs_ortho(0.0f, static_cast<float>(task->src_width), 0.0f, static_cast<float>(task->src_height), -100.0f,
		 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(task->source);
	gs_blend_state_pop();

	gs_texrender_end(self->texrender_);

	if (self->staged_width_ != task->out_width || self->staged_height_ != task->out_height) {
		if (self->stagesurf_)
			gs_stagesurface_destroy(self->stagesurf_);
		self->stagesurf_ = gs_stagesurface_create(task->out_width, task->out_height, GS_BGRA);
		self->staged_width_ = task->out_width;
		self->staged_height_ = task->out_height;
	}
	if (!self->stagesurf_) {
		self->staged_width_ = self->staged_height_ = 0;
		return;
	}

	/* Copy down from the GPU is started here and read in the next pass:
	 * mapping straight after staging stalls the graphics thread until the GPU
	 * catches up, and that stall is a dropped frame for the streamer. */
	gs_stage_texture(self->stagesurf_, gs_texrender_get_texture(self->texrender_));
	task->staged = true;
}

void FrameGrabber::map_with_context(RenderTask *task)
{
	FrameGrabber *self = task->self;
	if (!self->stagesurf_)
		return;

	uint8_t *mapped = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(self->stagesurf_, &mapped, &linesize))
		return;

	Frame &out = *task->out;
	out.width = task->out_width;
	out.height = task->out_height;
	out.data.resize(static_cast<size_t>(task->out_width) * task->out_height * 4);

	const size_t row_bytes = static_cast<size_t>(task->out_width) * 4;
	for (uint32_t row = 0; row < task->out_height; ++row)
		memcpy(out.data.data() + row * row_bytes, mapped + static_cast<size_t>(row) * linesize, row_bytes);

	gs_stagesurface_unmap(self->stagesurf_);
	task->status = Status::Ok;
}

void FrameGrabber::shutdown()
{
	if (!texrender_ && !stagesurf_)
		return;

	obs_enter_graphics();
	if (stagesurf_) {
		gs_stagesurface_destroy(stagesurf_);
		stagesurf_ = nullptr;
	}
	if (texrender_) {
		gs_texrender_destroy(texrender_);
		texrender_ = nullptr;
	}
	obs_leave_graphics();

	staged_width_ = staged_height_ = 0;
}

} // namespace sokaster
