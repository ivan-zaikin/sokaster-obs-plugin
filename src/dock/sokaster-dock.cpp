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

#include <obs-module.h>

#include <QLabel>
#include <QVBoxLayout>

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

	layout->addWidget(status_);
	layout->addStretch(1);
}
