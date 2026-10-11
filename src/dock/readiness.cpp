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

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

Row row(Level level, const QString &message)
{
	return Row{level, message};
}

int seconds_until(uint64_t at_ns, uint64_t now_ns)
{
	return at_ns > now_ns ? static_cast<int>((at_ns - now_ns + 999999999ull) / 1000000000ull) : 0;
}

QString format_date(const std::string &iso)
{
	const QDateTime when = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODateWithMs);
	if (!when.isValid())
		return QString();
	return QLocale().toString(when.toLocalTime().date(), QLocale::ShortFormat);
}

Row account_row(const LocalFacts &local, const ServerState &server)
{
	if (!local.configured)
		return row(Level::Block, text("Dock.Account.SignIn.Hint"));

	if (server.link == ServerState::Link::KeyRejected)
		return row(Level::Block, text("Dock.Account.Expired"));

	if (!server.have_beta)
		return row(Level::Neutral, QString());

	if (!server.admitted) {
		if (server.beta_status == "Pending")
			return row(Level::Block, text("Dock.Account.Pending").arg(format_date(server.requested_at)));
		if (server.beta_status == "Rejected")
			return row(Level::Block, text("Dock.Account.Rejected"));
		return row(Level::Block, text("Dock.Account.NoApplication"));
	}

	return row(Level::Ok, text("Dock.Account.Ok").arg(QString::fromStdString(server.display_name)));
}

/*
 * The server says whether an overlay is connected; OBS says where it is and
 * what will cut it off. Connected wins: if the co-host can be heard, the rest
 * is advice.
 */
Row overlay_row(const LocalFacts &local, const ServerState &server, Readiness::OverlayFix *fix)
{
	using Fix = Readiness::OverlayFix;

	const OverlayPresence &o = local.overlay;
	const bool known = server.have_beta && server.link == ServerState::Link::Ok;
	*fix = Fix::None;

	if (known && server.overlay_connected) {
		/* Heard twice right now beats cut off at the next scene switch.
		 * No button: which copy to remove is the streamer's call. */
		if (server.overlay_count > 1)
			return row(Level::Warn, text("Dock.Overlay.Duplicate").arg(server.overlay_count));
		if (o.shutdown_when_hidden) {
			*fix = Fix::KeepRunning;
			return row(Level::Warn, text("Dock.Overlay.Shutdown"));
		}
		if (o.where == OverlayPresence::Where::OtherScene) {
			*fix = Fix::Add;
			return row(Level::Warn, text("Dock.Overlay.OtherScene"));
		}
		return row(Level::Ok, text("Dock.Overlay.Ok"));
	}

	if (o.where == OverlayPresence::Where::Missing) {
		*fix = Fix::Add;
		return row(Level::Block, text("Dock.Overlay.Missing"));
	}
	if (o.stale) {
		*fix = Fix::Update;
		return row(Level::Block, text("Dock.Overlay.Stale"));
	}
	if (!known)
		return row(Level::Neutral, QString());

	/* Just added or re-pointed: the page takes a few seconds to load and
	 * connect, and "not connected" in that time reads like a failure. */
	if (local.overlay_placing)
		return row(Level::Neutral, text("Dock.Overlay.Connecting"));

	/* Present in OBS but not connected. The flag is the likeliest reason
	 * once the stream has moved to another scene. */
	if (o.shutdown_when_hidden && o.where == OverlayPresence::Where::OtherScene) {
		*fix = Fix::KeepRunning;
		return row(Level::Block, text("Dock.Overlay.Shutdown"));
	}
	return row(Level::Block, text("Dock.Overlay.Disconnected"));
}

Row eyes_row(const LocalFacts &local, const ServerState &server)
{
	if (!local.eyes_selected) {
		if (local.no_capture_sources || local.eyes_name.isEmpty())
			return row(Level::Block, text("Dock.Eyes.None"));
		return row(Level::Block, text("Dock.Eyes.Gone").arg(local.eyes_name));
	}

	/* Off air the worker is idle and the server expects no frames; the
	 * selection itself is all there is to check. */
	if (!local.streaming)
		return row(Level::Ok, QString());

	switch (local.vision.state) {
	case VisionHealth::State::NoSource:
		return row(Level::Block, text("Dock.Eyes.Gone").arg(local.eyes_name));
	case VisionHealth::State::Empty:
		return row(Level::Warn, text("Dock.Eyes.Empty").arg(local.eyes_name));
	case VisionHealth::State::Retrying:
		return row(Level::Block, text("Dock.Eyes.Retry").arg(local.vision.retry_seconds));
	case VisionHealth::State::RateLimited:
		return row(Level::Warn, text("Dock.Eyes.Slow").arg(local.vision.retry_seconds));
	case VisionHealth::State::KeyRejected:
		/* Said once, on the account row. */
		return row(Level::Block, QString());
	default:
		break;
	}

	/* The worker thinks it is fine, but the server has not seen a frame in
	 * a while: the same thing from the other end. */
	if (server.link == ServerState::Link::Ok && server.run_state == "DesktopOffline")
		return row(Level::Block, text("Dock.Eyes.NotArriving"));

	return row(Level::Ok, QString());
}

