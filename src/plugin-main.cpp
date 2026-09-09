/*
sokaster — an AI co-host inside OBS Studio
Copyright (C) 2026 sokaster <hello@sokaster.ru>

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

bool obs_module_load(void)
{
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
		dock_registered = obs_frontend_add_dock_by_id(SokasterDock::kDockId,
							     obs_module_text("Dock.Title"), dock);

		if (!dock_registered) {
			obs_log(LOG_WARNING, "dock registration failed");
			delete dock;
			return;
		}

		obs_log(LOG_INFO, "dock registered");
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "post_load failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "post_load failed");
	}
}

void obs_module_unload(void)
{
	if (dock_registered) {
		obs_frontend_remove_dock(SokasterDock::kDockId);
		dock_registered = false;
	}

	obs_log(LOG_INFO, "sokaster unloaded");
}
