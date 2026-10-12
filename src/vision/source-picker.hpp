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

#include <obs.h>

#include <string>
#include <vector>

namespace sokaster {

struct CaptureSourceInfo {
	std::string uuid;
	std::string name;
};

/* Every game, window and display capture in the current scene, in scene
 * order: what the "Watches" list offers. Frontend API, UI thread only. */
std::vector<CaptureSourceInfo> list_capture_sources();

/*
 * A guess at what the co-host should watch: the largest capture in the current
 * scene, or the first one while none has a size yet (captures report one only
 * after their first frame). The streamer confirms or changes it in the dock.
 *
 * Returns a strong reference the caller releases, or nullptr. UI thread only.
 */
obs_source_t *pick_default_source();

} // namespace sokaster
