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

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sokaster {

struct HttpResult {
	/* 0 means the request never got an answer: DNS, connect, timeout. */
	long status = 0;
	std::string body;

	bool ok() const { return status >= 200 && status < 300; }
	bool rate_limited() const { return status == 429; }
	bool network_error() const { return status == 0; }
};

/*
 * The sokaster backend, over libcurl.
 *
 * One easy handle behind a mutex: connection reuse and TLS session reuse are
 * what keep a frame upload cheap, and only the vision worker talks through it.
 * OBS already ships libcurl, so nothing extra travels with the plugin.
 */
class BackendClient {
public:
	BackendClient();
	~BackendClient();

	BackendClient(const BackendClient &) = delete;
	BackendClient &operator=(const BackendClient &) = delete;

	/* Safe to call between requests; the trailing slash is normalised here so
	 * callers can pass whatever the streamer typed. */
	void configure(const std::string &base_url, const std::string &access_key);

	HttpResult post_frame(const std::vector<uint8_t> &jpeg);
	HttpResult get_settings();
	HttpResult post_session_start();
	HttpResult post_session_stop();

	/* Aborts an in-flight request so stop() does not wait out a 30s timeout. */
	void cancel();

	/* Lifts a previous cancel. The abort is meant for the frame on the wire,
	 * not for the goodbye that follows it. */
	void clear_cancel();

	static bool global_init();
	static void global_cleanup();

private:
	HttpResult perform_locked(const std::string &path, bool post, void *mime, long timeout_seconds);
	std::string url_for(const std::string &path) const;

	mutable std::mutex mutex_;
	/* A CURL easy handle. Kept opaque so curl.h stays out of every
	 * translation unit that talks to the backend. */
	void *curl_ = nullptr;
	std::string base_url_;
	std::string access_key_;
	/* Read from curl's progress callback while a request holds the mutex. */
	std::atomic<bool> cancelled_{false};
};

} // namespace sokaster
