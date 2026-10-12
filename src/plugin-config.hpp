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

#include <string>

namespace sokaster {

/*
 * Plugin settings, stored in obs_module_config_path().
 *
 * Deliberately not kept in the scene collection or the profile: the access key
 * belongs to the machine, not to a set of scenes a streamer may share.
 */
struct Config {
	/* Empty means the public service. Only ever set by hand, for development. */
	std::string backend_url;
	std::string access_key;

	/* The source the co-host looks at, by UUID: names are what streamers
	 * rename, UUIDs are what survives it. */
	std::string source_uuid;

	/* The OBS track the co-host listens to, 0-based; -1 until one is picked. */
	int audio_track = -1;

	/* An unconfigured plugin does no periodic work and touches nothing. */
	bool configured() const { return !access_key.empty(); }

	/* The server, without a trailing slash. */
	std::string base_url() const;
};

/* Access keys and overlay tokens are base64url. Whatever else arrives from the
 * network or the settings file is refused before it reaches a header or a URL. */
bool is_token(const std::string &value);

namespace config {

/* Reads the config file. A missing or broken file is not an error: defaults. */
void load();

/* Thread-safe snapshot. */
Config get();

/* Replaces the settings and writes them out. */
void set(const Config &next);

} // namespace config

} // namespace sokaster
