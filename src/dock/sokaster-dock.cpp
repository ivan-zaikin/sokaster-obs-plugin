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

#include "plugin-config.hpp"
#include "audio/audio-loop.hpp"
#include "vision/source-picker.hpp"
#include "vision/vision-loop.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

using sokaster::Readiness;

/* Carries the source UUID on list items; names change, UUIDs do not. */
constexpr int kUuidRole = Qt::UserRole + 1;

constexpr const char *kDashboardPath = "/profile";

const char *const kSourceSignals[] = {"source_create", "source_destroy", "source_rename"};

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* The words carry the meaning; the dot's colour only has to be told apart.
 * Mid-tones that hold up on both the dark and the light OBS themes. */
const char *level_colour(Readiness::Level level)
{
	switch (level) {
	case Readiness::Level::Ok:
		return "#3fb950";
	case Readiness::Level::Warn:
		return "#d29922";
	case Readiness::Level::Block:
		return "#f85149";
	default:
		return nullptr;
	}
}

const char *overlay_fix_label(Readiness::OverlayFix fix)
{
	switch (fix) {
	case Readiness::OverlayFix::Update:
		return "Dock.Overlay.Update";
	case Readiness::OverlayFix::Listen:
		return "Dock.Overlay.Listen";
	case Readiness::OverlayFix::Sound:
		return "Dock.Overlay.Sound";
	default:
		return "Dock.Overlay.Add";
	}
}

/* The workers read the key and open the session when they start: a restart
 * picks up a new key, and opens a session the server has lost. */
void restart_workers()
{
	sokaster::VisionLoop::instance().stop();
	sokaster::AudioLoop::instance().stop();
	sokaster::VisionLoop::instance().start();
	sokaster::AudioLoop::instance().start();
}

/* The page needs a few seconds to load and connect: ask the server then
 * rather than at the next routine poll. */
void poke_status_soon(QObject *context)
{
	for (int ms : {3000, 8000})
		QTimer::singleShot(ms, context, [] { sokaster::StatusLoop::instance().poke(); });
}

QLabel *wrapping_label(QWidget *parent, bool bold = false)
{
	auto *label = new QLabel(parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	if (bold) {
		QFont font = label->font();
		font.setBold(true);
		label->setFont(font);
	}
	return label;
}

} // namespace

SokasterDock::SokasterDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(kDockId);
	buildUi();

	refreshTimer_ = new QTimer(this);
	refreshTimer_->setSingleShot(true);
	refreshTimer_->setInterval(100);
	connect(refreshTimer_, &QTimer::timeout, this, &SokasterDock::refreshNow);

	tick_ = new QTimer(this);
	tick_->setInterval(1000);
	connect(tick_, &QTimer::timeout, this, &SokasterDock::reevaluate);

	signal_handler_t *handler = obs_get_signal_handler();
	for (const char *name : kSourceSignals)
		signal_handler_connect(handler, name, onSourceSignal, this);

	/* The sinks run on workers and only post here. Both workers are stopped
	 * in the destructor, and Qt drops what is still queued for a deleted
	 * receiver. */
	sokaster::LinkFlow::instance().set_sink([this](const sokaster::LinkEvent &event) {
		QMetaObject::invokeMethod(this, [this, event] { applyLinkEvent(event); }, Qt::QueuedConnection);
	});
	sokaster::StatusLoop::instance().set_streaming(obs_frontend_streaming_active());
	sokaster::StatusLoop::instance().start([this](const sokaster::ServerState &state) {
		QMetaObject::invokeMethod(this, [this, state] { applyServerState(state); }, Qt::QueuedConnection);
	});

	refreshNow();
}

SokasterDock::~SokasterDock()
{
	sokaster::LinkFlow::instance().set_sink(nullptr);
	sokaster::LinkFlow::instance().cancel();
	sokaster::StatusLoop::instance().stop();

	signal_handler_t *handler = obs_get_signal_handler();
	for (const char *name : kSourceSignals)
		signal_handler_disconnect(handler, name, onSourceSignal, this);
}

