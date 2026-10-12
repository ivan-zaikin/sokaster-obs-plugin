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

#include <algorithm>
#include <chrono>

namespace sokaster {
namespace {

/* On air a pending order has minutes for a decision; off air nobody watches. */
constexpr int kOnAirPollMs = 5000;
constexpr int kOffAirPollMs = 30000;
constexpr int kMaxBackoffMs = 60000;

QJsonDocument parse(const std::string &body)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(body));
}

std::string str(const QJsonObject &o, const char *key)
{
	return o.value(QLatin1String(key)).toString().toStdString();
}

} // namespace

StatusLoop &StatusLoop::instance()
{
	static StatusLoop loop;
	return loop;
}

void StatusLoop::start(Sink sink)
{
	if (worker_.joinable())
		return;

	sink_ = std::move(sink);
	stop_ = false;
	reconfigure_ = true;
	worker_ = std::thread([this] { run(); });
}

void StatusLoop::stop()
{
	if (!worker_.joinable())
		return;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	client_.cancel();
	cv_.notify_all();
	worker_.join();
	sink_ = nullptr;
}

void StatusLoop::reconfigure()
{
	/* Cancel first: whatever is on the wire was asked with the old key, and
	 * the worker lifts the cancel when it picks up the new one. */
	client_.cancel();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		reconfigure_ = true;
	}
	cv_.notify_all();
}

void StatusLoop::poke()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		poke_ = true;
	}
	cv_.notify_all();
}

void StatusLoop::set_streaming(bool streaming)
{
	streaming_ = streaming;
	poke();
}

int StatusLoop::poll_interval_ms() const
{
	const int base = streaming_ ? kOnAirPollMs : kOffAirPollMs;
	if (state_.link != ServerState::Link::Offline)
		return base;
	return std::min(base << std::min(failures_ - 1, 4), kMaxBackoffMs);
}

void StatusLoop::fetch_account()
{
	const HttpResult me = client_.get("/api/user/me");
	if (me.ok()) {
		const QJsonObject o = parse(me.body).object();
		state_.display_name = str(o, "twitchDisplayName");
		if (state_.display_name.empty())
			state_.display_name = str(o, "name");
	}

	/* The token goes into a browser source's URL: nothing but a token does. */
	const HttpResult token = client_.get("/api/overlay-token");
	const std::string overlay_token = token.ok() ? str(parse(token.body).object(), "token") : std::string();
	if (is_token(overlay_token))
		state_.overlay_token = overlay_token;

	account_fetched_ = me.ok() && is_token(overlay_token);
}

void StatusLoop::poll()
{
	if (!account_fetched_)
		fetch_account();

	const HttpResult beta = client_.get("/api/beta/me");
	if (beta.status == 401 || beta.status == 403) {
		/* A rejected key stays rejected; the dock asks to sign in again. */
		state_.link = ServerState::Link::KeyRejected;
		account_fetched_ = false;
		return;
	}

	if (!beta.ok()) {
		++failures_;
		state_.link = ServerState::Link::Offline;
		state_.retry_at_ns = os_gettime_ns() + static_cast<uint64_t>(poll_interval_ms()) * 1000000ull;
		obs_log(LOG_DEBUG, "status: poll failed (%ld)", beta.status);
		return;
	}

	failures_ = 0;
	state_.link = ServerState::Link::Ok;

	const QJsonObject o = parse(beta.body).object();
	state_.have_beta = true;
	state_.beta_status = str(o, "status");
	state_.requested_at = str(o, "requestedAt");
	state_.admitted = o.value(QLatin1String("admitted")).toBool();
	state_.run_state = str(o, "runState");
	state_.budget_usd = o.value(QLatin1String("budgetUsd")).toDouble();
	state_.spent_usd = o.value(QLatin1String("spentUsd")).toDouble();
	state_.overlay_connected = o.value(QLatin1String("overlayConnected")).toBool();
	state_.overlay_count = o.value(QLatin1String("overlayCount")).toInt();

	/* A failed read keeps the last count: a blink is not "nothing pending". */
	const HttpResult orders = client_.get("/api/orders");
	if (orders.ok()) {
		state_.pending_orders = 0;
		for (const QJsonValue &order : parse(orders.body).array())
			if (order.toObject().value(QLatin1String("state")).toString() == QLatin1String("pending"))
				++state_.pending_orders;
	}
}

void StatusLoop::run()
{
	os_set_thread_name("sokaster-status");

	std::unique_lock<std::mutex> lock(mutex_);
	while (!stop_) {
		bool configured = true;
		if (reconfigure_) {
			reconfigure_ = false;
			const Config cfg = config::get();
			client_.configure(cfg.base_url(), cfg.access_key);
			configured = cfg.configured();
			state_ = ServerState();
			account_fetched_ = false;
			failures_ = 0;
		} else {
			configured = config::get().configured();
		}
		poke_ = false;

		lock.unlock();
		if (configured)
			poll();
		else
			state_ = ServerState();
		lock.lock();

		if (stop_)
			break;
		if (!reconfigure_) {
			lock.unlock();
			sink_(state_);
			lock.lock();
		}

		if (!configured || state_.link == ServerState::Link::KeyRejected) {
			/* Nothing to ask with: idle until a new key or a stop. */
			cv_.wait(lock, [this] { return stop_ || reconfigure_; });
		} else {
			cv_.wait_for(lock, std::chrono::milliseconds(poll_interval_ms()),
				     [this] { return stop_ || reconfigure_ || poke_; });
		}
	}
}

} // namespace sokaster
