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
};

/*
 * The readiness panel, worked out from scratch on every refresh.
 *
 * A row blocks exactly when the server's gate would keep the co-host silent:
 * sign-in and beta admission, a connected overlay, frames arriving. Hearing is
 * a warning only — a co-host that cannot hear still watches and talks.
 */
struct Readiness {
	enum class Level {
		/* Nothing to say, or still finding out. */
		Neutral,
		Ok,
		/* Works, but not as well as it could. */
		Warn,
		/* The co-host stays silent until this is fixed. */
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
	};

	QString headline;
	Row account;
	Row overlay;
	OverlayFix overlay_fix = OverlayFix::None;
	Row eyes;
	Row ears;
	Row air;

	/* No key yet, or the server turned it down. */
	bool ask_to_sign_in = false;
};

/* now_ns is os_gettime_ns(), for the retry countdown. */
Readiness evaluate_readiness(const LocalFacts &local, const ServerState &server, uint64_t now_ns);

} // namespace sokaster
