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

#include "overlay-finder.hpp"
#include "plugin-config.hpp"
#include "audio/audio-loop.hpp"
#include "audio/audio-tap.hpp"
#include "vision/source-picker.hpp"
#include "vision/vision-loop.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cstring>

namespace {

using sokaster::Readiness;

/* Carries the source UUID on list items; names change, UUIDs do not. */
constexpr int kUuidRole = Qt::UserRole + 1;

/* What the server accepts for a prompt (PersonaOrderService.MaxPromptLen). */
constexpr int kMaxPromptLength = 400;

/* The rest of the dashboard lives on the profile page. */
constexpr const char *kDashboardPath = "/profile";

const char *const kSourceSignals[] = {"source_create", "source_destroy", "source_rename"};

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Status dots carry the level; the words carry the meaning, so the colours
 * only have to be told apart, not read. Mid-tones that hold up on both the
 * dark and the light OBS themes. */
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

/* The server names categories after its enum (SelfHarm); the locale keys
 * are the snake_case ids the safety judge uses (self_harm). */
std::string category_key(const std::string &id)
{
	std::string key = "Dock.Category.";
	for (size_t i = 0; i < id.size(); ++i) {
		const char c = id[i];
		if (c >= 'A' && c <= 'Z') {
			if (i > 0)
				key += '_';
			key += static_cast<char>(c - 'A' + 'a');
		} else {
			key += c;
		}
	}
	return key;
}

QString category_label(const std::string &id)
{
	const std::string key = category_key(id);
	const char *label = nullptr;
	if (obs_module_get_string(key.c_str(), &label) && label)
		return QString::fromUtf8(label);

	/* A category the server learned after this build: its id is still more
	 * useful to the streamer than nothing. */
	return QString::fromStdString(id);
}

/* A refusal in the streamer's language, worded by its code. Empty for a code
 * this build does not know: the caller falls back to the server's text. */
QString refusal_text(const sokaster::CommandResult &result)
{
	const std::string &code = result.code;

	if (code == "blocked") {
		if (result.categories.empty())
			return text("Dock.Refusal.Blocked");
		QStringList reasons;
		for (const std::string &category : result.categories)
			reasons << category_label(category);
		return text("Dock.Refusal.BlockedWhy").arg(reasons.join(QStringLiteral(", ")));
	}
	if (code == "air_full")
		return text("Dock.Refusal.AirFull").arg(result.ahead_minutes);
	if (code == "not_admitted")
		return text("Dock.Refusal.NotAdmitted");
	if (code == "empty_prompt")
		return text("Dock.Order.Empty");
	if (code == "not_found")
		return text("Dock.Queue.Gone");

	return QString();
}

/* Voice hints come from the server in Russian. Elsewhere the id reads better
 * than a sentence in a language the streamer may not speak. */
QString voice_label(const sokaster::VoiceOption &voice)
{
	QString name = QString::fromStdString(voice.id);
	if (!name.isEmpty())
		name[0] = name[0].toUpper();

	const char *locale = obs_get_locale();
	if (locale && strncmp(locale, "ru", 2) == 0 && !voice.hint.empty())
		return name + QStringLiteral(" — ") + QString::fromStdString(voice.hint);
	return name;
}

QLabel *wrapping_label(QWidget *parent)
{
	auto *label = new QLabel(parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	return label;
}

QLabel *section_title(const char *key, QWidget *parent)
{
	auto *label = new QLabel(text(key), parent);
	QFont font = label->font();
	font.setBold(true);
	label->setFont(font);
	return label;
}

} // namespace

SokasterDock::SokasterDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(kDockId);
	buildUi();

	tick_ = new QTimer(this);
	tick_->setInterval(1000);
	connect(tick_, &QTimer::timeout, this, &SokasterDock::onTick);

	/* The sinks run on the status worker. They only post to this object; the
	 * worker is stopped in the destructor before the object goes away, and
	 * Qt drops whatever is still queued for a deleted receiver. */
	sokaster::StatusLoop::instance().set_sinks(
		[this](const sokaster::ServerState &state) {
			QMetaObject::invokeMethod(
				this, [this, state] { applyServerState(state); }, Qt::QueuedConnection);
		},
		[this](const sokaster::CommandResult &result) {
			QMetaObject::invokeMethod(
				this, [this, result] { applyCommandResult(result); }, Qt::QueuedConnection);
		});
	sokaster::StatusLoop::instance().set_streaming(obs_frontend_streaming_active());
	sokaster::StatusLoop::instance().start();

