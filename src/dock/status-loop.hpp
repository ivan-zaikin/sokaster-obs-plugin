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
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sokaster {

/*
 * Everything the dock knows from the server, as of one poll. A failed poll
 * keeps the previous values and says so in `link`.
 */
struct ServerState {
	enum class Link {
		/* Not asked yet, or no key to ask with. */
		Unknown,
		Ok,
		/* Network or 5xx; retry_at_ns says when the next poll is. */
		Offline,
		KeyRejected,
	};

	Link link = Link::Unknown;
	/* os_gettime_ns() of the next attempt while Offline. */
	uint64_t retry_at_ns = 0;

	/* GET /api/user/me and /api/overlay-token, once per key. */
	std::string display_name;
	std::string overlay_token;

	/* GET /api/beta/me; have_beta is false until the first answer. */
	bool have_beta = false;
	/* "None", "Pending", "Approved", "Rejected". */
	std::string beta_status;
	std::string requested_at;
	bool admitted = false;
	/* The server's RunState: "Ok", "NoOrder", "NoOverlay", ... */
	std::string run_state;
	double budget_usd = 0;
	double spent_usd = 0;
	bool overlay_connected = false;
	/* More than one: every open overlay plays the voice. */
	int overlay_count = 0;

	/* GET /api/orders: viewer orders waiting for the streamer's decision,
	 * which happens in the dashboard. */
	int pending_orders = 0;
	/* An order is on air: what the panic button would take off. */
	bool order_on_air = false;

	/* The answer to the last take-off-air request. drop_seq counts the
	 * answers, so the dock tells a new one from the one it has shown. */
	enum class Drop {
		None,
		Dropped,
		/* Nothing was on air by the time the request arrived. */
		NothingOnAir,
		Failed,
	};
	Drop last_drop = Drop::None;
	uint32_t drop_seq = 0;
};

/*
 * Polls the server for the dock on a worker of its own, so the UI thread never
 * waits on the network: every few seconds on air, rarely off air, never
 * without a key.
 */
class StatusLoop {
public:
	/* Called on the worker; must only post to the UI thread. */
	using Sink = std::function<void(const ServerState &)>;

	static StatusLoop &instance();

	void start(Sink sink);

	/* Joins the worker. Safe to call when not running. */
	void stop();

	/* The key changed: forget the account and ask again. */
	void reconfigure();

	/* Poll now rather than at the next tick. */
	void poke();

	void set_streaming(bool streaming);

	/* Takes the order on air off it, on the worker. Any thread: the hotkey
	 * calls it from the hotkey thread. A repeat within a few seconds is
	 * dropped — by then the next order may be on air, and a held key or a
	 * double click must not take that one off too. */
	void drop_order();

private:
	StatusLoop() = default;

	void run();
	void poll();
	void fetch_account();
	void drop_now();
	void reset_state();
	int poll_interval_ms() const;

	BackendClient client_;
	std::thread worker_;
	Sink sink_;
	std::atomic<bool> streaming_{false};

	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_ = false;
	bool reconfigure_ = false;
	bool poke_ = false;
	bool drop_ = false;
	uint64_t last_drop_request_ns_ = 0;

	/* Worker-owned. */
	ServerState state_;
	bool account_fetched_ = false;
	int failures_ = 0;
};

} // namespace sokaster
