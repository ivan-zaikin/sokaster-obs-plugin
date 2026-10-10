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

#include "status-loop.hpp"

#include "plugin-config.hpp"

#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <chrono>

namespace sokaster {
namespace {

/* On air the queue moves: orders arrive, timers run out, a pending order has
 * three minutes for a decision. Five seconds is what the desktop polled at. */
constexpr int kOnAirPollMs = 5000;

/* Off air nobody is watching the dock, and the queue only changes when
 * someone orders. */
constexpr int kOffAirPollMs = 30000;

constexpr int kMaxBackoffMs = 60000;

QJsonObject parse_object(const std::string &body)
{
	const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body));
	return doc.isObject() ? doc.object() : QJsonObject();
}

std::string str(const QJsonObject &o, const char *key)
{
	return o.value(QLatin1String(key)).toString().toStdString();
}

int integer(const QJsonObject &o, const char *key)
{
	return o.value(QLatin1String(key)).toInt();
}

/* The server answers refusals as {"error": "..."}; anything else has no text
 * worth showing, and the dock falls back to its own words. */
std::string error_text(const HttpResult &result)
{
	return str(parse_object(result.body), "error");
}

std::string encode_path_segment(const std::string &segment)
{
	return QUrl::toPercentEncoding(QString::fromStdString(segment)).toStdString();
}

QueuedOrder parse_order(const QJsonObject &o)
{
	QueuedOrder order;
	order.id = str(o, "id");
	order.state = str(o, "state");
	order.from = str(o, "from");
	order.prompt = str(o, "prompt");
	order.display_name = str(o, "displayName");
	order.risk = integer(o, "risk");
	order.seconds_left = integer(o, "secondsLeft");
	order.position = integer(o, "position");
	order.wait_seconds = integer(o, "waitSeconds");
	order.hold_seconds_left = integer(o, "holdSecondsLeft");
	order.own = o.value(QLatin1String("own")).toBool();

	for (const QJsonValue &category : o.value(QLatin1String("categories")).toArray())
		order.categories.push_back(category.toString().toStdString());

	return order;
}

} // namespace

StatusLoop &StatusLoop::instance()
{
	static StatusLoop loop;
	return loop;
}

void StatusLoop::set_sinks(StateSink state_sink, CommandSink command_sink)
{
	state_sink_ = std::move(state_sink);
	command_sink_ = std::move(command_sink);
}

void StatusLoop::start()
{
	if (running_)
		return;

	stop_requested_ = false;
	reconfigure_requested_ = true;
	running_ = true;
	worker_ = std::thread([this] { run(); });
}

void StatusLoop::stop()
{
	if (!running_)
		return;

	stop_requested_ = true;
	client_.cancel();
	cv_.notify_all();

	if (worker_.joinable())
		worker_.join();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		commands_.clear();
	}

	running_ = false;
}

void StatusLoop::reconfigure()
{
	reconfigure_requested_ = true;
	/* Whatever is on the wire was asked with the old key. */
	client_.cancel();
	cv_.notify_all();
}

void StatusLoop::poke()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		poke_requested_ = true;
	}
	cv_.notify_all();
}

void StatusLoop::set_streaming(bool streaming)
{
	streaming_ = streaming;
	poke();
}

void StatusLoop::enqueue(std::function<void()> command)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		commands_.push_back(std::move(command));
	}
	cv_.notify_all();
}

void StatusLoop::report_command(const CommandResult &result)
{
	if (command_sink_)
		command_sink_(result);
}

void StatusLoop::place_order(const std::string &prompt, const std::string &voice)
{
	enqueue([this, prompt, voice] {
		QJsonObject body;
		body.insert(QLatin1String("prompt"), QString::fromStdString(prompt));
		if (!voice.empty())
			body.insert(QLatin1String("voice"), QString::fromStdString(voice));

		const std::string json = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
		const HttpResult http = client_.post_json("/api/persona/order", json);

		CommandResult result;
		result.kind = CommandResult::Kind::PlaceOrder;
		result.status = http.status;
		result.ok = http.ok();
		if (http.ok()) {
			const QJsonObject o = parse_object(http.body);
			result.state = str(o, "state");
			result.position = integer(o, "position");
			result.wait_seconds = integer(o, "waitSeconds");
		} else {
			result.error = error_text(http);
		}
		report_command(result);
	});
}

void StatusLoop::approve(const std::string &order_id)
{
	enqueue([this, order_id] {
		const HttpResult http =
			client_.post_json("/api/orders/" + encode_path_segment(order_id) + "/approve", "{}");

		CommandResult result;
		result.kind = CommandResult::Kind::Approve;
		result.status = http.status;
		result.ok = http.ok();
		if (http.ok()) {
			const QJsonObject o = parse_object(http.body);
			result.state = str(o, "status");
			result.position = integer(o, "position");
			result.wait_seconds = integer(o, "waitSeconds");
		} else {
			result.error = error_text(http);
		}
		report_command(result);
	});
}

void StatusLoop::reject(const std::string &order_id)
{
	enqueue([this, order_id] {
		const HttpResult http =
			client_.post_json("/api/orders/" + encode_path_segment(order_id) + "/reject", "{}");

		CommandResult result;
		result.kind = CommandResult::Kind::Reject;
		result.status = http.status;
		result.ok = http.ok();
		if (!http.ok())
			result.error = error_text(http);
		report_command(result);
	});
}

void StatusLoop::drop_on_air()
{
	enqueue([this] {
		const HttpResult http = client_.del("/api/persona/donation");

		CommandResult result;
		result.kind = CommandResult::Kind::Drop;
		result.status = http.status;
		result.ok = http.ok();
		if (http.ok())
			result.dropped = parse_object(http.body).value(QLatin1String("dropped")).toBool();
		else
			result.error = error_text(http);
		report_command(result);
	});
}

