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
#include <plugin-support.h>

#include <cstring>

namespace sokaster {
namespace {

/* Everything else in a scene — overlays, alerts, cameras — is either us, or
 * something the co-host has no business narrating. */
bool is_capture_source(obs_source_t *source)
{
	static const char *const kCaptureIds[] = {"game_capture", "window_capture", "monitor_capture",
						  "display_capture"};

	const char *id = obs_source_get_id(source);
	for (const char *capture : kCaptureIds)
		if (id && strcmp(id, capture) == 0)
			return true;
	return false;
}

/* Calls f(source) for each capture source in the current scene. */
template<typename F> void for_each_capture_source(F &&f)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;

	auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source && is_capture_source(source))
			(*static_cast<F *>(param))(source);
		return true;
	};

	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		obs_scene_enum_items(scene, visit, &f);
	obs_source_release(scene_source);
}

} // namespace

std::vector<CaptureSourceInfo> list_capture_sources()
{
	std::vector<CaptureSourceInfo> result;
	for_each_capture_source([&result](obs_source_t *source) {
		result.push_back({obs_source_get_uuid(source), obs_source_get_name(source)});
	});
	return result;
}

obs_source_t *pick_default_source()
{
	obs_source_t *best = nullptr;
	uint64_t best_area = 0;

	for_each_capture_source([&](obs_source_t *source) {
		const uint64_t area =
			static_cast<uint64_t>(obs_source_get_width(source)) * obs_source_get_height(source);
		if (!best || area > best_area) {
			obs_source_release(best);
			best = obs_source_get_ref(source);
			best_area = area;
		}
	});

	if (best)
		obs_log(LOG_INFO, "vision: default pick '%s'", obs_source_get_name(best));
	return best;
}

} // namespace sokaster
