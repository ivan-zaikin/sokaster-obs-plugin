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

#include "dock/sokaster-dock.hpp"
#include "plugin-config.hpp"
#include "net/backend-client.hpp"
#include "vision/vision-loop.hpp"

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
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		sokaster::VisionLoop::instance().start();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		sokaster::VisionLoop::instance().stop();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		/* Last moment at which the graphics thread and Qt are both still
		 * alive. A thread that outlives this is a crash on the way out —
		 * the most visible kind of failure there is. */
		sokaster::VisionLoop::instance().stop();
		sokaster::VisionLoop::instance().grabber().select(nullptr);
		sokaster::VisionLoop::instance().grabber().shutdown();
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

		/* OBS takes ownership: it wraps the widget in a QDockWidget.
		 * We keep no reference of our own and never delete it. */
		auto *dock = new SokasterDock();
		dock_registered =
			obs_frontend_add_dock_by_id(SokasterDock::kDockId, obs_module_text("Dock.Title"), dock);

		if (!dock_registered) {
			obs_log(LOG_WARNING, "dock registration failed");
			delete dock;
			return;
		}

		obs_log(LOG_INFO, "dock registered");

		obs_frontend_add_event_callback(on_frontend_event, nullptr);
		frontend_callback_added = true;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "post_load failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "post_load failed");
	}
}

void obs_module_unload(void)
{
	/* Normally already done by OBS_FRONTEND_EVENT_EXIT; repeated here because
	 * a module can also be unloaded without the frontend ever running. */
	sokaster::VisionLoop::instance().stop();

	if (frontend_callback_added) {
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		frontend_callback_added = false;
	}

	if (dock_registered) {
		obs_frontend_remove_dock(SokasterDock::kDockId);
		dock_registered = false;
	}

	sokaster::BackendClient::global_cleanup();
	obs_log(LOG_INFO, "sokaster unloaded");
}
