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

namespace sokaster {

/*
 * Picks the source the co-host most likely should be watching: the largest
 * game, window or display capture in the current scene.
 *
 * A guess, not a decision — the streamer confirms or changes it in the dock
 * (step 5 of the reference scenario). Returns a strong reference the caller
 * releases, or nullptr when the scene holds nothing worth watching.
 *
 * Uses the frontend API, so call it from the UI thread.
 */
obs_source_t *pick_default_source();

} // namespace sokaster
