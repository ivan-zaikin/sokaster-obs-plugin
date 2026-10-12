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

#include "readiness.hpp"

#include <obs-module.h>

#include <QDateTime>
#include <QLocale>

namespace sokaster {
namespace {

using Level = Readiness::Level;
using Row = Readiness::Row;
using Fix = Readiness::OverlayFix;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString format_date(const std::string &iso)
{
	const QDateTime when = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODateWithMs);
	return when.isValid() ? QLocale().toString(when.toLocalTime().date(), QLocale::ShortFormat) : QString();
}

Row account_row(const LocalFacts &local, const ServerState &server)
{
	if (!local.configured)
		return {Level::Block, text("Dock.Account.SignIn.Hint")};
	if (server.link == ServerState::Link::KeyRejected)
		return {Level::Block, text("Dock.Account.Expired")};
	if (!server.have_beta)
		return {};

	if (!server.admitted) {
		if (server.beta_status == "Pending")
			return {Level::Block, text("Dock.Account.Pending").arg(format_date(server.requested_at))};
		if (server.beta_status == "Rejected")
			return {Level::Block, text("Dock.Account.Rejected")};
		return {Level::Block, text("Dock.Account.NoApplication")};
	}

	return {Level::Ok, text("Dock.Account.Ok").arg(QString::fromStdString(server.display_name))};
}

/*
 * The server says whether an overlay is connected; OBS says where it is and
 * what will cut it off. Connected wins: if the co-host can be heard, the rest
 * is advice.
 */
Row overlay_row(const LocalFacts &local, const ServerState &server, Fix &fix)
{
	const OverlayPresence &o = local.overlay;
	const bool known = server.have_beta && server.link == ServerState::Link::Ok;
	fix = Fix::None;

	if (known && server.overlay_connected) {
		/* First: a browser tab opened to hear the voice is the likeliest
		 * duplicate, and it goes away once OBS plays the voice itself. */
		switch (o.sound) {
		case OverlayPresence::Sound::Unheard:
			fix = Fix::Listen;
			return {Level::Warn, text("Dock.Overlay.Unheard")};
		case OverlayPresence::Sound::Doubled:
			fix = Fix::Sound;
			return {Level::Warn, text("Dock.Overlay.Doubled")};
		case OverlayPresence::Sound::OffAir:
			fix = Fix::Sound;
			return {Level::Warn, text("Dock.Overlay.OffAir")};
		case OverlayPresence::Sound::Ok:
			break;
		}
		/* No button for a duplicate: which copy to remove is the streamer's call. */
		if (server.overlay_count > 1)
			return {Level::Warn, text("Dock.Overlay.Duplicate").arg(server.overlay_count)};
		if (o.shutdown_when_hidden)
			return {Level::Warn, text("Dock.Overlay.Shutdown")};
		if (o.where == OverlayPresence::Where::OtherScene) {
			fix = Fix::Add;
			return {Level::Warn, text("Dock.Overlay.OtherScene")};
		}
		return {Level::Ok, text("Dock.Overlay.Ok")};
	}

	if (o.where == OverlayPresence::Where::Missing) {
		fix = Fix::Add;
		return {Level::Block, text("Dock.Overlay.Missing")};
	}
	if (o.stale) {
		fix = Fix::Update;
		return {Level::Block, text("Dock.Overlay.Stale")};
	}
	if (!known)
		return {};

	/* Present but not connected. Once the stream has moved to another scene,
	 * the flag is the likeliest reason. */
	if (o.shutdown_when_hidden && o.where == OverlayPresence::Where::OtherScene)
		return {Level::Block, text("Dock.Overlay.Shutdown")};
	return {Level::Block, text("Dock.Overlay.Disconnected")};
}

Row eyes_row(const LocalFacts &local, const ServerState &server)
{
	if (!local.eyes_selected) {
		if (local.no_capture_sources || local.eyes_name.isEmpty())
			return {Level::Block, text("Dock.Eyes.None")};
		return {Level::Block, text("Dock.Eyes.Gone").arg(local.eyes_name)};
	}

	/* Off air nothing is captured; the selection is all there is to check. */
	if (!local.streaming)
		return {Level::Ok, QString()};

	switch (local.vision.state) {
	case VisionHealth::State::NoSource:
		return {Level::Block, text("Dock.Eyes.Gone").arg(local.eyes_name)};
	case VisionHealth::State::Empty:
		return {Level::Warn, text("Dock.Eyes.Empty").arg(local.eyes_name)};
	case VisionHealth::State::Retrying:
		return {Level::Block, text("Dock.Eyes.Retry").arg(local.vision.retry_seconds)};
	case VisionHealth::State::RateLimited:
		return {Level::Warn, text("Dock.Eyes.Slow").arg(local.vision.retry_seconds)};
	case VisionHealth::State::KeyRejected:
		/* Said once, on the account row. */
		return {Level::Block, QString()};
	default:
		break;
	}

	/* The worker thinks it is fine, but the server has not seen a frame in a
	 * while: the same problem from the other end. */
	if (server.link == ServerState::Link::Ok && server.run_state == "DesktopOffline")
		return {Level::Block, text("Dock.Eyes.NotArriving")};
	return {Level::Ok, QString()};
}

Row ears_row(const LocalFacts &local)
{
	if (!local.configured)
		return {};
	if (local.audio_track < 0)
		return {Level::Warn, text("Dock.Hears.NoTrack")};

	switch (local.audio.state) {
	case AudioHealth::State::SpeechDisabled:
		return {Level::Warn, text("Dock.Hears.Disabled")};
	case AudioHealth::State::VadFailed:
		return {Level::Warn, text("Dock.Hears.VadFailed")};
	case AudioHealth::State::Retrying:
		return {Level::Warn, text("Dock.Hears.Retry").arg(local.audio.retry_seconds)};
	default:
		break;
	}

	if (!local.hearing_anything)
		return {Level::Warn, text("Dock.Hears.Nothing")};
	return {Level::Ok, text("Dock.Hears.Track").arg(local.audio_track + 1)};
}

Row air_row(const LocalFacts &local, const ServerState &server)
{
	if (!server.have_beta || server.link != ServerState::Link::Ok)
		return {};

	const std::string &state = server.run_state;
	if (state == "BudgetExhausted")
		return {Level::Block,
			text("Dock.Air.Budget").arg(server.spent_usd, 0, 'f', 2).arg(server.budget_usd, 0, 'f', 2)};
	if (server.pending_orders > 0)
		return {Level::Warn, text("Dock.Air.Pending").arg(server.pending_orders)};
	if (!local.streaming)
		return {Level::Neutral, text("Dock.Air.NotStreaming")};
	if (state == "SessionStopped")
		return {Level::Warn, text("Dock.Air.Reconnecting")};
	if (state == "NoCredit")
		return {Level::Neutral, text("Dock.Air.NoCredit")};
	if (state == "NoOrder")
		return {Level::Neutral, text("Dock.Air.NoOrder")};
	if (state == "Ok")
		return {Level::Ok, text("Dock.Air.Talking")};

	/* NotApproved, NoOverlay, DesktopOffline: said on their own rows. */
	return {};
}

} // namespace