SokasterDock::RowWidgets SokasterDock::addRow(QVBoxLayout *layout, const char *title_key)
{
	RowWidgets row;
	row.title = wrapping_label(this, true);
	row.title->setText(text(title_key));
	row.status = new QLabel(this);
	row.status->setWordWrap(true);
	row.status->setTextFormat(Qt::RichText);

	layout->addSpacing(6);
	layout->addWidget(row.title);
	layout->addWidget(row.status);
	return row;
}

void SokasterDock::buildUi()
{
	/* The dock can be squeezed to a sliver; scrolling beats clipping. No
	 * styling of our own beyond the status dots: the panel inherits the OBS
	 * theme like any built-in dock. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outer->addWidget(scroll);

	auto *content = new QWidget(scroll);
	scroll->setWidget(content);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(4);

	headline_ = wrapping_label(content, true);
	layout->addWidget(headline_);

	/* Account: sign in through the site; nothing to copy or paste. */
	account_ = addRow(layout, "Dock.Account");

	signInButton_ = new QPushButton(text("Dock.Account.SignIn"), content);
	layout->addWidget(signInButton_);
	connect(signInButton_, &QPushButton::clicked, this, &SokasterDock::onSignIn);

	linkBox_ = new QWidget(content);
	auto *linkLayout = new QVBoxLayout(linkBox_);
	linkLayout->setContentsMargins(0, 0, 0, 0);
	linkCode_ = wrapping_label(linkBox_, true);
	linkCode_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *waiting = wrapping_label(linkBox_);
	waiting->setText(text("Dock.Account.Waiting"));
	/* The address as text too, for a browser that did not open. */
	linkUrl_ = wrapping_label(linkBox_);
	linkUrl_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *cancelLink = new QPushButton(text("Dock.Account.Cancel"), linkBox_);
	linkLayout->addWidget(linkCode_);
	linkLayout->addWidget(waiting);
	linkLayout->addWidget(linkUrl_);
	linkLayout->addWidget(cancelLink, 0, Qt::AlignLeft);
	linkBox_->hide();
	layout->addWidget(linkBox_);
	connect(cancelLink, &QPushButton::clicked, this, [this] {
		sokaster::LinkFlow::instance().cancel();
		linking_ = false;
		reevaluate();
	});

	linkStatus_ = wrapping_label(content);
	linkStatus_->hide();
	layout->addWidget(linkStatus_);

	signOutButton_ = new QPushButton(text("Dock.Account.SignOut"), content);
	signOutButton_->setFlat(true);
	signOutButton_->hide();
	layout->addWidget(signOutButton_, 0, Qt::AlignLeft);
	connect(signOutButton_, &QPushButton::clicked, this, &SokasterDock::onSignOut);

	overlay_ = addRow(layout, "Dock.Overlay");
	overlayButton_ = new QPushButton(content);
	overlayButton_->hide();
	layout->addWidget(overlayButton_);
	connect(overlayButton_, &QPushButton::clicked, this, &SokasterDock::onOverlayFix);

	/* For eyes and ears the choice goes under the title, above its outcome. */
	eyes_ = addRow(layout, "Dock.Eyes");
	watches_ = new QComboBox(content);
	watches_->setToolTip(text("Dock.Eyes.Hint"));
	watches_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	layout->insertWidget(layout->indexOf(eyes_.status), watches_);
	connect(watches_, &QComboBox::activated, this, &SokasterDock::onWatchesActivated);

	ears_ = addRow(layout, "Dock.Hears");
	sources_ = new QListWidget(content);
	sources_->setSelectionMode(QAbstractItemView::NoSelection);
	sources_->setToolTip(text("Dock.Hears.Hint"));
	sources_->setMaximumHeight(130);
	layout->insertWidget(layout->indexOf(ears_.status), sources_);
	connect(sources_, &QListWidget::itemChanged, this, &SokasterDock::onSourceToggled);

	air_ = addRow(layout, "Dock.Air");

	reconnectButton_ = new QPushButton(text("Dock.Reconnect"), content);
	reconnectButton_->setToolTip(text("Dock.Reconnect.Hint"));
	reconnectButton_->hide();
	layout->addWidget(reconnectButton_);
	connect(reconnectButton_, &QPushButton::clicked, this, &SokasterDock::onReconnect);

	layout->addStretch(1);

	auto *dashboard = new QPushButton(text("Dock.Cabinet"), content);
	dashboard->setToolTip(text("Dock.Cabinet.Hint"));
	layout->addWidget(dashboard);
	connect(dashboard, &QPushButton::clicked, this, [] {
		const std::string url = sokaster::config::get().base_url() + kDashboardPath;
		QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
	});
}

