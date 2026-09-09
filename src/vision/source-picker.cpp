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

#include "source-picker.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <cstring>

namespace sokaster {
namespace {

/* Everything else in a scene — overlays, alerts, cameras, browser docks — is
 * either us, or something the co-host has no business narrating. */
const char *const kCaptureSourceIds[] = {
	"game_capture",
	"window_capture",
	"monitor_capture",
	"display_capture",
};

bool is_capture_source(const obs_source_t *source)
{
	const char *id = obs_source_get_id(const_cast<obs_source_t *>(source));
	if (!id)
		return false;

	for (const char *candidate : kCaptureSourceIds)
		if (strcmp(id, candidate) == 0)
			return true;

	return false;
}

struct Best {
	obs_source_t *source = nullptr;
	uint64_t area = 0;

	/* Capture sources report a size only once they have produced a frame, and
	 * this runs moments after OBS finishes loading — usually before that has
	 * happened. Without a zero-area fallback the first launch would find
	 * nothing at exactly the moment the streamer expects a suggestion. */
	obs_source_t *first = nullptr;
};

bool consider_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *best = static_cast<Best *>(param);

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || !is_capture_source(source))
		return true;

	if (!best->first)
		best->first = source;

	const uint64_t area = static_cast<uint64_t>(obs_source_get_width(source)) * obs_source_get_height(source);
	if (area > best->area) {
		best->area = area;
		best->source = source;
	}

	return true;
}

} // namespace

obs_source_t *pick_default_source()
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return nullptr;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	Best best;
	if (scene)
		obs_scene_enum_items(scene, consider_item, &best);

	/* consider_item borrows the item's reference, so take our own before the
	 * scene goes out of scope. */
	obs_source_t *chosen = best.source ? best.source : best.first;
	obs_source_t *result = chosen ? obs_source_get_ref(chosen) : nullptr;
	obs_source_release(scene_source);

	if (result)
		obs_log(LOG_INFO, "vision: default pick '%s'", obs_source_get_name(result));

	return result;
}

} // namespace sokaster
