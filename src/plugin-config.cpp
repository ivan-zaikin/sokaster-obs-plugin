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

#include <QUrl>

#include <mutex>

namespace sokaster {
namespace {

constexpr const char *kDefaultBackendUrl = "https://sokaster.ru";

/* HTTPS, or plain HTTP to this machine for development: the access key travels
 * in a header, and never in the clear across a network. */
bool is_usable_backend(const std::string &value)
{
	const QUrl url(QString::fromStdString(value), QUrl::StrictMode);
	if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
		return false;

	if (url.scheme() == QLatin1String("https"))
		return true;
	const QString host = url.host();
	return url.scheme() == QLatin1String("http") &&
	       (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") ||
		host == QLatin1String("::1"));
}

std::mutex g_mutex;
Config g_config;

std::string config_file_path(const char *file = "sokaster.json")
{
	char *path = obs_module_config_path(file);
	if (!path)
		return {};

	std::string result = path;
	bfree(path);
	return result;
}

/* OBS creates neither the plugin's config directory nor its parents. */
bool ensure_config_dir()
{
	char *dir = obs_module_config_path(nullptr);
	const bool dir_ok = dir && os_mkdirs(dir) != MKDIR_ERROR;
	bfree(dir);

	if (!dir_ok)
		obs_log(LOG_WARNING, "config: cannot create config directory");
	return dir_ok;
}

constexpr const char *kHotkeysFile = "hotkeys.json";

void save_locked()
{
	const std::string path = config_file_path();
	if (!ensure_config_dir() || path.empty())
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "backend_url", g_config.backend_url.c_str());
	obs_data_set_string(data, "access_key", g_config.access_key.c_str());
	obs_data_set_string(data, "source_uuid", g_config.source_uuid.c_str());
	obs_data_set_int(data, "audio_track", g_config.audio_track);

	/* Writes to a temporary and renames: a crash mid-write keeps the old file. */
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "config: save failed");

	obs_data_release(data);
}

} // namespace

bool is_token(const std::string &value)
{
	if (value.size() < 16 || value.size() > 256)
		return false;
	for (const char c : value)
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		      c == '_'))
			return false;
	return true;
}

std::string Config::base_url() const
{
	std::string base = backend_url.empty() ? kDefaultBackendUrl : backend_url;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	return base;
}

namespace config {

void load()
{
	const std::string path = config_file_path();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data) {
		obs_log(LOG_INFO, "config: no settings yet, using defaults");
		return;
	}

	Config loaded;
	loaded.backend_url = obs_data_get_string(data, "backend_url");
	loaded.access_key = obs_data_get_string(data, "access_key");
	if (!loaded.backend_url.empty() && !is_usable_backend(loaded.backend_url)) {
		obs_log(LOG_WARNING, "config: backend_url must be https (or http to localhost), using the default");
		loaded.backend_url.clear();
	}
	if (!loaded.access_key.empty() && !is_token(loaded.access_key)) {
		obs_log(LOG_WARNING, "config: stored access key is malformed, signing out");
		loaded.access_key.clear();
	}
	loaded.source_uuid = obs_data_get_string(data, "source_uuid");

	const long long track = obs_data_get_int(data, "audio_track");
	if (obs_data_has_user_value(data, "audio_track") && track >= 0 && track < MAX_AUDIO_MIXES)
		loaded.audio_track = static_cast<int>(track);

	obs_data_release(data);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_config = loaded;
	}

	/* The key is a credential: say whether we have one, never what it is. */
	obs_log(LOG_INFO, "config: loaded, backend=%s key=%s", loaded.base_url().c_str(),
		loaded.configured() ? "(set)" : "(unset)");
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

obs_data_array_t *load_hotkey(const char *name)
{
	const std::string path = config_file_path(kHotkeysFile);
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		return nullptr;

	obs_data_array_t *bindings = obs_data_get_array(data, name);
	obs_data_release(data);
	return bindings;
}

void save_hotkey(const char *name, obs_data_array_t *bindings)
{
	const std::string path = config_file_path(kHotkeysFile);
	if (!ensure_config_dir() || path.empty())
		return;

	/* Read, change one key, write back: other hotkeys keep their bindings. */
	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		data = obs_data_create();
	obs_data_set_array(data, name, bindings);

	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "config: hotkey save failed");

	obs_data_release(data);
}

} // namespace config
} // namespace sokaster