void SokasterDock::onSourceSignal(void *data, calldata_t *)
{
	/* Any thread: only ask the UI thread for a refresh. */
	QMetaObject::invokeMethod(static_cast<SokasterDock *>(data), "refreshSources", Qt::QueuedConnection);
}

void SokasterDock::refreshSources()
{
	refreshTimer_->start();
}

void SokasterDock::refreshNow()
{
	refreshWatches();
	refreshHears();
	reevaluate();
}

void SokasterDock::refreshWatches()
{
	/* Refilling an open drop-down closes it under the cursor; the next
	 * refresh will catch up. */
	if (watches_->view()->isVisible())
		return;

	const std::string selected = sokaster::VisionLoop::instance().selected_uuid();

	populating_ = true;
	watches_->clear();
	for (const auto &info : sokaster::list_capture_sources())
		watches_->addItem(QString::fromStdString(info.name), QString::fromStdString(info.uuid));

	int current = watches_->findData(QString::fromStdString(selected));

	/* The eyes follow the source, not the scene: a selection from another
	 * scene stays in the list instead of vanishing on a scene switch. */
	if (current < 0 && !selected.empty()) {
		if (obs_source_t *source = obs_get_source_by_uuid(selected.c_str())) {
			watches_->addItem(QString::fromUtf8(obs_source_get_name(source)),
					  QString::fromStdString(selected));
			current = watches_->count() - 1;
			obs_source_release(source);
		}
	}

	watches_->setCurrentIndex(current);
	watches_->setEnabled(watches_->count() > 0);
	populating_ = false;
}

void SokasterDock::refreshHears()
{
	const int track = sokaster::AudioLoop::instance().track();

	populating_ = true;
	sources_->clear();
	if (track >= 0) {
		for (const auto &info : sokaster::tracks::list_audio_sources(static_cast<size_t>(track))) {
			auto *item = new QListWidgetItem(QString::fromStdString(info.name), sources_);
			item->setData(kUuidRole, QString::fromStdString(info.uuid));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(info.heard ? Qt::Checked : Qt::Unchecked);
		}
	}
	sources_->setEnabled(track >= 0);
	populating_ = false;
}

void SokasterDock::onSourceToggled(QListWidgetItem *item)
{
	const int track = sokaster::AudioLoop::instance().track();
	if (populating_ || track < 0)
		return;

	const QByteArray uuid = item->data(kUuidRole).toString().toUtf8();
	sokaster::tracks::set_source_heard(uuid.constData(), static_cast<size_t>(track),
					   item->checkState() == Qt::Checked);
	reevaluate();
}

void SokasterDock::onWatchesActivated(int index)
{
	if (populating_ || index < 0)
		return;

	const QByteArray uuid = watches_->itemData(index).toString().toUtf8();
	if (obs_source_t *source = obs_get_source_by_uuid(uuid.constData())) {
		sokaster::VisionLoop::instance().select_source(source);
		obs_source_release(source);
	}
	reevaluate();
}

