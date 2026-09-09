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

#include "sokaster-dock.hpp"

#include "audio/audio-loop.hpp"
#include "audio/audio-tap.hpp"

#include <obs-module.h>

#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QVBoxLayout>

namespace {

/* Carries the source UUID on the list item; names change, UUIDs do not. */
constexpr int kUuidRole = Qt::UserRole + 1;

const char *const kSourceSignals[] = {"source_create", "source_destroy", "source_rename"};

} // namespace

SokasterDock::SokasterDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(kDockId);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);

	/* No styling of our own: the dock should look like part of OBS rather than
	 * a guest, so palette and fonts are inherited from the parent. */
	status_ = new QLabel(obs_module_text("Dock.Placeholder"), this);
	status_->setWordWrap(true);
	status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	auto *hearsTitle = new QLabel(obs_module_text("Dock.Hears"), this);
	hearsTitle->setWordWrap(true);

	sources_ = new QListWidget(this);
	sources_->setSelectionMode(QAbstractItemView::NoSelection);
	sources_->setToolTip(obs_module_text("Dock.Hears.Hint"));

	trackLabel_ = new QLabel(this);
	trackLabel_->setWordWrap(true);

	layout->addWidget(status_);
	layout->addSpacing(8);
	layout->addWidget(hearsTitle);
	layout->addWidget(sources_, 1);
	layout->addWidget(trackLabel_);

	connect(sources_, &QListWidget::itemChanged, this, &SokasterDock::onSourceToggled);

	connectSourceSignals();
	refreshSources();
}

SokasterDock::~SokasterDock()
{
	disconnectSourceSignals();
}

void SokasterDock::connectSourceSignals()
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler)
		return;

	for (const char *name : kSourceSignals)
		signal_handler_connect(handler, name, onSourceChangedSignal, this);

	signalsConnected_ = true;
}

void SokasterDock::disconnectSourceSignals()
{
	if (!signalsConnected_)
		return;

	signal_handler_t *handler = obs_get_signal_handler();
	if (handler) {
		for (const char *name : kSourceSignals)
			signal_handler_disconnect(handler, name, onSourceChangedSignal, this);
	}

	signalsConnected_ = false;
}

void SokasterDock::onSourceChangedSignal(void *data, calldata_t *)
{
	/* Arrives on whichever thread touched the source. Anything Qt has to be
	 * done on the UI thread, so all this does is ask for a refresh there. */
	QMetaObject::invokeMethod(static_cast<SokasterDock *>(data), "refreshSources", Qt::QueuedConnection);
}

void SokasterDock::refreshSources()
{
	const int track = sokaster::AudioLoop::instance().track();

	populating_ = true;
	sources_->clear();

	if (track < 0) {
		/* Six tracks and no free one. Better said out loud than worked
		 * around behind the streamer's back. */
		trackLabel_->setText(obs_module_text("Dock.Hears.NoTrack"));
		populating_ = false;
		return;
	}

	const auto items = sokaster::tracks::list_audio_sources(static_cast<size_t>(track));
	for (const auto &info : items) {
		auto *item = new QListWidgetItem(QString::fromStdString(info.name), sources_);
		item->setData(kUuidRole, QString::fromStdString(info.uuid));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(info.heard ? Qt::Checked : Qt::Unchecked);
	}

	populating_ = false;

	trackLabel_->setText(QString(obs_module_text("Dock.Hears.Track")).arg(track + 1));
}

void SokasterDock::onSourceToggled(QListWidgetItem *item)
{
	if (populating_ || !item)
		return;

	const int track = sokaster::AudioLoop::instance().track();
	if (track < 0)
		return;

	const QString uuid = item->data(kUuidRole).toString();
	if (uuid.isEmpty())
		return;

	sokaster::tracks::set_source_heard(uuid.toUtf8().constData(), static_cast<size_t>(track),
					   item->checkState() == Qt::Checked);
}