	connectSourceSignals();
	refreshSources();
}

SokasterDock::~SokasterDock()
{
	sokaster::StatusLoop::instance().stop();
	sokaster::StatusLoop::instance().set_sinks(nullptr, nullptr);
	disconnectSourceSignals();
}

SokasterDock::RowWidgets SokasterDock::addRow(QVBoxLayout *layout, const char *title_key)
{
	RowWidgets row;
	row.title = section_title(title_key, this);
	row.status = new QLabel(this);
	row.status->setWordWrap(true);
	row.status->setTextFormat(Qt::RichText);
	row.status->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	layout->addSpacing(6);
	layout->addWidget(row.title);
	layout->addWidget(row.status);
	return row;
}

void SokasterDock::buildUi()
{
	/* The dock can be squeezed to a sliver; scrolling beats clipping the
	 * queue or the order field off the bottom. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outer->addWidget(scroll);

	/* No styling of our own beyond the status dots: the dock should look
	 * like part of OBS rather than a guest, so palette and fonts are
	 * inherited from the parent. */
	auto *content = new QWidget(scroll);
	scroll->setWidget(content);

	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(4);

	headline_ = wrapping_label(content);
	QFont headlineFont = headline_->font();
	headlineFont.setBold(true);
	headline_->setFont(headlineFont);
	layout->addWidget(headline_);

	/* Account, with the key field for when there is no key or it stopped
	 * working. Twitch sign-in replaces the field in the onboarding stage. */
	account_ = addRow(layout, "Dock.Account");
	keyBox_ = new QWidget(content);
	auto *keyLayout = new QHBoxLayout(keyBox_);
	keyLayout->setContentsMargins(0, 0, 0, 0);
	keyEdit_ = new QLineEdit(keyBox_);
	keyEdit_->setEchoMode(QLineEdit::Password);
	keyEdit_->setPlaceholderText(text("Dock.Account.Key"));
	auto *saveKey = new QPushButton(text("Dock.Account.Save"), keyBox_);
	keyLayout->addWidget(keyEdit_, 1);
	keyLayout->addWidget(saveKey);
	layout->addWidget(keyBox_);
	connect(saveKey, &QPushButton::clicked, this, &SokasterDock::onSaveKey);
	connect(keyEdit_, &QLineEdit::returnPressed, this, &SokasterDock::onSaveKey);

	overlay_ = addRow(layout, "Dock.Overlay");

	eyes_ = addRow(layout, "Dock.Eyes");
	watches_ = new QComboBox(content);
	watches_->setToolTip(text("Dock.Eyes.Hint"));
	watches_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	/* Under the title, above the status: the choice first, then what came
	 * of it. */
	layout->insertWidget(layout->indexOf(eyes_.status), watches_);
	connect(watches_, &QComboBox::activated, this, &SokasterDock::onWatchesActivated);

	ears_ = addRow(layout, "Dock.Hears");
	sources_ = new QListWidget(content);
	sources_->setSelectionMode(QAbstractItemView::NoSelection);
	sources_->setToolTip(text("Dock.Hears.Hint"));
	/* A handful of sources is the norm; the list should not swallow the
	 * dock when someone has forty. */
	sources_->setMaximumHeight(130);
	layout->insertWidget(layout->indexOf(ears_.status), sources_);
	connect(sources_, &QListWidget::itemChanged, this, &SokasterDock::onSourceToggled);

	air_ = addRow(layout, "Dock.Air");
	donateButton_ = new QPushButton(text("Dock.Air.CopyDonate"), content);
	layout->addWidget(donateButton_);
	connect(donateButton_, &QPushButton::clicked, this, &SokasterDock::onCopyDonateLink);

	/* The order. */
	layout->addSpacing(10);
	layout->addWidget(section_title("Dock.Order", content));

	auto *voiceRow = new QHBoxLayout();
	voiceRow->addWidget(new QLabel(text("Dock.Order.Voice"), content));
	voice_ = new QComboBox(content);
	voice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	voice_->addItem(text("Dock.Order.VoiceDefault"));
	voiceRow->addWidget(voice_, 1);
	layout->addLayout(voiceRow);

	orderEdit_ = new QPlainTextEdit(content);
	orderEdit_->setPlaceholderText(text("Dock.Order.Placeholder"));
	orderEdit_->setTabChangesFocus(true);
	/* Three lines visible: enough to read a prompt back, not so much that
	 * the queue disappears below the fold. */
	const int lineHeight = orderEdit_->fontMetrics().lineSpacing();
	orderEdit_->setFixedHeight(lineHeight * 3 + 14);
	orderEdit_->installEventFilter(this);
	layout->addWidget(orderEdit_);
	connect(orderEdit_, &QPlainTextEdit::textChanged, this, &SokasterDock::onOrderTextChanged);

	auto *sendRow = new QHBoxLayout();
	orderCounter_ = new QLabel(content);
	sendButton_ = new QPushButton(QStringLiteral("▶"), content);
	sendButton_->setToolTip(text("Dock.Order.Send"));
	sendRow->addWidget(orderCounter_, 1);
	sendRow->addWidget(sendButton_);
	layout->addLayout(sendRow);
	connect(sendButton_, &QPushButton::clicked, this, &SokasterDock::onSendOrder);

	orderStatus_ = wrapping_label(content);
	layout->addWidget(orderStatus_);

	/* The queue. */
	layout->addSpacing(10);
	layout->addWidget(section_title("Dock.Queue", content));
	queueStatus_ = wrapping_label(content);
	queueStatus_->hide();
	layout->addWidget(queueStatus_);
	queueEmpty_ = wrapping_label(content);
	queueEmpty_->setText(text("Dock.Queue.Empty"));
	layout->addWidget(queueEmpty_);
	queueLayout_ = new QVBoxLayout();
	queueLayout_->setSpacing(6);
	layout->addLayout(queueLayout_);

	layout->addStretch(1);

	auto *dashboard = new QPushButton(text("Dock.Cabinet"), content);
	dashboard->setToolTip(text("Dock.Cabinet.Hint"));
	layout->addWidget(dashboard);
	connect(dashboard, &QPushButton::clicked, this, &SokasterDock::onOpenDashboard);

	onOrderTextChanged();
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
	refreshWatches();
	refreshHears();
	reevaluate();
}