void SokasterDock::paintRow(RowWidgets &row, const Readiness::Row &value)
{
	const char *colour = level_colour(value.level);
	const QString escaped = value.text.toHtmlEscaped();

	row.status->setText(
		colour ? QStringLiteral("<span style=\"color:%1\">●</span> %2").arg(QLatin1String(colour), escaped)
		       : escaped);
	row.status->setVisible(colour || !value.text.isEmpty());
}

void SokasterDock::reevaluate()
{
	auto &vision = sokaster::VisionLoop::instance();
	auto &audio = sokaster::AudioLoop::instance();

	sokaster::LocalFacts local;
	local.configured = sokaster::config::get().configured();
	local.streaming = obs_frontend_streaming_active();

	/* Asking by UUID is what tells a deleted selection from a live one. */
	const std::string selected = vision.selected_uuid();
	if (obs_source_t *source = selected.empty() ? nullptr : obs_get_source_by_uuid(selected.c_str())) {
		local.eyes_selected = true;
		lastEyesName_ = QString::fromUtf8(obs_source_get_name(source));
		obs_source_release(source);
	}
	local.eyes_name = lastEyesName_;
	local.no_capture_sources = watches_->count() == 0;
	local.vision = vision.health();

	local.audio_track = audio.track();
	local.hearing_anything = audio.hearing_anything();
	local.audio = audio.health();

	local.overlay = sokaster::find_overlay(sokaster::config::get().base_url(), server_.overlay_token);

	const Readiness r = sokaster::evaluate_readiness(local, server_, os_gettime_ns());

	headline_->setText(r.headline);
	headline_->setVisible(!r.headline.isEmpty());
	paintRow(account_, r.account);
	paintRow(overlay_, r.overlay);
	paintRow(eyes_, r.eyes);
	paintRow(ears_, r.ears);
	paintRow(air_, r.air);

	signInButton_->setVisible(r.ask_to_sign_in && !linking_);
	linkBox_->setVisible(r.ask_to_sign_in && linking_);
	linkStatus_->setVisible(r.ask_to_sign_in && !linking_ && !linkStatus_->text().isEmpty());
	signOutButton_->setVisible(local.configured && !r.ask_to_sign_in);
	reconnectButton_->setVisible(local.configured && !r.ask_to_sign_in);

	overlayFix_ = r.overlay_fix;
	overlayFound_ = local.overlay;
	overlayButton_->setVisible(overlayFix_ != Readiness::OverlayFix::None);
	overlayButton_->setText(text(overlay_fix_label(overlayFix_)));
	/* An address needs the token; until the server names it, an overlay
	 * would point nowhere. Sound fixes need no address. */
	const bool sound_fix = overlayFix_ == Readiness::OverlayFix::Listen ||
			       overlayFix_ == Readiness::OverlayFix::Sound;
	overlayButton_->setEnabled(sound_fix || !server_.overlay_token.empty());

	/* Ticks only while something on screen counts down or can change
	 * without a signal. An idle dock does no periodic work. */
	const bool tick = local.configured && (local.streaming || server_.link == sokaster::ServerState::Link::Offline);
	if (tick && !tick_->isActive())
		tick_->start();
	else if (!tick && tick_->isActive())
		tick_->stop();
}

void SokasterDock::applyServerState(const sokaster::ServerState &state)
{
	server_ = state;

	/* The server lost our session — restarted, most likely. The eyes open it
	 * again; restarting the stream is not the streamer's job. */
	if (state.link == sokaster::ServerState::Link::Ok && state.run_state == "SessionStopped" &&
	    obs_frontend_streaming_active())
		sokaster::VisionLoop::instance().reopen_session();

	/* Also the moment to notice scene edits no signal reported, such as an
	 * existing source added to a scene. */
	refreshWatches();
	reevaluate();
}

