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

#include <QWidget>

class QLabel;

/*
 * The sokaster control panel — a dock inside the OBS window.
 *
 * A placeholder for now: it verifies that the dock registers, appears in the
 * interface and survives a restart. Contents follow.
 *
 * The widget is handed to obs_frontend_add_dock_by_id, which wraps it in a
 * QDockWidget and takes ownership — we must not delete it ourselves.
 */
class SokasterDock : public QWidget {
	Q_OBJECT

public:
	explicit SokasterDock(QWidget *parent = nullptr);

	/* Dock id: OBS remembers the panel's position under this key, so it must
	 * never change — otherwise the streamer loses their layout. */
	static constexpr const char *kDockId = "sokaster_dock";

private:
	QLabel *status_ = nullptr;
};
