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
 * Where the co-host's overlay is in the scene collection, as far as OBS can
 * tell without asking the server.
 *
 * The server knows whether an overlay is connected; only OBS knows whether it
 * sits in the scene viewers are looking at, or carries a flag that will cut it
 * off on the next scene switch. The dock needs both halves.
 */
struct OverlayPresence {
	enum class Where {
		/* No browser source points at an overlay page. */
		Missing,
		/* There is one, but not in the current scene. */
		OtherScene,
		CurrentScene,
	};

	Where where = Where::Missing;

	/* Points at an overlay page, but not with this account's token — an
	 * old address after a rotation, or someone else's. */
	bool stale = false;

	/* "Shutdown source when not visible": the page unloads on a scene switch
	 * and the co-host goes quiet until it comes back. */
	bool shutdown_when_hidden = false;
};

/* token may be empty when it is not known yet; then any overlay page counts
 * and none is stale. Frontend API inside, so UI thread only. */
OverlayPresence find_overlay(const std::string &token);

} // namespace sokaster