void SokasterDock::refreshWatches()
{
	/* Refilling an open drop-down closes it under the streamer's cursor.
	 * The next refresh will catch up. */
	if (watches_->view() && watches_->view()->isVisible())
		return;

	auto &grabber = sokaster::VisionLoop::instance().grabber();
	const std::string selected = grabber.selected_uuid();

	populating_ = true;
	watches_->clear();

	int current = -1;
	for (const auto &info : sokaster::list_capture_sources()) {
		watches_->addItem(QString::fromStdString(info.name), QString::fromStdString(info.uuid));
		if (info.uuid == selected)
			current = watches_->count() - 1;
	}

	/* Watching something outside this scene is allowed — the eyes follow the
	 * source, not the scene — so it stays in the list rather than vanishing
	 * the moment the streamer switches scenes. */
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
		const auto items = sokaster::tracks::list_audio_sources(static_cast<size_t>(track));
		for (const auto &info : items) {
			auto *item = new QListWidgetItem(QString::fromStdString(info.name), sources_);
			item->setData(kUuidRole, QString::fromStdString(info.uuid));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(info.heard ? Qt::Checked : Qt::Unchecked);
		}
	}

	/* Six tracks and no free one: nothing to tick, and the row says why. */
	sources_->setEnabled(track >= 0);
	populating_ = false;
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

sokaster::LocalFacts SokasterDock::gatherLocal()
{
	sokaster::LocalFacts local;
	const sokaster::Config cfg = sokaster::config::get();

	local.configured = cfg.configured();
	local.streaming = obs_frontend_streaming_active();

	/* A weak reference outlives its source; asking by UUID is what tells a
	 * deleted selection from a live one while the eyes are idle. */
	auto &grabber = sokaster::VisionLoop::instance().grabber();
	const std::string selected = grabber.selected_uuid();
	if (!selected.empty()) {
		if (obs_source_t *source = obs_get_source_by_uuid(selected.c_str())) {
			local.eyes_selected = true;
			lastEyesName_ = QString::fromUtf8(obs_source_get_name(source));
			obs_source_release(source);
		}
	}
	local.eyes_name = lastEyesName_;
	local.no_capture_sources = watches_->count() == 0;
	local.vision = sokaster::VisionLoop::instance().health();

	auto &audio = sokaster::AudioLoop::instance();
	local.audio_track = audio.track();
	local.hearing_anything = audio.hearing_anything();
	local.audio = audio.health();

	local.overlay = sokaster::find_overlay(server_.overlay_token);
	return local;
}

