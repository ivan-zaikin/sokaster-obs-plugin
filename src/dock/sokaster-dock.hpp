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

#include "readiness.hpp"
#include "status-loop.hpp"

#include <obs.h>

#include <QWidget>

#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

/*
 * The sokaster control panel — a dock inside the OBS window.
 *
 * Only what a streamer needs in the middle of a stream lives here: whether the
 * co-host is ready and if not, why; what it watches and hears; the order field;
 * the queue. Everything else is one click away in the browser dashboard —
 * duplicating the web in Qt would mean two of everything, drifting apart.
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
	/* Rebuilds both source lists and re-reads everything OBS can tell us.
	 * UI thread only; the OBS signals that trigger it arrive on other
	 * threads and are queued here. */
	void refreshSources();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void onSourceToggled(QListWidgetItem *item);
	void onWatchesActivated(int index);
	void onOrderTextChanged();
	void onSendOrder();
	void onSaveKey();
	void onCopyDonateLink();
	void onOpenDashboard();
	void onOverlayFix();
	void onTick();

private:
	/* One line of the readiness panel: a title and what is going on. */
	struct RowWidgets {
		QLabel *title = nullptr;
		QLabel *status = nullptr;
	};

	/* One order in the queue panel, kept so the timers can tick without a
	 * rebuild between polls. */
	struct QueueRow {
		std::string id;
		std::string state;
		QWidget *frame = nullptr;
		QLabel *line = nullptr;
	};

	void buildUi();
	RowWidgets addRow(QVBoxLayout *layout, const char *title_key);

	void connectSourceSignals();
	void disconnectSourceSignals();

	/* Global OBS signals. Queue a refresh and return: these fire on whatever
	 * thread created or destroyed the source. */
	static void onSourceChangedSignal(void *data, calldata_t *cd);

	void refreshWatches();
	void refreshHears();

	void applyServerState(const sokaster::ServerState &state);
	void applyCommandResult(const sokaster::CommandResult &result);

	sokaster::LocalFacts gatherLocal();

	/* Recomputes the readiness panel and paints it. Cheap: no network, a
	 * walk over the scene collection at most. */
	void reevaluate();
	void paintRow(RowWidgets &row, const sokaster::Readiness::Row &value);
	void paintOverlayFix(sokaster::Readiness::OverlayFix fix, const sokaster::OverlayPresence &found);

	void rebuildQueue();
	void updateQueueLines();
	QString queueLine(const sokaster::QueuedOrder &order, int elapsed) const;
	int secondsSinceOrders() const;

	void rebuildVoices();
	void updateTimer();

	QString dashboardUrl() const;

	QLabel *headline_ = nullptr;

	RowWidgets account_;
	QWidget *keyBox_ = nullptr;
	QLineEdit *keyEdit_ = nullptr;

	RowWidgets overlay_;
	QPushButton *overlayButton_ = nullptr;
	/* What the button does and to which source, as of the last repaint. */
	sokaster::Readiness::OverlayFix overlayFix_ = sokaster::Readiness::OverlayFix::None;
	sokaster::OverlayPresence lastOverlay_;
	/* os_gettime_ns() of the last fix, for "connecting" while the page loads. */
	uint64_t overlayPlacedAt_ = 0;

	RowWidgets eyes_;
	QComboBox *watches_ = nullptr;

	RowWidgets ears_;
	QListWidget *sources_ = nullptr;

	RowWidgets air_;
	QPushButton *donateButton_ = nullptr;

	QComboBox *voice_ = nullptr;
	QPlainTextEdit *orderEdit_ = nullptr;
	QLabel *orderCounter_ = nullptr;
	QPushButton *sendButton_ = nullptr;
	QLabel *orderStatus_ = nullptr;

	QLabel *queueStatus_ = nullptr;
	QLabel *queueEmpty_ = nullptr;
	QVBoxLayout *queueLayout_ = nullptr;
	std::vector<QueueRow> queueRows_;

	QTimer *tick_ = nullptr;

	sokaster::ServerState server_;
	std::vector<std::string> voiceIds_;
	QString lastEyesName_;

	/* Set while an order is on its way, so Enter cannot send it twice. */
	bool sending_ = false;
	/* The order field has something of its own to say; the generic "finish
	 * the setup" hint must not overwrite it. */
	bool orderStatusOwned_ = false;

	/* Set while lists are populated, so the check-state and selection
	 * changes they make are not mistaken for the streamer clicking. */
	bool populating_ = false;
	bool signalsConnected_ = false;
};
