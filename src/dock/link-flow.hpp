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

#include <atomic>
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
		/* The streamer pressed "That wasn't me". */
		Denied,
		/* The code ran out, or the server refused to start. */
		Failed,
	};

	Kind kind = Kind::Failed;
	std::string user_code;
	std::string url;
	std::string access_key;
};

/*
 * Signing the plugin in through the sokaster site.
 *
 * The plugin asks for a code, the streamer opens /link in the browser where
 * Twitch already knows them, checks the code against the one in the dock and
 * confirms; the plugin then collects its key. No Twitch token passes through
 * OBS and nothing is copied by hand: Twitch only ever talks to the site.
 *
 * The exchange runs on a thread of its own with its own connection, keyless:
 * the status loop knocks with the old key, and this has none yet.
 */
class LinkFlow {
public:
	using Sink = std::function<void(const LinkEvent &)>;

	static LinkFlow &instance();

	/* Called on the worker thread; must only post to the UI. */
	void set_sink(Sink sink);

	/* Starts a new sign-in, dropping one already under way. */
	void start(const std::string &base_url);

	/* Stops waiting. Returns once the worker is gone. */
	void cancel();

	bool active() const { return active_; }

private:
	LinkFlow() = default;
	~LinkFlow();

	void run(std::string base_url);
	void notify(const LinkEvent &event);

	/* Sleeps unless cancelled; false once cancelled. */
	bool wait_ms(int ms);

	BackendClient client_;
	std::thread worker_;
	std::atomic<bool> active_{false};
	std::atomic<bool> cancelled_{false};

	std::mutex mutex_;
	std::condition_variable cv_;
	Sink sink_;
};

} // namespace sokaster
