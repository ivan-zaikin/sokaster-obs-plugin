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

namespace sokaster {
namespace {

/* Nothing sane comes back from this API in megabytes. */
constexpr size_t kMaxBodyBytes = 1024 * 1024;

size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *body = static_cast<std::string *>(userdata);
	if (body->size() + total > kMaxBodyBytes)
		return 0;

	body->append(ptr, total);
	return total;
}

int on_progress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return static_cast<const std::atomic<bool> *>(clientp)->load() ? 1 : 0;
}

} // namespace

bool BackendClient::global_init()
{
	/* Reference-counted, so safe next to OBS's own use of libcurl. Never
	 * paired with curl_global_cleanup: that must not run while other threads
	 * of the process may still be using curl. */
	return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
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
	base_url_ = base_url;
	access_key_ = access_key;
	cancelled_ = false;
}

HttpResult BackendClient::perform(const std::string &path, void *mime, const std::string *json, long timeout_seconds,
				  const char *method)
{
	HttpResult result;
	if (!curl_ || base_url_.empty() || cancelled_)
		return result;

	CURL *curl = static_cast<CURL *>(curl_);
	const std::string url = base_url_ + path;
	const std::string user_agent = std::string("sokaster-obs/") + PLUGIN_VERSION;

	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	/* Belt and braces next to the config check; redirects are not followed,
	 * so the key never goes anywhere but base_url. */
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds < 10L ? timeout_seconds : 10L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled_);

	struct curl_slist *headers = nullptr;
	if (!access_key_.empty())
		headers = curl_slist_append(headers, ("X-Access-Key: " + access_key_).c_str());
	/* Otherwise curl waits a second for a 100-continue on every upload. */
	headers = curl_slist_append(headers, "Expect:");

	if (mime) {
		curl_easy_setopt(curl, CURLOPT_MIMEPOST, static_cast<curl_mime *>(mime));
	} else if (json) {
		headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json->c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json->size()));
	}
	if (method)
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	const CURLcode code = curl_easy_perform(curl);
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
	else if (code != CURLE_ABORTED_BY_CALLBACK)
		obs_log(LOG_WARNING, "http: %s failed: %s", path.c_str(), curl_easy_strerror(code));

	curl_slist_free_all(headers);
	return result;
}

HttpResult BackendClient::get(const std::string &path, long timeout_seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform(path, nullptr, nullptr, timeout_seconds);
}

HttpResult BackendClient::post_json(const std::string &path, const std::string &json, long timeout_seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform(path, nullptr, &json, timeout_seconds);
}

HttpResult BackendClient::del(const std::string &path, long timeout_seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return perform(path, nullptr, nullptr, timeout_seconds, "DELETE");
}

HttpResult BackendClient::post_file(const std::string &path, const char *field, const char *filename,
				    const char *mime_type, const std::vector<uint8_t> &data, const Fields &fields,
				    long timeout_seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!curl_)
		return {};

	curl_mime *mime = curl_mime_init(static_cast<CURL *>(curl_));

	curl_mimepart *part = curl_mime_addpart(mime);
	curl_mime_name(part, field);
	curl_mime_filename(part, filename);
	curl_mime_type(part, mime_type);
	curl_mime_data(part, reinterpret_cast<const char *>(data.data()), data.size());

	for (const auto &[name, value] : fields) {
		part = curl_mime_addpart(mime);
		curl_mime_name(part, name.c_str());
		curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
	}

	HttpResult result = perform(path, mime, nullptr, timeout_seconds);
	curl_mime_free(mime);
	return result;
}

} // namespace sokaster
