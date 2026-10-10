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
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sokaster {

/* One row of GET /api/orders. Field names follow the server's OrderDto. */
struct QueuedOrder {
	std::string id;
	/* "onair", "queued" or "pending". */
	std::string state;
	std::string from;
	/* The viewer's raw text. Shown only while the streamer has to decide on
	 * it; it never goes on air as written. */
	std::string prompt;
	std::string display_name;
	int risk = 0;
	std::vector<std::string> categories;
	int seconds_left = 0;
	int position = 0;
	int wait_seconds = 0;
	int hold_seconds_left = 0;
	bool own = false;
};

struct VoiceOption {
	std::string id;
	/* Server-side hint, Russian only for now. */
	std::string hint;
};

/*
 * Everything the dock knows from the server, as of one poll.
 *
 * Failed requests keep the previous values and say so in `link` and
 * `orders_stale`: a network blip must not read as "your queue is empty".
 */
struct ServerState {
	enum class Link {
		/* No key configured, so nothing was asked. */
		Unconfigured,
		/* Asked, nothing answered yet. */
		Connecting,
		Ok,
		/* Network or 5xx; retry_at_ns says when the next poll is. */
		Offline,
		KeyRejected,
	};

	Link link = Link::Unconfigured;
	/* os_gettime_ns() of the next attempt while Offline, so the dock can
	 * count it down instead of showing a frozen number. */
	uint64_t retry_at_ns = 0;

	/* GET /api/user/me. */
	std::string display_name;

	/* GET /api/beta/me — have_beta is false until the first answer. */
	bool have_beta = false;
	/* "None", "Pending", "Approved", "Rejected". */
	std::string beta_status;
	std::string requested_at;
	bool admitted = false;
	/* IRunGate.Evaluate as a string: "Ok", "NoOrder", "NoOverlay", ... */
	std::string run_state;
	double budget_usd = 0;
	double spent_usd = 0;
	std::string donate_url;
	bool overlay_connected = false;

	/* GET /api/orders. */
	bool have_orders = false;
	bool orders_stale = false;
	std::vector<QueuedOrder> orders;

	std::vector<VoiceOption> voices;

	/* GET /api/overlay-token: tells our overlay from a stale one. */
	std::string overlay_token;

	/* os_gettime_ns() when the orders were read, so the dock can count
	 * their timers down between polls. */
	uint64_t orders_received_ns = 0;
};

/* The outcome of something the streamer clicked. */
struct CommandResult {
	enum class Kind { PlaceOrder, Approve, Reject, Drop };

	Kind kind = Kind::PlaceOrder;
	bool ok = false;
	/* 0 when the server never answered. */
	long status = 0;
	/* The server's own "error" text, when it sent one. */
	std::string error;

	/* PlaceOrder and Approve: where the order landed. */
	std::string state;
	int position = 0;
	int wait_seconds = 0;
	bool dropped = false;
};

/*
 * The dock's link to the server: a worker that polls the status and the queue
 * and runs whatever the streamer clicks, so the Qt thread never waits on the
 * network.
 *
 * Results go out through two sinks set by the dock. They are called on the
 * worker thread; the dock is responsible for hopping to the UI thread.
 */
class StatusLoop {
public:
	using StateSink = std::function<void(const ServerState &)>;
	using CommandSink = std::function<void(const CommandResult &)>;

	static StatusLoop &instance();

	/* Sinks must be set before start() and stay valid until stop(). */
	void set_sinks(StateSink state_sink, CommandSink command_sink);

	/* Starts the worker. With no key it reports Unconfigured once and does
	 * no periodic work, as an unconfigured plugin must. */
	void start();

	/* Joins the worker. Safe to call when not running. */
	void stop();

	/* The key or server changed: forget who we were and ask again. */
	void reconfigure();

	/* Poll now rather than at the next tick: after a click, or when OBS
	 * starts or stops streaming. */
	void poke();

	/* Polls every few seconds on air, rarely off air: between streams the
	 * queue only changes when someone orders, and nobody is watching it. */
	void set_streaming(bool streaming);

	void place_order(const std::string &prompt, const std::string &voice);
	void approve(const std::string &order_id);
	void reject(const std::string &order_id);
	void drop_on_air();

private:
	StatusLoop() = default;
	~StatusLoop() = default;

	StatusLoop(const StatusLoop &) = delete;
	StatusLoop &operator=(const StatusLoop &) = delete;

	void run();
	void poll();
	void enqueue(std::function<void()> command);
	void report_command(const CommandResult &result);

	/* Fetches once per account: who we are, the voices, the overlay token. */
	void fetch_account();

	BackendClient client_;

	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> reconfigure_requested_{false};
	std::atomic<bool> streaming_{false};

	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> commands_;
	bool poke_requested_ = false;

	/* Worker-owned from here down. */
	ServerState state_;
	bool account_fetched_ = false;
	int consecutive_failures_ = 0;

	StateSink state_sink_;
	CommandSink command_sink_;
};

} // namespace sokaster