Readiness evaluate_readiness(const LocalFacts &local, const ServerState &server, uint64_t now_ns)
{
	Readiness r;
	r.account = account_row(local, server);
	r.overlay = overlay_row(local, server, r.overlay_fix);
	r.eyes = eyes_row(local, server);
	r.ears = ears_row(local);
	r.air = air_row(local, server);
	r.ask_to_sign_in = !local.configured || server.link == ServerState::Link::KeyRejected;

	/* Air is not a setup step: a spent budget is not something to set up. */
	int blocking = 0;
	for (const Row *row : {&r.account, &r.overlay, &r.eyes})
		if (row->level == Level::Block)
			++blocking;

	if (server.link == ServerState::Link::Offline) {
		const uint64_t left_ns = server.retry_at_ns > now_ns ? server.retry_at_ns - now_ns : 0;
		r.headline = text("Dock.Ready.Offline").arg((left_ns + 999999999ull) / 1000000000ull);
	} else if (blocking > 0) {
		r.headline = text("Dock.Ready.Setup").arg(blocking);
	} else if (r.air.level == Level::Block) {
		r.headline = text("Dock.Ready.Stopped");
	} else if (server.run_state == "Ok" && local.streaming) {
		r.headline = text("Dock.Ready.OnAir");
	} else if (server.have_beta) {
		r.headline = text("Dock.Ready.Idle");
	}
	return r;
}

} // namespace sokaster
