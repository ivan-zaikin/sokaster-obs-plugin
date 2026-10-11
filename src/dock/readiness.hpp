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

#include "overlay-finder.hpp"
#include "status-loop.hpp"
#include "audio/audio-loop.hpp"
#include "vision/vision-loop.hpp"

#include <QString>

namespace sokaster {

/* Everything OBS itself can tell the panel, gathered on the UI thread. */
struct LocalFacts {
	bool configured = false;
	bool streaming = false;

	bool eyes_selected = false;
	QString eyes_name;
	/* No game, window or display capture in the current scene at all. */
	bool no_capture_sources = false;
	VisionHealth vision;

	int audio_track = -1;
	bool hearing_anything = false;
	AudioHealth audio;

	OverlayPresence overlay;
	/* The streamer just added the overlay or fixed its address, and the
	 * page is still loading: say so instead of "not connected". */
	bool overlay_placing = false;
};

/*
 * The readiness panel, worked out from scratch on every refresh.
 *
 * The rule for the order field: it is locked by exactly what closes the gate
 * on the server — sign-in and beta admission, a connected overlay, frames
 * arriving. Hearing is a warning only: the gate does not depend on speech, and
 * a co-host that cannot hear still watches and talks.
 */
struct Readiness {
	enum class Level {
		/* Nothing to say, or still finding out. */
		Neutral,
		Ok,
		/* Works, but not as well as it could. Does not lock the order. */
		Warn,
		/* The co-host stays silent until this is fixed. Locks the order. */
		Block,
	};

	struct Row {
		Level level = Level::Neutral;
		QString text;
	};

	/* The one fix the overlay row offers, as a button under it. */
	enum class OverlayFix {
		None,
		/* Into the current scene: ours from another scene, or a new one. */
		Add,
		/* The overlay points at an old token. */
		Update,
		/* Clear "Shutdown source when not visible". */
		KeepRunning,
	};

	QString headline;
	Row account;
	Row overlay;
	OverlayFix overlay_fix = OverlayFix::None;
	Row eyes;
	Row ears;
	Row air;

	bool order_enabled = false;

	/* Show the key field: no key yet, or the server turned it down. */
	bool ask_for_key = false;
	/* Show "Copy donation link": the queue is empty, and that is the
	 * moment the link is worth something. */
	bool offer_donate_link = false;
};

/* now_ns is os_gettime_ns(), for the countdowns. */
Readiness evaluate_readiness(const LocalFacts &local, const ServerState &server, uint64_t now_ns);

/* m:ss, the way the queue and the order field show time. */
QString format_clock(int seconds);

} // namespace sokaster