void SokasterDock::paintRow(RowWidgets &row, const Readiness::Row &value)
{
	const char *colour = level_colour(value.level);
	const QString escaped = value.text.toHtmlEscaped();

	if (!colour) {
		row.status->setText(escaped);
	} else {
		row.status->setText(
			QStringLiteral("<span style=\"color:%1\">●</span> %2").arg(QLatin1String(colour), escaped));
	}
	row.status->setVisible(!value.text.isEmpty() || colour);
}

void SokasterDock::reevaluate()
{
	const sokaster::LocalFacts local = gatherLocal();
	const Readiness r = sokaster::evaluate_readiness(local, server_, os_gettime_ns());

	headline_->setText(r.headline);
	headline_->setVisible(!r.headline.isEmpty());

	paintRow(account_, r.account);
	paintRow(overlay_, r.overlay);
	paintRow(eyes_, r.eyes);
	paintRow(ears_, r.ears);
	paintRow(air_, r.air);

	keyBox_->setVisible(r.ask_for_key);
	donateButton_->setVisible(r.offer_donate_link);

	const bool canOrder = r.order_enabled && !sending_;
	orderEdit_->setReadOnly(!r.order_enabled);
	voice_->setEnabled(r.order_enabled);
	sendButton_->setEnabled(canOrder);

	if (!orderStatusOwned_)
		orderStatus_->setText(r.order_enabled ? QString() : text("Dock.Order.Locked"));

	updateTimer();
}

void SokasterDock::applyServerState(const sokaster::ServerState &state)
{
	const bool voicesChanged = state.voices.size() != voiceIds_.size();
	server_ = state;

	if (voicesChanged)
		rebuildVoices();

	rebuildQueue();

	/* The poll is also the moment to notice scene edits no signal told us
	 * about — an item added to a scene, a capture that finished loading. */
	refreshWatches();
	reevaluate();
}

void SokasterDock::rebuildVoices()
{
	const QString current = voice_->currentData().toString();

	voice_->clear();
	voiceIds_.clear();
	voice_->addItem(text("Dock.Order.VoiceDefault"), QString());

	for (const auto &voice : server_.voices) {
		voice_->addItem(voice_label(voice), QString::fromStdString(voice.id));
		voiceIds_.push_back(voice.id);
	}

	const int index = voice_->findData(current);
	voice_->setCurrentIndex(index >= 0 ? index : 0);
}

int SokasterDock::secondsSinceOrders() const
{
	if (server_.orders_received_ns == 0)
		return 0;
	const uint64_t now = os_gettime_ns();
	return now > server_.orders_received_ns ? static_cast<int>((now - server_.orders_received_ns) / 1000000000ull)
						: 0;
}

QString SokasterDock::queueLine(const sokaster::QueuedOrder &order, int elapsed) const
{
	const QString from = order.own ? text("Dock.Queue.Own") : QString::fromStdString(order.from);
	const auto left = [elapsed](int seconds) {
		return sokaster::format_clock(seconds - elapsed);
	};

	if (order.state == "onair")
		return text("Dock.Queue.OnAir").arg(left(order.seconds_left), from);
	if (order.state == "pending")
		return text("Dock.Queue.Pending").arg(left(order.hold_seconds_left), from);
	return text("Dock.Queue.Queued").arg(QString::number(order.position), left(order.wait_seconds), from);
}

