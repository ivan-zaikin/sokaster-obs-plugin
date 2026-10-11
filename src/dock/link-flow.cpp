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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

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
	const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body));
	return doc.isObject() ? doc.object() : QJsonObject();
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

LinkFlow::~LinkFlow()
{
	cancel();
}

void LinkFlow::set_sink(Sink sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	sink_ = std::move(sink);
}

void LinkFlow::start(const std::string &base_url)
{
	cancel();

	cancelled_ = false;
	client_.clear_cancel();
	active_ = true;
	worker_ = std::thread([this, base_url] { run(base_url); });
}

void LinkFlow::cancel()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = true;
	}
	cv_.notify_all();
	client_.cancel();

	if (worker_.joinable())
		worker_.join();
	active_ = false;
}

void LinkFlow::notify(const LinkEvent &event)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (sink_ && !cancelled_)
		sink_(event);
}

bool LinkFlow::wait_ms(int ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return cancelled_.load(); });
	return !cancelled_;
}

void LinkFlow::run(std::string base_url)
{
	client_.configure(base_url, std::string());

	LinkEvent failed;
	failed.kind = LinkEvent::Kind::Failed;

	const HttpResult started = client_.post_json("/api/link/start", "{}");
	if (!started.ok()) {
		if (!cancelled_)
			obs_log(LOG_WARNING, "link: start refused (%ld)", started.status);
		notify(failed);
		active_ = false;
		return;
	}

	const QJsonObject s = parse_object(started.body);
	const std::string device_code = str(s, "deviceCode");
	LinkEvent code;
	code.kind = LinkEvent::Kind::Code;
	code.user_code = str(s, "userCode");
	code.url = str(s, "verificationUrlComplete");
	if (device_code.empty() || code.user_code.empty()) {
		obs_log(LOG_WARNING, "link: start answered without a code");
		notify(failed);
		active_ = false;
		return;
	}

	const int interval = std::max(1, s.value(QLatin1String("intervalSeconds")).toInt(kDefaultIntervalSeconds));
	const int lifetime =
		std::max(interval, s.value(QLatin1String("expiresInSeconds")).toInt(kDefaultLifetimeSeconds));
	const uint64_t deadline = os_gettime_ns() + static_cast<uint64_t>(lifetime) * 1000000000ull;

	obs_log(LOG_INFO, "link: waiting for the streamer to confirm in the browser");
	notify(code);

	QJsonObject body;
	body.insert(QLatin1String("deviceCode"), QString::fromStdString(device_code));
	const std::string poll_json = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();

	while (wait_ms(interval * 1000)) {
		if (os_gettime_ns() >= deadline)
			break;

		const HttpResult poll = client_.post_json("/api/link/poll", poll_json);
		/* A blink of the network is not an answer; the code is still good
		 * until the deadline. */
		if (!poll.ok())
			continue;

		const QJsonObject p = parse_object(poll.body);
		const std::string status = str(p, "status");
		if (status == "pending")
			continue;

		if (status == "approved") {
			LinkEvent done;
			done.kind = LinkEvent::Kind::Done;
			done.access_key = str(p, "accessKey");
			if (!done.access_key.empty()) {
				obs_log(LOG_INFO, "link: signed in");
				notify(done);
				active_ = false;
				return;
			}
		} else if (status == "denied") {
			obs_log(LOG_INFO, "link: the streamer declined in the browser");
			LinkEvent denied;
			denied.kind = LinkEvent::Kind::Denied;
			notify(denied);
			active_ = false;
			return;
		}

		/* "expired", or an approval without a key: either way, start over. */
		break;
	}

	if (!cancelled_)
		obs_log(LOG_INFO, "link: the code ran out");
	notify(failed);
	active_ = false;
}

} // namespace sokaster
