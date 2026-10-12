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
#include "audio/audio-tap.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <plugin-support.h>

#include <cstring>

namespace sokaster {
namespace {

/* The path every overlay page lives under; the token follows it. */
constexpr const char *kOverlayPath = "/overlay/";

/* The page is laid out for this size; another canvas gets it scaled to fit
 * rather than reflowed. */
constexpr int kPageWidth = 1920;
constexpr int kPageHeight = 1080;

struct Candidate {
	bool found = false;
	bool matches_token = false;
	bool shutdown_when_hidden = false;
	std::string uuid;
};

struct Search {
	/* base_url + kOverlayPath: only a page of our own server is ours. Other
	 * services keep overlays under /overlay/ too, and "Update address" must
	 * never rewrite one of theirs. */
	std::string prefix;
	const std::string *token = nullptr;
	Candidate best;
};

/* The token is the whole next path segment, not just a prefix of it. */
bool names_token(const char *rest, const std::string &token)
{
	if (strncmp(rest, token.c_str(), token.size()) != 0)
		return false;
	const char next = rest[token.size()];
	return next == '\0' || next == '?' || next == '#' || next == '/';
}

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

	if (!url || strncmp(url, search->prefix.c_str(), search->prefix.size()) != 0)
		return;

	const bool matches = search->token->empty() || names_token(url + search->prefix.size(), *search->token);

	/* A matching overlay beats a stale one: with both in the scene, the
	 * co-host is still heard, and that is the state worth reporting. */
	if (!search->best.found || (matches && !search->best.matches_token)) {
		search->best.found = true;
		search->best.matches_token = matches;
		search->best.shutdown_when_hidden = shutdown;
		search->best.uuid = obs_source_get_uuid(source);
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

Candidate search_scene(obs_source_t *scene_source, const std::string &base_url, const std::string &token)
{
	Search search;
	search.prefix = base_url + kOverlayPath;
	search.token = &token;

	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		obs_scene_enum_items(scene, consider_item, &search);

	return search.best;
}

} // namespace

OverlayPresence find_overlay(const std::string &base_url, const std::string &token)
{
	OverlayPresence result;

	/* The current scene first: what viewers see is what decides the answer.
	 * A scene nested inside it as a source is not followed — it is rare for
	 * an overlay, and the server still reports whether one is connected. */
	if (obs_source_t *current = obs_frontend_get_current_scene()) {
		const Candidate here = search_scene(current, base_url, token);
		obs_source_release(current);

		if (here.found) {
			result.where = OverlayPresence::Where::CurrentScene;
			result.stale = !here.matches_token;
			result.shutdown_when_hidden = here.shutdown_when_hidden;
			result.source_uuid = here.uuid;
			if (here.matches_token)
				return result;
		}
	}

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	Candidate elsewhere;
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		const Candidate found = search_scene(scenes.sources.array[i], base_url, token);
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
	result.source_uuid = elsewhere.uuid;
	return result;
}

std::string overlay_url(const std::string &base_url, const std::string &token)
{
	return base_url + kOverlayPath + token;
}

namespace {

/* OBS keeps source names unique in its own dialogs; so do we, or the streamer
 * is left with two identical lines in the source list. */
std::string unique_name(const char *base)
{
	std::string name = base;
	for (int n = 2;; ++n) {
		obs_source_t *existing = obs_get_source_by_name(name.c_str());
		if (!existing)
			return name;
		obs_source_release(existing);
		name = std::string(base) + " " + std::to_string(n);
	}
}

void fit_to_canvas(obs_sceneitem_t *item)
{
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;

	struct vec2 bounds;
	vec2_set(&bounds, static_cast<float>(ovi.base_width), static_cast<float>(ovi.base_height));
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(item, &bounds);
}

obs_source_t *create_overlay_source(const std::string &url, const char *new_name)
{
	/* Without obs-browser there is nothing to make; creating it anyway would
	 * leave a dead "missing source" in the scene. */
	if (!obs_get_source_output_flags("browser_source"))
		return nullptr;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.c_str());
	obs_data_set_int(settings, "width", kPageWidth);
	obs_data_set_int(settings, "height", kPageHeight);
	/* The voice comes out of this page: through OBS it reaches the stream and
	 * the mixer, not only the streamer's speakers. */
	obs_data_set_bool(settings, "reroute_audio", true);
	obs_data_set_bool(settings, "shutdown", false);

	const std::string name = unique_name(new_name);
	obs_source_t *source = obs_source_create("browser_source", name.c_str(), settings, nullptr);
	obs_data_release(settings);
	return source;
}

} // namespace

bool add_overlay_to_current_scene(const OverlayPresence &found, const std::string &url, const char *new_name,
				  int listen_track)
{
	obs_source_t *current = obs_frontend_get_current_scene();
	if (!current)
		return false;

	obs_scene_t *scene = obs_scene_from_source(current);
	bool fresh = false;
	obs_source_t *source = nullptr;

	/* Ours, elsewhere: the same source once more. A stale one is not reused —
	 * that would be the old address in one more scene. */
	if (found.where == OverlayPresence::Where::OtherScene && !found.stale && !found.source_uuid.empty())
		source = obs_get_source_by_uuid(found.source_uuid.c_str());

	if (!source) {
		source = create_overlay_source(url, new_name);
		fresh = true;
	}

	bool placed = false;
	if (scene && source) {
		if (obs_sceneitem_t *item = obs_scene_add(scene, source)) {
			fit_to_canvas(item);
			placed = true;
		}
	}

	if (placed && fresh && listen_track >= 0)
		tracks::set_source_heard(obs_source_get_uuid(source), static_cast<size_t>(listen_track), false);

	if (placed)
		obs_log(LOG_INFO, "overlay: %s in scene '%s'", fresh ? "created" : "added",
			obs_source_get_name(current));
	else
		obs_log(LOG_WARNING, "overlay: could not add to scene '%s'", obs_source_get_name(current));

	obs_source_release(source);
	obs_source_release(current);
	return placed;
}

bool set_overlay_url(const std::string &source_uuid, const std::string &url)
{
	obs_source_t *source = source_uuid.empty() ? nullptr : obs_get_source_by_uuid(source_uuid.c_str());
	if (!source)
		return false;

	/* The browser source reloads the page by itself when its URL changes. */
	obs_data_t *changes = obs_data_create();
	obs_data_set_string(changes, "url", url.c_str());
	obs_source_update(source, changes);
	obs_data_release(changes);
	obs_source_release(source);

	obs_log(LOG_INFO, "overlay: address updated");
	return true;
}

} // namespace sokaster
