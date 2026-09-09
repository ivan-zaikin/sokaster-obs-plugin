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

#include "plugin-config.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <mutex>

namespace sokaster {
namespace {

std::mutex g_mutex;
Config g_config;

/* obs_module_config_path(nullptr) is the plugin's own config directory; OBS
 * creates neither it nor its parents for us. */
std::string config_file_path()
{
	char *path = obs_module_config_path("sokaster.json");
	if (!path)
		return {};

	std::string result = path;
	bfree(path);
	return result;
}

bool ensure_config_dir()
{
	char *dir = obs_module_config_path(nullptr);
	if (!dir)
		return false;

	const int rc = os_mkdirs(dir);
	bfree(dir);
	return rc != MKDIR_ERROR;
}

const char *str_or_empty(obs_data_t *data, const char *key)
{
	const char *value = obs_data_get_string(data, key);
	return value ? value : "";
}

void save_locked()
{
	if (!ensure_config_dir()) {
		obs_log(LOG_WARNING, "config: cannot create config directory");
		return;
	}

	const std::string path = config_file_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "backend_url", g_config.backend_url.c_str());
	obs_data_set_string(data, "access_key", g_config.access_key.c_str());
	obs_data_set_string(data, "source_uuid", g_config.source_uuid.c_str());
	obs_data_set_int(data, "max_width", g_config.max_width);
	obs_data_set_int(data, "max_height", g_config.max_height);
	obs_data_set_int(data, "jpeg_quality", g_config.jpeg_quality);
	obs_data_set_int(data, "vision_interval_ms", g_config.vision_interval_ms);
	obs_data_set_int(data, "audio_track", g_config.audio_track);

	/* _safe writes to a temporary and renames: a crash mid-write leaves the
	 * previous settings intact instead of an empty file. */
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "config: save failed");

	obs_data_release(data);
}

} // namespace

namespace config {

void load()
{
	const std::string path = config_file_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data) {
		obs_log(LOG_INFO, "config: no settings yet, using defaults");
		return;
	}

	Config loaded;
	loaded.backend_url = str_or_empty(data, "backend_url");
	loaded.access_key = str_or_empty(data, "access_key");
	loaded.source_uuid = str_or_empty(data, "source_uuid");

	/* Defaults double as sanity bounds: a hand-edited file must not be able
	 * to ask for a 40k-pixel frame or a one-millisecond tempo. */
	const long long max_width = obs_data_get_int(data, "max_width");
	const long long max_height = obs_data_get_int(data, "max_height");
	const long long quality = obs_data_get_int(data, "jpeg_quality");
	const long long interval = obs_data_get_int(data, "vision_interval_ms");
	const bool has_track = obs_data_has_user_value(data, "audio_track");
	const long long track = obs_data_get_int(data, "audio_track");

	if (max_width >= 160 && max_width <= 3840)
		loaded.max_width = static_cast<uint32_t>(max_width);
	if (max_height >= 90 && max_height <= 2160)
		loaded.max_height = static_cast<uint32_t>(max_height);
	if (quality >= 10 && quality <= 100)
		loaded.jpeg_quality = static_cast<int>(quality);
	if (interval >= 1000 && interval <= 600000)
		loaded.vision_interval_ms = static_cast<int>(interval);
	if (has_track && track >= 0 && track < 6)
		loaded.audio_track = static_cast<int>(track);

	obs_data_release(data);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_config = loaded;
	}

	/* The key is a credential: say whether we have one, never what it is. */
	obs_log(LOG_INFO, "config: loaded, backend=%s key=%s",
		loaded.backend_url.empty() ? "(unset)" : loaded.backend_url.c_str(),
		loaded.access_key.empty() ? "(unset)" : "(set)");
}

Config get()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_config;
}

void set(const Config &next)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_config = next;
	save_locked();
}

void set_source_uuid(const std::string &uuid)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_config.source_uuid == uuid)
		return;

	g_config.source_uuid = uuid;
	save_locked();
}

} // namespace config
} // namespace sokaster
