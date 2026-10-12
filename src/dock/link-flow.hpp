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

#include "net/backend-client.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sokaster {

/* What the sign-in has come to, as told to the dock. */
struct LinkEvent {
	enum class Kind {
		/* The server handed out a code: show it, open the browser. */
		Code,
		/* The streamer confirmed on the site; access_key is theirs. */
		Done,
		/* Declined in the browser, expired, or the server refused. */
		Failed,
	};

	Kind kind = Kind::Failed;
	std::string user_code;
	std::string url;
	std::string access_key;
};

/*
 * Signing the plugin in through the sokaster site, device-code style.
 *
 * The plugin asks for a code, the streamer opens the site where Twitch already
 * knows them, checks the code against the one in the dock and confirms; the
 * plugin then collects its key. No Twitch token passes through OBS and nothing
 * is copied by hand.
 */
class LinkFlow {
public:
	/* Called on the worker; must only post to the UI thread. */
	using Sink = std::function<void(const LinkEvent &)>;

	static LinkFlow &instance();

	void set_sink(Sink sink);

	/* Starts a new sign-in, dropping one already under way. */
	void start(const std::string &base_url);

	/* Stops waiting. Returns once the worker is gone. */
	void cancel();

private:
	LinkFlow() = default;

	void run(const std::string &base_url);
	void notify(LinkEvent::Kind kind, const std::string &user_code = {}, const std::string &url = {},
		    const std::string &access_key = {});

	/* Sleeps unless cancelled; false once cancelled. */
	bool wait_ms(int ms);

	BackendClient client_;
	std::thread worker_;

	std::mutex mutex_;
	std::condition_variable cv_;
	bool cancelled_ = false;
	Sink sink_;
};

} // namespace sokaster