void SokasterDock::onSignIn()
{
	linking_ = true;
	linkCode_->clear();
	linkUrl_->clear();
	linkStatus_->clear();
	sokaster::LinkFlow::instance().start(sokaster::config::get().base_url());
	reevaluate();
}

void SokasterDock::applyLinkEvent(const sokaster::LinkEvent &event)
{
	using Kind = sokaster::LinkEvent::Kind;

	if (event.kind == Kind::Code) {
		linkCode_->setText(text("Dock.Account.Code").arg(QString::fromStdString(event.user_code)));

		/* Only a page of our own server: the address came over the network,
		 * and opening whatever arrives is how a browser gets sent astray. */
		const std::string prefix = sokaster::config::get().base_url() + "/";
		if (event.url.rfind(prefix, 0) == 0) {
			linkUrl_->setText(QString::fromStdString(event.url));
			QDesktopServices::openUrl(QUrl(QString::fromStdString(event.url)));
		} else {
			obs_log(LOG_WARNING, "link: not opening a sign-in page outside the server");
		}
		return;
	}

	linking_ = false;
	if (event.kind == Kind::Failed) {
		linkStatus_->setText(text("Dock.Account.Failed"));
		reevaluate();
		return;
	}

	sokaster::Config cfg = sokaster::config::get();
	cfg.access_key = event.access_key;
	sokaster::config::set(cfg);

	sokaster::AudioLoop::instance().resolve_track();
	sokaster::StatusLoop::instance().reconfigure();

	/* The workers read the key when they start: mid-stream, restart them. */
	if (obs_frontend_streaming_active())
		restart_workers();
	refreshNow();
}

void SokasterDock::onSignOut()
{
	sokaster::Config cfg = sokaster::config::get();
	cfg.access_key.clear();
	sokaster::config::set(cfg);
	obs_log(LOG_INFO, "config: signed out");

	sokaster::VisionLoop::instance().stop();
	sokaster::AudioLoop::instance().stop();
	sokaster::StatusLoop::instance().reconfigure();
	linkStatus_->clear();
	reevaluate();
}

void SokasterDock::onOverlayFix()
{
	using Fix = Readiness::OverlayFix;

	const std::string url = sokaster::overlay_url(sokaster::config::get().base_url(), server_.overlay_token);
	bool done = false;
	if (overlayFix_ == Fix::Add)
		done = sokaster::add_overlay_to_current_scene(overlayFound_, url,
							      obs_module_text("Dock.Overlay.SourceName"),
							      sokaster::AudioLoop::instance().track());
	else if (overlayFix_ == Fix::Update)
		done = sokaster::set_overlay_url(overlayFound_.source_uuid, url);
	else if (overlayFix_ == Fix::Listen || overlayFix_ == Fix::Sound)
		done = sokaster::fix_overlay_sound(overlayFound_.source_uuid);

	if (!done) {
		/* Said on the button itself; the reason is in the OBS log. */
		overlayButton_->setText(text("Dock.Overlay.Failed"));
		return;
	}

	poke_status_soon(this);
	refreshNow();
}

/*
 * Everything a stream restart would redo, short of the restart: a fresh
 * session with fresh workers, a reloaded overlay page, a poll without backoff.
 * For the case the automatic retries did not cover.
 */
void SokasterDock::onReconnect()
{
	obs_log(LOG_INFO, "dock: reconnect requested");

	/* Off air the workers are not running; starting them would start the
	 * co-host without a stream. */
	if (obs_frontend_streaming_active())
		restart_workers();

	if (overlayFound_.where != sokaster::OverlayPresence::Where::Missing)
		sokaster::reload_overlay(overlayFound_.source_uuid);

	sokaster::StatusLoop::instance().reconfigure();
	poke_status_soon(this);

	/* One click is enough; a second one would only cut the first short. */
	reconnectButton_->setEnabled(false);
	QTimer::singleShot(5000, this, [this] { reconnectButton_->setEnabled(true); });
	refreshNow();
}