void SokasterDock::rebuildQueue()
{
	/* Same orders in the same states: only the clocks moved, and those
	 * tick locally. Rebuilding would drop a hover or a half-made click. */
	bool same = queueRows_.size() == server_.orders.size();
	for (size_t i = 0; same && i < queueRows_.size(); ++i)
		same = queueRows_[i].id == server_.orders[i].id && queueRows_[i].state == server_.orders[i].state;

	queueStatus_->setVisible(server_.orders_stale);
	if (server_.orders_stale)
		queueStatus_->setText(text("Dock.Queue.Stale"));

	if (same) {
		updateQueueLines();
		return;
	}

	for (QueueRow &row : queueRows_)
		delete row.frame;
	queueRows_.clear();

	QWidget *parent = queueEmpty_->parentWidget();

	for (const sokaster::QueuedOrder &order : server_.orders) {
		auto *frame = new QFrame(parent);
		frame->setFrameShape(QFrame::StyledPanel);
		auto *box = new QVBoxLayout(frame);
		box->setContentsMargins(6, 4, 6, 4);
		box->setSpacing(2);

		auto *line = wrapping_label(frame);
		box->addWidget(line);

		/* The streamer decides on a pending order by its raw text; once it
		 * is through, the character's name is what matters. */
		auto *body = wrapping_label(frame);
		if (order.state == "pending") {
			body->setText(QString::fromStdString(order.prompt));

			QStringList reasons;
			for (const std::string &category : order.categories)
				reasons << category_label(category);
			auto *why = wrapping_label(frame);
			why->setText(
				text("Dock.Queue.PendingWhy").arg(QString::number(order.risk), reasons.join(", ")));
			box->addWidget(body);
			box->addWidget(why);
		} else {
			body->setText(QString::fromStdString(order.display_name));
			box->addWidget(body);
		}

		const std::string id = order.id;
		if (order.state == "pending") {
			auto *buttons = new QHBoxLayout();
			auto *approve = new QPushButton(QStringLiteral("✓"), frame);
			approve->setToolTip(text("Dock.Queue.Approve"));
			auto *reject = new QPushButton(QStringLiteral("✕"), frame);
			reject->setToolTip(text("Dock.Queue.Reject"));
			buttons->addStretch(1);
			buttons->addWidget(approve);
			buttons->addWidget(reject);
			box->addLayout(buttons);

			connect(approve, &QPushButton::clicked, this, [approve, reject, id] {
				approve->setEnabled(false);
				reject->setEnabled(false);
				sokaster::StatusLoop::instance().approve(id);
			});
			connect(reject, &QPushButton::clicked, this, [approve, reject, id] {
				approve->setEnabled(false);
				reject->setEnabled(false);
				sokaster::StatusLoop::instance().reject(id);
			});
		} else if (order.state == "onair") {
			auto *buttons = new QHBoxLayout();
			auto *drop = new QPushButton(QStringLiteral("✖"), frame);
			drop->setToolTip(text("Dock.Queue.Drop"));
			buttons->addStretch(1);
			buttons->addWidget(drop);
			box->addLayout(buttons);

			connect(drop, &QPushButton::clicked, this, [drop] {
				drop->setEnabled(false);
				sokaster::StatusLoop::instance().drop_on_air();
			});
		}

		queueLayout_->addWidget(frame);
		queueRows_.push_back({order.id, order.state, frame, line});
	}

	queueEmpty_->setVisible(server_.have_orders && server_.orders.empty());
	updateQueueLines();
}

void SokasterDock::updateQueueLines()
{
	const int elapsed = secondsSinceOrders();
	for (size_t i = 0; i < queueRows_.size() && i < server_.orders.size(); ++i)
		queueRows_[i].line->setText(queueLine(server_.orders[i], elapsed));
}

void SokasterDock::updateTimer()
{
	/* Ticks only while something on screen counts down. An idle dock, and
	 * certainly an unconfigured one, does no periodic work. */
	const bool counting = !server_.orders.empty() || server_.link == sokaster::ServerState::Link::Offline ||
			      obs_frontend_streaming_active();

	if (counting && sokaster::config::get().configured()) {
		if (!tick_->isActive())
			tick_->start();
	} else if (tick_->isActive()) {
		tick_->stop();
	}
}

void SokasterDock::onTick()
{
	updateQueueLines();
	reevaluate();
}

