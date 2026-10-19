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

	/* Whether the voice reaches both the streamer and the stream, judged
	 * only for an overlay whose audio OBS controls — without that, the page
	 * plays straight to the speakers like any other app. */
	enum class Sound {
		Ok,
		/* Not monitored: viewers hear the co-host, the streamer does not,
		 * and opens the overlay in a browser tab — a second copy. */
		Unheard,
		/* Monitored and sent out, while a desktop audio capture on an output
		 * track picks up the monitoring device: viewers hear it twice. */
		Doubled,
		/* Monitored only, and no such capture: only the streamer hears it. */
		OffAir,
	};
	Sound sound = Sound::Ok;

	/* The browser source all of the above is about; empty when Missing.
	 * The fixes below act on exactly this one, never on every overlay in the
	 * collection — two connected copies would play the voice twice. */
	std::string source_uuid;
};

/* Only browser sources showing base_url's overlay pages count. token may be
 * empty when it is not known yet; then any of them counts and none is stale.
 * Frontend API inside, so UI thread only. */
OverlayPresence find_overlay(const std::string &base_url, const std::string &token);

/* The address of this account's overlay page: <base_url>/overlay/<token>. */
std::string overlay_url(const std::string &base_url, const std::string &token);

/*
 * Puts the overlay into the current scene. An overlay of ours found in another
 * scene is added as a reference to the same source — one source is one page and
 * one connection, so the voice is heard once. Otherwise a new browser source is
 * made: fitted to the canvas, its audio controlled by OBS and monitored as
 * fix_overlay_sound() would, kept running when hidden, and left off
 * listen_track so the co-host does not hear itself.
 *
 * Only on the streamer's click: the plugin never edits a scene on its own.
 * listen_track is -1 when there is none. False when nothing could be done.
 */
bool add_overlay_to_current_scene(const OverlayPresence &found, const std::string &url, const char *new_name,
				  int listen_track);

/* Points the given overlay source at url. */
bool set_overlay_url(const std::string &source_uuid, const std::string &url);

/* Monitors the given overlay source so the streamer hears the voice, and sends
 * it out unless a desktop audio capture already carries the monitoring device
 * to the stream. */
bool fix_overlay_sound(const std::string &source_uuid);

/* Reloads the given overlay page, as the browser source's own "Refresh cache
 * of current page" button does: a page that lost its connection gets a new one. */
bool reload_overlay(const std::string &source_uuid);

} // namespace sokaster
