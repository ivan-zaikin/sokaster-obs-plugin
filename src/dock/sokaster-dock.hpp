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

#include "link-flow.hpp"
#include "overlay-finder.hpp"
#include "readiness.hpp"
#include "status-loop.hpp"

#include <obs.h>

#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;
class QVBoxLayout;

/*
 * The sokaster panel inside the OBS window: whether the co-host is ready and,
 * if not, why and what fixes it; what it watches; what it hears. Everything
 * else — orders, moderation, voice, lore — lives in the browser dashboard.
 *
 * Handed to obs_frontend_add_dock_by_id, which takes ownership.
 */
class SokasterDock : public QWidget {
	Q_OBJECT

public:
	explicit SokasterDock(QWidget *parent = nullptr);
	~SokasterDock() override;

	/* OBS remembers the panel's position under this id: never change it. */
	static constexpr const char *kDockId = "sokaster_dock";

public slots:
	/* Re-reads sources and scenes. Coalesced: OBS loading a scene collection
	 * fires one signal per source. Safe to queue from any thread. */
	void refreshSources();

private:
	struct RowWidgets {
		QLabel *title = nullptr;
		QLabel *status = nullptr;
	};

	void buildUi();
	RowWidgets addRow(QVBoxLayout *layout, const char *title_key);
	void paintRow(RowWidgets &row, const sokaster::Readiness::Row &value);

	static void onSourceSignal(void *data, calldata_t *cd);

	void refreshNow();
	void refreshWatches();
	void refreshHears();
	void reevaluate();

	void onSourceToggled(QListWidgetItem *item);
	void onWatchesActivated(int index);
	void onSignIn();
	void onSignOut();
	void onOverlayFix();
	void applyServerState(const sokaster::ServerState &state);
	void applyLinkEvent(const sokaster::LinkEvent &event);

	QLabel *headline_ = nullptr;

	RowWidgets account_;
	QPushButton *signInButton_ = nullptr;
	/* While the sign-in waits for the browser: the code, the address, a way out. */
	QWidget *linkBox_ = nullptr;
	QLabel *linkCode_ = nullptr;
	QLabel *linkUrl_ = nullptr;
	/* Why the last sign-in did not finish; empty once it did. */
	QLabel *linkStatus_ = nullptr;
	QPushButton *signOutButton_ = nullptr;
	bool linking_ = false;

	RowWidgets overlay_;
	QPushButton *overlayButton_ = nullptr;
	/* What the button does and to which source, as of the last repaint. */
	sokaster::Readiness::OverlayFix overlayFix_ = sokaster::Readiness::OverlayFix::None;
	sokaster::OverlayPresence overlayFound_;

	RowWidgets eyes_;
	QComboBox *watches_ = nullptr;
	QString lastEyesName_;

	RowWidgets ears_;
	QListWidget *sources_ = nullptr;

	RowWidgets air_;

	QTimer *refreshTimer_ = nullptr;
	/* Counts the retry and the workers' health down; only while that moves. */
	QTimer *tick_ = nullptr;

	sokaster::ServerState server_;

	/* Set while lists are filled, so their signals are not taken for clicks. */
	bool populating_ = false;
};
