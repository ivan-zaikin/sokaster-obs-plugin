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
#include <utility>
#include <vector>

namespace sokaster {

struct HttpResult {
	/* 0 means the request never got an answer: DNS, connect, timeout, cancel. */
	long status = 0;
	std::string body;

	bool ok() const { return status >= 200 && status < 300; }
};

/*
 * The sokaster backend over libcurl, which OBS already ships.
 *
 * One easy handle per client, behind a mutex: connection and TLS session reuse
 * keep a frame upload cheap. Every worker owns its own client, so a slow upload
 * never holds up another thread's request.
 */
class BackendClient {
public:
	static constexpr long kDefaultTimeoutSeconds = 20;

	/* Plain form fields sent next to a file. */
	using Fields = std::vector<std::pair<std::string, std::string>>;

	BackendClient();
	~BackendClient();

	BackendClient(const BackendClient &) = delete;
	BackendClient &operator=(const BackendClient &) = delete;

	/* An empty key sends no X-Access-Key header. Lifts a previous cancel(). */
	void configure(const std::string &base_url, const std::string &access_key);

	HttpResult get(const std::string &path, long timeout_seconds = kDefaultTimeoutSeconds);
	HttpResult post_json(const std::string &path, const std::string &json = "{}",
			     long timeout_seconds = kDefaultTimeoutSeconds);

	/* multipart/form-data: one file part plus plain fields. */
	HttpResult post_file(const std::string &path, const char *field, const char *filename, const char *mime_type,
			     const std::vector<uint8_t> &data, const Fields &fields, long timeout_seconds);

	/* Aborts the request in flight and every one after it until
	 * clear_cancel() or configure(), so a stop never waits out a timeout. */
	void cancel() { cancelled_ = true; }
	void clear_cancel() { cancelled_ = false; }
	bool cancelled() const { return cancelled_; }

	static bool global_init();

private:
	/* Body is a curl_mime* or the JSON text; at most one of them. */
	HttpResult perform(const std::string &path, void *mime, const std::string *json, long timeout_seconds);

	std::mutex mutex_;
	/* A CURL easy handle, opaque so curl.h stays in one translation unit. */
	void *curl_ = nullptr;
	std::string base_url_;
	std::string access_key_;
	/* Read by curl's progress callback while a request holds the mutex. */
	std::atomic<bool> cancelled_{false};
};

} // namespace sokaster
