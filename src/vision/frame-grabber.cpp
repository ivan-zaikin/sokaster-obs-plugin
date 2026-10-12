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

#include <plugin-support.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>

namespace sokaster {
namespace {

/* At 60 fps the graphics thread has 16.7 ms per frame; a 1280x720 readback
 * measures 1-2 ms. Past 5 ms it is a real share of someone's frame. */
constexpr uint64_t kGraphicsBudgetNs = 5 * 1000 * 1000;

/* Some sources report odd sizes; an even target avoids chroma surprises. */
uint32_t round_even(uint32_t value)
{
	return value < 2 ? 2 : value & ~1u;
}

void warn_if_over_budget(const char *stage, uint64_t started_ns)
{
	const uint64_t elapsed = os_gettime_ns() - started_ns;
	if (elapsed > kGraphicsBudgetNs)
		obs_log(LOG_WARNING, "vision: %s took %.2f ms on the graphics thread", stage, elapsed / 1000000.0);
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
	bool staged;
	bool mapped;
};

void FrameGrabber::select(obs_source_t *source)
{
	const bool was_active = active_;
	set_active(false);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		obs_weak_source_release(weak_source_);
		weak_source_ = source ? obs_source_get_weak_source(source) : nullptr;
		uuid_ = source ? obs_source_get_uuid(source) : "";
	}

	if (source)
		obs_log(LOG_INFO, "vision: watching '%s'", obs_source_get_name(source));

	set_active(was_active);
}

std::string FrameGrabber::selected_uuid() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return uuid_;
}

obs_source_t *FrameGrabber::source() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return weak_source_ ? obs_weak_source_get_source(weak_source_) : nullptr;
}

void FrameGrabber::set_active(bool active)
{
	if (active == active_)
		return;
	active_ = active;

	/* inc/dec_showing run source signal handlers, so never under our mutex. */
	if (active) {
		if (obs_source_t *s = source()) {
			obs_source_inc_showing(s);
			shown_ = obs_source_get_weak_source(s);
			obs_source_release(s);
		}
	} else if (shown_) {
		if (obs_source_t *s = obs_weak_source_get_source(shown_)) {
			obs_source_dec_showing(s);
			obs_source_release(s);
		}
		obs_weak_source_release(shown_);
		shown_ = nullptr;
	}
}

FrameGrabber::Status FrameGrabber::grab(uint32_t max_width, uint32_t max_height, Frame &out)
{
	obs_source_t *s = source();
	if (!s)
		return Status::NoSource;

	/* Asked every time: a game's resolution can change mid-stream. */
	const uint32_t src_width = obs_source_get_width(s);
	const uint32_t src_height = obs_source_get_height(s);
	if (src_width == 0 || src_height == 0) {
		obs_source_release(s);
		return Status::NotReady;
	}

	const double scale = std::min(
		{1.0, static_cast<double>(max_width) / src_width, static_cast<double>(max_height) / src_height});

	RenderTask task{};
	task.self = this;
	task.source = s;
	task.src_width = src_width;
	task.src_height = src_height;
	task.out_width = round_even(static_cast<uint32_t>(src_width * scale));
	task.out_height = round_even(static_cast<uint32_t>(src_height * scale));
	task.out = &out;

	/* Two blocking passes on the graphics thread, the second queued only
	 * after the first returned: the readback gets a frame to finish instead of
	 * stalling the GPU, and that stall would be a dropped frame. */
	obs_queue_task(OBS_TASK_GRAPHICS, stage, &task, true);
	if (task.staged)
		obs_queue_task(OBS_TASK_GRAPHICS, map, &task, true);

	obs_source_release(s);
	return task.mapped ? Status::Ok : Status::NotReady;
}

/* Graphics tasks run on the graphics thread but outside gs_enter_context, so
 * both passes enter it themselves; that is cheap on the owning thread. */
void FrameGrabber::stage(void *param)
{
	auto *task = static_cast<RenderTask *>(param);
	FrameGrabber *self = task->self;
	const uint64_t started = os_gettime_ns();

	obs_enter_graphics();

	if (!self->texrender_)
		self->texrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_texrender_reset(self->texrender_);

	if (self->texrender_ && gs_texrender_begin(self->texrender_, task->out_width, task->out_height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		/* Viewport at the scaled size, projection at the source size: the
		 * downscale happens during the draw. */
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
			self->staged_width_ = self->stagesurf_ ? task->out_width : 0;
			self->staged_height_ = self->stagesurf_ ? task->out_height : 0;
		}
		if (self->stagesurf_) {
			gs_stage_texture(self->stagesurf_, gs_texrender_get_texture(self->texrender_));
			task->staged = true;
		}
	}

	obs_leave_graphics();
	warn_if_over_budget("stage", started);
}

void FrameGrabber::map(void *param)
{
	auto *task = static_cast<RenderTask *>(param);
	FrameGrabber *self = task->self;
	const uint64_t started = os_gettime_ns();

	obs_enter_graphics();

	uint8_t *mapped = nullptr;
	uint32_t linesize = 0;
	if (self->stagesurf_ && gs_stagesurface_map(self->stagesurf_, &mapped, &linesize)) {
		Frame &out = *task->out;
		const size_t row_bytes = static_cast<size_t>(task->out_width) * 4;
		out.width = task->out_width;
		out.height = task->out_height;
		out.data.resize(row_bytes * task->out_height);

		for (uint32_t row = 0; row < task->out_height; ++row)
			memcpy(out.data.data() + row * row_bytes, mapped + static_cast<size_t>(row) * linesize,
			       row_bytes);

		gs_stagesurface_unmap(self->stagesurf_);
		task->mapped = true;
	}

	obs_leave_graphics();
	warn_if_over_budget("readback", started);
}

void FrameGrabber::release_gpu()
{
	if (!texrender_ && !stagesurf_)
		return;

	obs_enter_graphics();
	if (stagesurf_)
		gs_stagesurface_destroy(stagesurf_);
	gs_texrender_destroy(texrender_);
	obs_leave_graphics();

	stagesurf_ = nullptr;
	texrender_ = nullptr;
	staged_width_ = staged_height_ = 0;
}

} // namespace sokaster
