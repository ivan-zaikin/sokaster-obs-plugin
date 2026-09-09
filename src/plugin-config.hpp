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

#include <cstdint>
#include <string>

namespace sokaster {

/*
 * Plugin settings, stored next to the module in obs_module_config_path().
 *
 * Deliberately not kept in the scene collection or the profile: the access key
 * belongs to the machine, not to a set of scenes a streamer may import, export
 * or share.
 */
struct Config {
	std::string backend_url;
	std::string access_key;

	/* The source the co-host looks at, by UUID. Names are what streamers
	 * rename; UUIDs are what survives it. */
	std::string source_uuid;

	uint32_t max_width = 1280;
	uint32_t max_height = 720;
	int jpeg_quality = 80;

	/* The OBS track the co-host listens to, 0-based; -1 until one is picked.
	 * Remembered because it is only free the first time we look: after the
	 * streamer ticks a source, the track counts as taken. */
	int audio_track = -1;

	/* Fallback tempo. The backend owns the real one and hands it back as
	 * EffectiveVisionIntervalMs; this is what we use until it answers. */
	int vision_interval_ms = 5000;

	/* Without a server and a key there is nothing to send frames to, and an
	 * unconfigured plugin must do no periodic work at all. */
	bool configured() const { return !backend_url.empty() && !access_key.empty(); }
};

namespace config {

/* Reads the config file. Missing or broken file is not an error: defaults. */
void load();

/* Thread-safe snapshot. Callers work on a copy, never on shared state. */
Config get();

/* Replaces the settings and writes them out. */
void set(const Config &next);

/* Persists only the picked source, which changes far more often than the rest. */
void set_source_uuid(const std::string &uuid);

} // namespace config

} // namespace sokaster
