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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "dock/link-flow.hpp"
#include "dock/sokaster-dock.hpp"
#include "dock/status-loop.hpp"
#include "plugin-config.hpp"
#include "net/backend-client.hpp"
#include "vision/vision-loop.hpp"
#include "audio/audio-loop.hpp"

#include <QMetaObject>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("Plugin.Name");
}

static bool dock_registered = false;
static bool frontend_callback_added = false;

/* The panic button without the dock: OBS Settings -> Hotkeys, or a Stream Deck. */
static constexpr const char *kDropOrderHotkey = "sokaster.drop_order";
static obs_hotkey_id drop_order_hotkey = OBS_INVALID_HOTKEY_ID;
/* What is on disk, so a collection save that changed no key writes nothing. */
static std::string saved_drop_order_bindings;

/* Owned by OBS, never deleted here. Kept only so the panel can be told to
 * redraw when something it shows has changed underneath it. */
static SokasterDock *dock_widget = nullptr;

static void refresh_dock()
{
	if (dock_widget)
		QMetaObject::invokeMethod(dock_widget, "refreshSources", Qt::QueuedConnection);
}

static void on_drop_order_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	/* Hotkey thread: the loop only takes the request and wakes its worker. */
	if (!pressed)
		return;
	obs_log(LOG_INFO, "hotkey: take the order off air");
	sokaster::StatusLoop::instance().drop_order();
}

static std::string bindings_json(obs_data_array_t *bindings)
{
	obs_data_t *wrapper = obs_data_create();
	obs_data_set_array(wrapper, "b", bindings);
	std::string json = obs_data_get_json(wrapper);
	obs_data_release(wrapper);
	return json;
}

static void register_hotkeys()
{
	drop_order_hotkey = obs_hotkey_register_frontend(kDropOrderHotkey, obs_module_text("Hotkey.DropOrder"),
							 on_drop_order_hotkey, nullptr);

	if (obs_data_array_t *bindings = sokaster::config::load_hotkey(kDropOrderHotkey)) {
		obs_hotkey_load(drop_order_hotkey, bindings);
		saved_drop_order_bindings = bindings_json(bindings);
		obs_data_array_release(bindings);
	}
}

static void save_hotkeys()
{
	if (drop_order_hotkey == OBS_INVALID_HOTKEY_ID)
		return;

	obs_data_array_t *bindings = obs_hotkey_save(drop_order_hotkey);
	std::string json = bindings_json(bindings);
	if (json != saved_drop_order_bindings) {
		sokaster::config::save_hotkey(kDropOrderHotkey, bindings);
		saved_drop_order_bindings = std::move(json);
	}
	obs_data_array_release(bindings);
}

/* OBS saves the scene collection after the settings dialog, hotkeys included:
 * the moment to write ours, though they live in a file of their own. */
static void on_frontend_save(obs_data_t *, bool saving, void *)
{
	if (saving)
		save_hotkeys();
}

/* Idempotent: EXIT does it, and unload repeats it for a run without a frontend. */
static void shutdown_workers()
{
	sokaster::LinkFlow::instance().cancel();
	sokaster::StatusLoop::instance().stop();
	sokaster::AudioLoop::instance().stop();
	sokaster::VisionLoop::instance().shutdown();
}

/*
 * The co-host works while the stream works.
 *
 * The reference scenario allows zero actions at the start of a broadcast, so
 * there is no button of ours to press: OBS going live is the signal, and OBS
 * going off air is the one to stop. Recording alone does not count — a local
 * recording is not an audience, and frames cost money.
 */
static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		/* Scenes and sources are not safe to touch before this. */
		sokaster::VisionLoop::instance().restore_selection();
		sokaster::AudioLoop::instance().resolve_track();
		refresh_dock();
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* Another profile, other output settings: our track may be the
		 * one this profile records. */
		sokaster::AudioLoop::instance().resolve_track();
		refresh_dock();
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		/* A new scene can bring audio sources of its own along with it. */
		refresh_dock();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		sokaster::VisionLoop::instance().start();
		sokaster::AudioLoop::instance().start();
		sokaster::StatusLoop::instance().set_streaming(true);
		refresh_dock();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		sokaster::VisionLoop::instance().stop();
		sokaster::AudioLoop::instance().stop();
		sokaster::StatusLoop::instance().set_streaming(false);
		refresh_dock();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		save_hotkeys();
		/* Last moment at which the graphics thread and Qt are both still
		 * alive. A thread that outlives this is a crash on the way out —
		 * the most visible kind of failure there is. */
		shutdown_workers();
		break;

	default:
		break;
	}
}

bool obs_module_load(void)
{
	sokaster::config::load();

	if (!sokaster::BackendClient::global_init()) {
		obs_log(LOG_ERROR, "curl init failed, sokaster cannot reach the backend");
		return false;
	}

	obs_log(LOG_INFO, "sokaster %s loaded", PLUGIN_VERSION);
	return true;
}

/*
 * The dock is registered in post_load rather than load: by then the frontend is
 * up and obs_frontend_get_main_window() has a main window to give us. It also
 * keeps us independent of module load order.
 */
void obs_module_post_load(void)
{
	/* Invariant: no exception ever crosses the C API boundary. */
	try {
		if (!obs_frontend_get_main_window()) {
			/* Headless or UI-less run: carry on without a dock
			 * instead of failing to load. */
			obs_log(LOG_INFO, "no frontend, dock skipped");
			return;
		}

		/* OBS takes ownership: it wraps the widget in a QDockWidget. */
		auto *dock = new SokasterDock();
		dock_registered =
			obs_frontend_add_dock_by_id(SokasterDock::kDockId, obs_module_text("Dock.Title"), dock);

		if (!dock_registered) {
			obs_log(LOG_WARNING, "dock registration failed");
			delete dock;
			return;
		}

		dock_widget = dock;

		obs_log(LOG_INFO, "dock registered");

		obs_frontend_add_event_callback(on_frontend_event, nullptr);
		obs_frontend_add_save_callback(on_frontend_save, nullptr);
		frontend_callback_added = true;

		/* With the dock only: its status loop is what takes the order off. */
		register_hotkeys();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "post_load failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "post_load failed");
	}
}

void obs_module_unload(void)
{
	shutdown_workers();

	if (drop_order_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(drop_order_hotkey);
		drop_order_hotkey = OBS_INVALID_HOTKEY_ID;
	}

	if (frontend_callback_added) {
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		obs_frontend_remove_save_callback(on_frontend_save, nullptr);
		frontend_callback_added = false;
	}

	if (dock_registered) {
		obs_frontend_remove_dock(SokasterDock::kDockId);
		dock_registered = false;
		dock_widget = nullptr;
	}

	/* No curl_global_cleanup: it is not safe while other modules use curl. */
	obs_log(LOG_INFO, "sokaster unloaded");
}