bool SokasterDock::eventFilter(QObject *watched, QEvent *event)
{
	/* Enter sends, as in every chat box; Shift+Enter is swallowed rather
	 * than starting a new line the server would flatten anyway. */
	if (watched == orderEdit_ && event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
			if (!(key->modifiers() & Qt::ShiftModifier))
				onSendOrder();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void SokasterDock::onOrderTextChanged()
{
	QString value = orderEdit_->toPlainText();

	/* Typing again means the last verdict has been read. Clearing the field
	 * after a sent order must not wipe the verdict, hence the emptiness
	 * check. */
	if (!value.isEmpty() && !sending_)
		orderStatusOwned_ = false;

	if (value.size() > kMaxPromptLength) {
		/* Pasted past the limit: cut, and keep the cursor at the end
		 * rather than throwing it back to the start. */
		value.truncate(kMaxPromptLength);
		const QSignalBlocker block(orderEdit_);
		orderEdit_->setPlainText(value);
		orderEdit_->moveCursor(QTextCursor::End);
	}

	orderCounter_->setText(text("Dock.Order.Counter").arg(value.size()).arg(kMaxPromptLength));
}

void SokasterDock::onSendOrder()
{
	if (sending_ || orderEdit_->isReadOnly())
		return;

	const QString prompt = orderEdit_->toPlainText().simplified();
	if (prompt.isEmpty()) {
		orderStatusOwned_ = true;
		orderStatus_->setText(text("Dock.Order.Empty"));
		return;
	}

	sending_ = true;
	sendButton_->setEnabled(false);
	orderStatusOwned_ = true;
	orderStatus_->setText(text("Dock.Order.Checking"));

	sokaster::StatusLoop::instance().place_order(prompt.toStdString(),
						     voice_->currentData().toString().toStdString());
}

void SokasterDock::applyCommandResult(const sokaster::CommandResult &result)
{
	using Kind = sokaster::CommandResult::Kind;

	if (result.kind == Kind::PlaceOrder) {
		sending_ = false;
		orderStatusOwned_ = true;

		if (result.ok) {
			orderEdit_->clear();
			if (result.state == "onair")
				orderStatus_->setText(text("Dock.Order.OnAir"));
			else
				orderStatus_->setText(text("Dock.Order.Queued")
							      .arg(QString::number(result.position),
								   sokaster::format_clock(result.wait_seconds)));
		} else if (result.status == 0) {
			/* The text stays: retyping a prompt because the network
			 * blinked is the kind of thing that makes people give up. */
			orderStatus_->setText(text("Dock.Order.Failed"));
		} else if (const QString worded = refusal_text(result); !worded.isEmpty()) {
			orderStatus_->setText(worded);
		} else {
			const QString reason = result.error.empty() ? QString::number(result.status)
								    : QString::fromStdString(result.error);
			orderStatus_->setText(text("Dock.Order.Refused").arg(reason));
		}

		reevaluate();
		return;
	}

	QString message;
	if (result.status == 0)
		message = text("Dock.Queue.Stale");
	else if (result.status == 404)
		message = text("Dock.Queue.Gone");
	else if (!result.ok) {
		message = refusal_text(result);
		if (message.isEmpty())
			message = result.error.empty() ? text("Dock.Queue.Gone") : QString::fromStdString(result.error);
	} else if (result.kind == Kind::Approve)
		message = text("Dock.Queue.Approved").arg(result.position);
	else if (result.kind == Kind::Reject)
		message = text("Dock.Queue.Rejected");
	else
		message = text("Dock.Queue.Dropped");

	queueStatus_->setText(message);
	queueStatus_->show();

	/* A failed click leaves its buttons disabled. Forgetting the states
	 * makes the poll that follows every click rebuild the rows, buttons
	 * enabled again. */
	if (!result.ok)
		for (QueueRow &row : queueRows_)
			row.state.clear();
}

void SokasterDock::onSaveKey()
{
	const QString key = keyEdit_->text().trimmed();
	if (key.isEmpty())
		return;

	sokaster::Config cfg = sokaster::config::get();
	cfg.access_key = key.toStdString();
	if (cfg.backend_url.empty())
		cfg.backend_url = sokaster::kDefaultBackendUrl;
	sokaster::config::set(cfg);
	keyEdit_->clear();

	obs_log(LOG_INFO, "config: access key replaced from the dock");

	sokaster::StatusLoop::instance().reconfigure();

	/* The loops read the key when they start; mid-stream that means a
	 * restart, or they keep knocking with the old one. */
	if (obs_frontend_streaming_active()) {
		sokaster::VisionLoop::instance().stop();
		sokaster::AudioLoop::instance().stop();
		sokaster::VisionLoop::instance().start();
		sokaster::AudioLoop::instance().start();
	}

	reevaluate();
}

void SokasterDock::onCopyDonateLink()
{
	if (server_.donate_url.empty())
		return;

	QGuiApplication::clipboard()->setText(QString::fromStdString(server_.donate_url));

	donateButton_->setText(text("Dock.Air.Copied"));
	QTimer::singleShot(2000, this, [this] { donateButton_->setText(text("Dock.Air.CopyDonate")); });
}

QString SokasterDock::dashboardUrl() const
{
	std::string base = sokaster::config::get().backend_url;
	if (base.empty())
		base = sokaster::kDefaultBackendUrl;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	return QString::fromStdString(base + kDashboardPath);
}

void SokasterDock::onOpenDashboard()
{
	QDesktopServices::openUrl(QUrl(dashboardUrl()));
}
