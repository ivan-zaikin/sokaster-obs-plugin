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

#include "backend-client.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <curl/curl.h>

#include <cctype>
#include <stdexcept>

namespace sokaster {
namespace {

constexpr long kFrameTimeoutSeconds = 30;
constexpr long kAudioTimeoutSeconds = 30;
constexpr long kSettingsTimeoutSeconds = 10;

/* Sent while OBS is closing, so it gets a short leash: a backend that is not
 * answering must not hold up the streamer's shutdown. */
constexpr long kSessionStopTimeoutSeconds = 3;

size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *body = static_cast<std::string *>(userdata);

	/* Nothing sane comes back from this API in megabytes; refusing to grow
	 * without bound is cheaper than trusting the other end. */
	if (body->size() + total > 1024 * 1024)
		return 0;

	body->append(ptr, total);
	return total;
}

/* Only Retry-After is read back; the rest of the headers are of no interest,
 * and parsing more of them would only be more to get wrong. */
size_t read_header(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *result = static_cast<HttpResult *>(userdata);

	static const char kRetryAfter[] = "retry-after:";
	const size_t name_len = sizeof(kRetryAfter) - 1;

	if (total > name_len) {
		bool match = true;
		for (size_t i = 0; i < name_len; ++i) {
			if (std::tolower(static_cast<unsigned char>(ptr[i])) != kRetryAfter[i]) {
				match = false;
				break;
			}
		}

		if (match) {
			const std::string value(ptr + name_len, total - name_len);
			try {
				const int seconds = std::stoi(value);
				if (seconds > 0)
					result->retry_after_seconds = seconds;
			} catch (const std::exception &) {
				/* A date-form Retry-After, or nonsense. Either way the
				 * caller's own backoff is the fallback. */
			}
		}
	}

	return total;
}

std::string strip_trailing_slash(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	return url;
}

} // namespace

bool BackendClient::global_init()
{
	/* OBS itself already calls this, but a plugin may not assume load order,
	 * and curl_global_init refcounts. */
	return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void BackendClient::global_cleanup()
{
	curl_global_cleanup();
}

BackendClient::BackendClient() : curl_(curl_easy_init()) {}

BackendClient::~BackendClient()
{
	if (curl_)
		curl_easy_cleanup(static_cast<CURL *>(curl_));
}

void BackendClient::configure(const std::string &base_url, const std::string &access_key)
{
	std::lock_guard<std::mutex> lock(mutex_);
	base_url_ = strip_trailing_slash(base_url);
	access_key_ = access_key;
	cancelled_ = false;
}

void BackendClient::clear_cancel()
{
	cancelled_ = false;
}

void BackendClient::cancel()
{
	/* Read by the progress callback from inside curl_easy_perform, so it must
	 * not take the mutex the running request already holds. */
	cancelled_ = true;
}

std::string BackendClient::url_for(const std::string &path) const
{
	return base_url_ + path;
}

HttpResult BackendClient::perform_locked(const std::string &path, bool post, void *mime, long timeout_seconds)
{
	HttpResult result;

	if (!curl_ || base_url_.empty())
		return result;

	CURL *curl = static_cast<CURL *>(curl_);
	const std::string url = url_for(path);

	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
	/* Parenthesised comparison rather than std::min: windows.h arrives through
	 * curl and brings a min macro with it. */
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds < 10L ? timeout_seconds : 10L);
	const std::string user_agent = std::string("sokaster-obs/") + PLUGIN_VERSION;
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);

	/* Lets stop() interrupt an upload instead of waiting out the timeout. */
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
	curl_easy_setopt(
		curl, CURLOPT_XFERINFOFUNCTION,
		+[](void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
			return static_cast<BackendClient *>(clientp)->cancelled_ ? 1 : 0;
		});

	struct curl_slist *headers = nullptr;
	if (!access_key_.empty()) {
		const std::string header = "X-Access-Key: " + access_key_;
		headers = curl_slist_append(headers, header.c_str());
	}
	/* curl would otherwise wait 1s for a 100-continue on every upload. */
	headers = curl_slist_append(headers, "Expect:");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	if (mime)
		curl_easy_setopt(curl, CURLOPT_MIMEPOST, static_cast<curl_mime *>(mime));
	else if (post) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
	}

	const CURLcode code = curl_easy_perform(curl);
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
	else if (code != CURLE_ABORTED_BY_CALLBACK)
		obs_log(LOG_WARNING, "http: %s failed: %s", path.c_str(), curl_easy_strerror(code));

	curl_slist_free_all(headers);
	return result;
}

HttpResult BackendClient::post_frame(const std::vector<uint8_t> &jpeg)
{
	std::lock_guard<std::mutex> lock(mutex_);

	if (!curl_)
		return {};

	curl_mime *mime = curl_mime_init(static_cast<CURL *>(curl_));
	curl_mimepart *part = curl_mime_addpart(mime);
	curl_mime_name(part, "frame");
	curl_mime_filename(part, "frame.jpg");
	curl_mime_type(part, "image/jpeg");
	curl_mime_data(part, reinterpret_cast<const char *>(jpeg.data()), jpeg.size());

	HttpResult result = perform_locked("/api/frame", true, mime, kFrameTimeoutSeconds);

	curl_mime_free(mime);
	return result;
}

HttpResult BackendClient::post_audio(const std::vector<uint8_t> &wav, int duration_ms, const std::string &captured_at)
{
	std::lock_guard<std::mutex> lock(mutex_);

	if (!curl_)
		return {};

	const std::string duration = std::to_string(duration_ms);

	curl_mime *mime = curl_mime_init(static_cast<CURL *>(curl_));

	curl_mimepart *part = curl_mime_addpart(mime);
	curl_mime_name(part, "audio");
	curl_mime_filename(part, "segment.wav");
	curl_mime_type(part, "audio/wav");
	curl_mime_data(part, reinterpret_cast<const char *>(wav.data()), wav.size());

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "durationMs");
	curl_mime_data(part, duration.c_str(), CURL_ZERO_TERMINATED);

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "capturedAt");
	curl_mime_data(part, captured_at.c_str(), CURL_ZERO_TERMINATED);

	/* One track carrying everything the streamer ticked, so the backend is
	 * told "mixed" rather than a device name it could not use anyway. */
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "source");
	curl_mime_data(part, "mixed", CURL_ZERO_TERMINATED);

	HttpResult result = perform_locked("/api/audio", true, mime, kAudioTimeoutSeconds);

	curl_mime_free(mime);
	return result;
}

HttpResult BackendClient::get_settings()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform_locked("/api/user/me/settings", false, nullptr, kSettingsTimeoutSeconds);
}

HttpResult BackendClient::post_session_start()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform_locked("/api/session/start", true, nullptr, kSettingsTimeoutSeconds);
}

HttpResult BackendClient::post_session_stop()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform_locked("/api/session/stop", true, nullptr, kSessionStopTimeoutSeconds);
}

} // namespace sokaster