void StatusLoop::fetch_account()
{
	const HttpResult me = client_.get("/api/user/me");
	if (me.ok()) {
		const QJsonObject o = parse_object(me.body);
		state_.display_name = str(o, "twitchDisplayName");
		if (state_.display_name.empty())
			state_.display_name = str(o, "name");
	}

	const HttpResult voices = client_.get("/api/persona/voices");
	if (voices.ok()) {
		state_.voices.clear();
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(voices.body));
		for (const QJsonValue &v : doc.array()) {
			const QJsonObject o = v.toObject();
			state_.voices.push_back({str(o, "id"), str(o, "hint")});
		}
	}

	const HttpResult token = client_.get("/api/overlay-token");
	if (token.ok())
		state_.overlay_token = str(parse_object(token.body), "token");

	/* Only a complete set counts: a voice list missing because the network
	 * blinked would otherwise stay missing for the rest of the session. */
	account_fetched_ = me.ok() && voices.ok() && token.ok();
}

void StatusLoop::poll()
{
	if (!config::get().configured()) {
		state_ = ServerState();
		state_.link = ServerState::Link::Unconfigured;
		return;
	}

	if (!account_fetched_)
		fetch_account();

	const HttpResult beta = client_.get("/api/beta/me");
	if (stop_requested_ || reconfigure_requested_)
		return;

	if (beta.status == 401 || beta.status == 403) {
		/* A rejected key stays rejected; the dock asks for a new one. */
		state_.link = ServerState::Link::KeyRejected;
		account_fetched_ = false;
		return;
	}

	if (!beta.ok()) {
		++consecutive_failures_;
		const int shift = std::min(consecutive_failures_ - 1, 4);
		const int base = streaming_ ? kOnAirPollMs : kOffAirPollMs;
		const int backoff = std::min(base << shift, kMaxBackoffMs);

		state_.link = ServerState::Link::Offline;
		state_.retry_at_ns = os_gettime_ns() + static_cast<uint64_t>(backoff) * 1000000ull;
		state_.orders_stale = state_.have_orders;
		obs_log(LOG_DEBUG, "status: poll failed (%ld), retry in %d ms", beta.status, backoff);
		return;
	}

	consecutive_failures_ = 0;
	state_.link = ServerState::Link::Ok;
	state_.retry_at_ns = 0;

	const QJsonObject o = parse_object(beta.body);
	state_.have_beta = true;
	state_.beta_status = str(o, "status");
	state_.requested_at = str(o, "requestedAt");
	state_.admitted = o.value(QLatin1String("admitted")).toBool();
	state_.run_state = str(o, "runState");
	state_.budget_usd = o.value(QLatin1String("budgetUsd")).toDouble();
	state_.spent_usd = o.value(QLatin1String("spentUsd")).toDouble();
	state_.donate_url = str(o, "donateUrl");
	state_.overlay_connected = o.value(QLatin1String("overlayConnected")).toBool();

	const HttpResult orders = client_.get("/api/orders");
	if (orders.ok()) {
		state_.orders.clear();
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(orders.body));
		for (const QJsonValue &v : doc.array())
			state_.orders.push_back(parse_order(v.toObject()));

		state_.have_orders = true;
		state_.orders_stale = false;
		state_.orders_received_ns = os_gettime_ns();
	} else {
		/* The list stays as it was: "the network blinked" is not "the
		 * queue is empty". */
		state_.orders_stale = state_.have_orders;
	}
}

void StatusLoop::run()
{
	os_set_thread_name("sokaster-status");

	while (!stop_requested_) {
		if (reconfigure_requested_.exchange(false)) {
			const Config cfg = config::get();
			client_.configure(cfg.backend_url, cfg.access_key);
			state_ = ServerState();
			state_.link = cfg.configured() ? ServerState::Link::Connecting
						       : ServerState::Link::Unconfigured;
			account_fetched_ = false;
			consecutive_failures_ = 0;

			if (state_sink_)
				state_sink_(state_);
		}

		/* Clicks first: the streamer is waiting on those, and the poll that
		 * follows shows their effect. */
		std::deque<std::function<void()>> commands;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			commands.swap(commands_);
			poke_requested_ = false;
		}

		if (!config::get().configured()) {
			/* Nothing to run them against. Dropped rather than kept: an
			 * order typed before a key existed should not fire later on
			 * its own. */
			commands.clear();
		}

		for (auto &command : commands) {
			if (stop_requested_)
				break;
			command();
		}

		if (stop_requested_)
			break;

		poll();

		if (stop_requested_)
			break;
		if (state_sink_ && !reconfigure_requested_)
			state_sink_(state_);

		int wait_ms = streaming_ ? kOnAirPollMs : kOffAirPollMs;
		if (state_.link == ServerState::Link::Offline) {
			const uint64_t now = os_gettime_ns();
			wait_ms = state_.retry_at_ns > now ? static_cast<int>((state_.retry_at_ns - now) / 1000000ull)
							   : 0;
		}

		std::unique_lock<std::mutex> lock(mutex_);
		const auto wake = [this] {
			return stop_requested_ || reconfigure_requested_ || poke_requested_ || !commands_.empty();
		};

		if (state_.link == ServerState::Link::Unconfigured || state_.link == ServerState::Link::KeyRejected) {
			/* Idle until the streamer gives us a key: an unconfigured
			 * plugin does no periodic work. */
			cv_.wait(lock, [this] { return stop_requested_ || reconfigure_requested_.load(); });
		} else {
			cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), wake);
		}

		client_.clear_cancel();
	}
}

} // namespace sokaster
