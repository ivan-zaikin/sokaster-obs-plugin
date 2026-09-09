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

#include <obs.h>

#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;

/*
 * The sokaster control panel — a dock inside the OBS window.
 *
 * Today it carries what the co-host needs before it can hear anything: the
 * list of audio sources the streamer allows it to listen to, and the track
 * OBS mixes them onto. The rest of the panel follows.
 *
 * The widget is handed to obs_frontend_add_dock_by_id, which wraps it in a
 * QDockWidget and takes ownership — we must not delete it ourselves.
 */
class SokasterDock : public QWidget {
	Q_OBJECT

public:
	explicit SokasterDock(QWidget *parent = nullptr);
	~SokasterDock() override;

	/* Dock id: OBS remembers the panel's position under this key, so it must
	 * never change — otherwise the streamer loses their layout. */
	static constexpr const char *kDockId = "sokaster_dock";

public slots:
	/* Rebuilds the source list. UI thread only; the OBS signals that trigger
	 * it arrive on other threads and are queued here. */
	void refreshSources();

private slots:
	void onSourceToggled(QListWidgetItem *item);

private:
	void connectSourceSignals();
	void disconnectSourceSignals();

	/* Global OBS signals. Queue a refresh and return: these fire on whatever
	 * thread created or destroyed the source. */
	static void onSourceChangedSignal(void *data, calldata_t *cd);

	QLabel *status_ = nullptr;
	QLabel *trackLabel_ = nullptr;
	QListWidget *sources_ = nullptr;

	/* Set while refreshSources() populates, so the check-state changes it
	 * makes are not mistaken for the streamer clicking. */
	bool populating_ = false;
	bool signalsConnected_ = false;
};