Row ears_row(const LocalFacts &local)
{
	if (local.audio_track < 0)
		return row(Level::Warn, text("Dock.Hears.NoTrack"));

	switch (local.audio.state) {
	case AudioHealth::State::SpeechDisabled:
		return row(Level::Warn, text("Dock.Hears.Disabled"));
	case AudioHealth::State::VadFailed:
		return row(Level::Warn, text("Dock.Hears.VadFailed"));
	case AudioHealth::State::Retrying:
		return row(Level::Warn, text("Dock.Hears.Retry").arg(local.audio.retry_seconds));
	default:
		break;
	}

	if (!local.hearing_anything)
		return row(Level::Warn, text("Dock.Hears.Nothing"));

	return row(Level::Ok, text("Dock.Hears.Track").arg(local.audio_track + 1));
}

const QueuedOrder *on_air_order(const ServerState &server)
{
	for (const QueuedOrder &order : server.orders)
		if (order.state == "onair")
			return &order;
	return nullptr;
}

Row air_row(const LocalFacts &local, const ServerState &server, uint64_t now_ns, bool *offer_donate)
{
	*offer_donate = false;

	if (!server.have_beta || server.link != ServerState::Link::Ok)
		return row(Level::Neutral, QString());

	const std::string &state = server.run_state;

	if (state == "BudgetExhausted")
		return row(Level::Block,
			   text("Dock.Air.Budget").arg(server.spent_usd, 0, 'f', 2).arg(server.budget_usd, 0, 'f', 2));

	if (!local.streaming)
		return row(Level::Neutral, text("Dock.Air.NotStreaming"));

	if (state == "SessionStopped")
		return row(Level::Warn, text("Dock.Air.Reconnecting"));

	if (state == "NoCredit")
		return row(Level::Neutral, text("Dock.Air.NoCredit"));

	if (state == "NoOrder") {
		*offer_donate = !server.donate_url.empty();
		return row(Level::Neutral, text("Dock.Air.NoOrder"));
	}

	if (state == "Ok") {
		if (const QueuedOrder *order = on_air_order(server)) {
			const int elapsed = static_cast<int>((now_ns - server.orders_received_ns) / 1000000000ull);
			const int left = order->seconds_left > elapsed ? order->seconds_left - elapsed : 0;
			return row(Level::Ok,
				   text("Dock.Air.OnAir")
					   .arg(QString::fromStdString(order->display_name), format_clock(left)));
		}
		return row(Level::Ok, text("Dock.Ready.OnAir"));
	}

	/* NotApproved, NoOverlay, DesktopOffline: said on their own rows. */
	return row(Level::Neutral, QString());
}

} // namespace

QString format_clock(int seconds)
{
	if (seconds < 0)
		seconds = 0;
	return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

Readiness evaluate_readiness(const LocalFacts &local, const ServerState &server, uint64_t now_ns)
{
	Readiness r;

	r.account = account_row(local, server);
	r.overlay = overlay_row(local, server, &r.overlay_fix);
	r.eyes = eyes_row(local, server);
	r.ears = ears_row(local);
	r.air = air_row(local, server, now_ns, &r.offer_donate_link);

	r.ask_for_key = !local.configured || server.link == ServerState::Link::KeyRejected;

	/* Air is not counted among the steps: a spent budget is not something the
	 * streamer can set up, and the headline says it on its own. */
	int blocking = 0;
	for (const Row *each : {&r.account, &r.overlay, &r.eyes})
		if (each->level == Level::Block)
			++blocking;

	/* Rows still finding out count as not ready: an order typed while the
	 * server has not answered yet would only bounce. */
	const bool all_known = r.account.level == Level::Ok && r.overlay.level != Level::Neutral;
	r.order_enabled = blocking == 0 && all_known && r.air.level != Level::Block;

	if (server.link == ServerState::Link::Offline)
		r.headline = text("Dock.Ready.Offline").arg(seconds_until(server.retry_at_ns, now_ns));
	else if (blocking > 0)
		r.headline = text("Dock.Ready.Setup").arg(blocking);
	else if (r.air.level == Level::Block)
		r.headline = text("Dock.Ready.Stopped");
	else if (server.run_state == "Ok" && local.streaming)
		r.headline = text("Dock.Ready.OnAir");
	else if (server.have_beta)
		r.headline = text("Dock.Ready.Idle");

	return r;
}

} // namespace sokaster
