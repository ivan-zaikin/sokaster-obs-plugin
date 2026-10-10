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

#include "overlay-finder.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <cstring>

namespace sokaster {
namespace {

/* The path every overlay page lives under; the token follows it. */
constexpr const char *kOverlayPath = "/overlay/";

struct Candidate {
	bool found = false;
	bool matches_token = false;
	bool shutdown_when_hidden = false;
};

struct Search {
	const std::string *token = nullptr;
	Candidate best;
};

void consider_source(obs_source_t *source, Search *search)
{
	const char *id = obs_source_get_id(source);
	if (!id || strcmp(id, "browser_source") != 0)
		return;

	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings)
		return;

	const char *url = obs_data_get_string(settings, "url");
	const bool shutdown = obs_data_get_bool(settings, "shutdown");
	obs_data_release(settings);

	if (!url || !strstr(url, kOverlayPath))
		return;

	const bool matches = search->token->empty() || strstr(url, search->token->c_str()) != nullptr;

	/* A matching overlay beats a stale one: with both in the scene, the
	 * co-host is still heard, and that is the state worth reporting. */
	if (!search->best.found || (matches && !search->best.matches_token)) {
		search->best.found = true;
		search->best.matches_token = matches;
		search->best.shutdown_when_hidden = shutdown;
	}
}

bool consider_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<Search *>(param);

	/* Groups keep their own item list; an overlay tucked into a group is as
	 * much on screen as one at the top level. */
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, consider_item, param);
		return true;
	}

	if (obs_source_t *source = obs_sceneitem_get_source(item))
		consider_source(source, search);
	return true;
}

Candidate search_scene(obs_source_t *scene_source, const std::string &token)
{
	Search search;
	search.token = &token;

	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		obs_scene_enum_items(scene, consider_item, &search);

	return search.best;
}

} // namespace

OverlayPresence find_overlay(const std::string &token)
{
	OverlayPresence result;

	/* The current scene first: what viewers see is what decides the answer.
	 * A scene nested inside it as a source is not followed — it is rare for
	 * an overlay, and the server still reports whether one is connected. */
	if (obs_source_t *current = obs_frontend_get_current_scene()) {
		const Candidate here = search_scene(current, token);
		obs_source_release(current);

		if (here.found) {
			result.where = OverlayPresence::Where::CurrentScene;
			result.stale = !here.matches_token;
			result.shutdown_when_hidden = here.shutdown_when_hidden;
			if (here.matches_token)
				return result;
		}
	}

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	Candidate elsewhere;
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		const Candidate found = search_scene(scenes.sources.array[i], token);
		if (found.found && (!elsewhere.found || (found.matches_token && !elsewhere.matches_token)))
			elsewhere = found;
	}

	obs_frontend_source_list_free(&scenes);

	if (!elsewhere.found)
		return result;

	/* A stale overlay in this scene and a good one elsewhere: the good one is
	 * the one to talk about, it only needs moving here. */
	if (result.where == OverlayPresence::Where::CurrentScene && !elsewhere.matches_token)
		return result;

	result.where = OverlayPresence::Where::OtherScene;
	result.stale = !elsewhere.matches_token;
	result.shutdown_when_hidden = elsewhere.shutdown_when_hidden;
	return result;
}

} // namespace sokaster
