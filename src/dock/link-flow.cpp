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

#include "link-flow.hpp"

#include "plugin-config.hpp"

#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>

namespace sokaster {
namespace {

/* Used when the server does not say: it waits for a human to press a button. */
constexpr int kDefaultIntervalSeconds = 3;
constexpr int kDefaultLifetimeSeconds = 600;

QJsonObject parse_object(const std::string &body)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
}

std::string str(const QJsonObject &o, const char *key)
{
	return o.value(QLatin1String(key)).toString().toStdString();
}

} // namespace

LinkFlow &LinkFlow::instance()
{
	static LinkFlow flow;
	return flow;
}

void LinkFlow::set_sink(Sink sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	sink_ = std::move(sink);
}

void LinkFlow::start(const std::string &base_url)
{
	cancel();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = false;
	}
	/* Keyless: the account is what this is about to find out. */
	client_.configure(base_url, std::string());
	worker_ = std::thread([this, base_url] { run(base_url); });
}

void LinkFlow::cancel()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = true;
	}
	client_.cancel();
	cv_.notify_all();

	if (worker_.joinable())
		worker_.join();
}

void LinkFlow::notify(LinkEvent::Kind kind, const std::string &user_code, const std::string &url,
		      const std::string &access_key)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (sink_ && !cancelled_)
		sink_(LinkEvent{kind, user_code, url, access_key});
}

bool LinkFlow::wait_ms(int ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	return !cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return cancelled_; });
}

void LinkFlow::run(const std::string &base_url)
{
	os_set_thread_name("sokaster-link");
	obs_log(LOG_INFO, "link: signing in through %s", base_url.c_str());

	const HttpResult started = client_.post_json("/api/link/start");
	const QJsonObject s = parse_object(started.body);
	const std::string device_code = str(s, "deviceCode");
	const std::string user_code = str(s, "userCode");
	if (!started.ok() || device_code.empty() || user_code.empty()) {
		obs_log(LOG_WARNING, "link: start refused (%ld)", started.status);
		notify(LinkEvent::Kind::Failed);
		return;
	}

	const int interval = std::max(1, s.value(QLatin1String("intervalSeconds")).toInt(kDefaultIntervalSeconds));
	const int lifetime =
		std::max(interval, s.value(QLatin1String("expiresInSeconds")).toInt(kDefaultLifetimeSeconds));
	const uint64_t deadline = os_gettime_ns() + static_cast<uint64_t>(lifetime) * 1000000000ull;

	notify(LinkEvent::Kind::Code, user_code, str(s, "verificationUrlComplete"));

	QJsonObject body;
	body.insert(QLatin1String("deviceCode"), QString::fromStdString(device_code));
	const std::string poll_json = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();

	while (wait_ms(interval * 1000) && os_gettime_ns() < deadline) {
		const HttpResult poll = client_.post_json("/api/link/poll", poll_json);
		/* A blink of the network is not an answer; the code is good until
		 * the deadline. */
		if (!poll.ok())
			continue;

		const QJsonObject p = parse_object(poll.body);
		const std::string status = str(p, "status");
		if (status == "pending")
			continue;

		const std::string key = str(p, "accessKey");
		if (status == "approved" && is_token(key)) {
			obs_log(LOG_INFO, "link: signed in");
			notify(LinkEvent::Kind::Done, {}, {}, key);
			return;
		}

		/* "denied", "expired", or an approval without a usable key. */
		obs_log(LOG_INFO, "link: sign-in ended: %s", status.c_str());
		break;
	}

	notify(LinkEvent::Kind::Failed);
}

} // namespace sokaster
