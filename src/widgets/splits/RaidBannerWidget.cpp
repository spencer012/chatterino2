// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/RaidBannerWidget.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/StreamLink.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace std::chrono_literals;
using namespace Qt::Literals;

namespace chatterino {

namespace {

constexpr auto MUTED_STYLE = "color: #adadb8;";

Split *findSplitForChannel(const QString &login)
{
    auto *windows = getApp()->getWindows();
    if (windows == nullptr || login.isEmpty())
    {
        return nullptr;
    }

    Split *found = nullptr;
    for (auto *window : windows->windows())
    {
        window->getNotebook().forEachSplit([&](Split *split) {
            if (found != nullptr || split == nullptr)
            {
                return;
            }

            if (split->getChannel()->getName().compare(
                    login, Qt::CaseInsensitive) == 0)
            {
                found = split;
            }
        });

        if (found != nullptr)
        {
            break;
        }
    }

    return found;
}

void openOrFocusChannel(const QString &login)
{
    if (login.isEmpty())
    {
        return;
    }

    if (auto *existing = findSplitForChannel(login))
    {
        getApp()->getWindows()->select(existing);
        if (auto *win = existing->window())
        {
            win->raise();
            win->activateWindow();
        }
        return;
    }

    auto channel = getApp()->getTwitch()->getOrAddChannel(login);
    auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
    SplitContainer *container = nb.addPage(true);
    auto *split = new Split(container);
    split->setChannel(channel);
    container->insertSplit(split);
}

}  // namespace

RaidBannerWidget::RaidBannerWidget(Split *split)
    : BaseWidget(split)
    , split_(split)
    , avatarLabel_(new QLabel(this))
    , titleLabel_(new QLabel(this))
    , infoLabel_(new QLabel(this))
    , countdownLabel_(new QLabel(this))
    , closeButton_(new QPushButton(u"\u00D7"_s, this))
    , streamlinkButton_(new QPushButton(u"Open in Streamlink"_s, this))
    , goToButton_(new QPushButton(u"Open in new tab"_s, this))
    , autoCheck_(new QCheckBox(u"Auto"_s, this))
    , countdownTimer_(new QTimer(this))
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *outerBox = new QVBoxLayout(this);
    outerBox->setContentsMargins(0, 0, 0, 0);
    outerBox->setSpacing(0);

    auto *contentBox = new QVBoxLayout();
    contentBox->setContentsMargins(8, 6, 8, 6);
    contentBox->setSpacing(4);

    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(8);

    this->avatarLabel_->setFixedSize(40, 40);
    this->avatarLabel_->setScaledContents(true);
    this->avatarLabel_->hide();
    headerRow->addWidget(this->avatarLabel_, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout();
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);

    this->titleLabel_->setTextFormat(Qt::RichText);
    this->titleLabel_->setWordWrap(true);
    textCol->addWidget(this->titleLabel_);

    this->infoLabel_->setWordWrap(true);
    this->infoLabel_->setStyleSheet(MUTED_STYLE);
    textCol->addWidget(this->infoLabel_);
    headerRow->addLayout(textCol, 1);

    this->countdownLabel_->setStyleSheet(MUTED_STYLE);
    this->countdownLabel_->hide();
    headerRow->addWidget(this->countdownLabel_, 0, Qt::AlignTop);

    this->closeButton_->setFlat(true);
    this->closeButton_->setFixedSize(24, 24);
    this->closeButton_->setToolTip(u"Dismiss raid banner"_s);
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    headerRow->addWidget(this->closeButton_, 0, Qt::AlignTop);
    contentBox->addLayout(headerRow);

    auto *actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);
    this->streamlinkButton_->setCursor(Qt::PointingHandCursor);
    this->goToButton_->setCursor(Qt::PointingHandCursor);
    this->autoCheck_->setToolTip(
        u"Open or jump to the raid target when the raid goes through"_s);
    actionRow->addWidget(this->streamlinkButton_);
    actionRow->addWidget(this->goToButton_);
    actionRow->addWidget(this->autoCheck_);
    actionRow->addStretch(1);
    contentBox->addLayout(actionRow);

    outerBox->addLayout(contentBox);

    auto *bottomBorder = new QWidget(this);
    bottomBorder->setFixedHeight(1);
    bottomBorder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomBorder->setAutoFillBackground(true);
    {
        QPalette pal = bottomBorder->palette();
        pal.setColor(QPalette::Window, pal.color(QPalette::Mid));
        bottomBorder->setPalette(pal);
    }
    outerBox->addWidget(bottomBorder);

    this->countdownTimer_->setInterval(1s);
    QObject::connect(this->countdownTimer_, &QTimer::timeout, this, [this] {
        this->tickCountdown();
    });

    QObject::connect(this->closeButton_, &QPushButton::clicked, this, [this] {
        this->userDismissed_ = true;
        this->hide();
    });
    QObject::connect(this->streamlinkButton_, &QPushButton::clicked, this,
                     [this] {
                         this->openTargetInStreamlink();
                     });
    QObject::connect(this->goToButton_, &QPushButton::clicked, this, [this] {
        this->goToTarget();
    });

    if (this->split_ != nullptr)
    {
        this->split_->installEventFilter(this);
    }

    this->scaleChangedEvent(this->scale());
    this->hide();
}

void RaidBannerWidget::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    this->channel_ = channel;
    this->userDismissed_ = false;
    this->lastRaidId_.clear();
    this->autoCheck_->setChecked(false);
    this->avatarUrl_.clear();
    this->avatarLabel_->clear();
    this->avatarLabel_->hide();

    if (channel)
    {
        this->signalHolder_.managedConnect(channel->raidChanged, [this] {
            this->refresh();
        });
    }

    this->refresh();
}

void RaidBannerWidget::refresh()
{
    if (!this->channel_)
    {
        this->countdownTimer_->stop();
        this->userDismissed_ = false;
        this->hide();
        return;
    }

    const auto *raid = this->channel_->getRaidState();
    if (!raid)
    {
        this->countdownTimer_->stop();
        this->userDismissed_ = false;
        this->lastRaidId_.clear();
        this->hide();
        return;
    }

    if (raid->phase == RaidPhase::GoneThrough)
    {
        if (this->split_ != nullptr && this->split_->isVisible() &&
            this->autoCheck_->isChecked() &&
            this->channel_->tryConsumeRaidAutoFollow())
        {
            this->goToTarget();
        }
        this->countdownTimer_->stop();
        this->hide();
        return;
    }

    if (raid->phase == RaidPhase::Cancelled)
    {
        this->countdownTimer_->stop();
        this->hide();
        return;
    }

    if (this->lastRaidId_ != raid->info.id)
    {
        this->lastRaidId_ = raid->info.id;
        this->userDismissed_ = false;
        this->autoCheck_->setChecked(false);
    }

    this->titleLabel_->setText(
        u"Raiding <b>%1</b>"_s.arg(raid->info.targetName().toHtmlEscaped()));

    QStringList details;
    if (raid->streamInfoFetched)
    {
        if (raid->live)
        {
            if (!raid->streamTitle.isEmpty())
            {
                details << raid->streamTitle;
            }
            if (!raid->gameName.isEmpty())
            {
                details << raid->gameName;
            }
            if (raid->streamViewerCount > 0)
            {
                details << u"%1 viewers"_s.arg(raid->streamViewerCount);
            }
        }
        else
        {
            details << u"Offline"_s;
        }
    }
    if (raid->info.viewerCount > 0)
    {
        details << u"%1 in raid"_s.arg(raid->info.viewerCount);
    }
    this->infoLabel_->setText(details.join(u" \u00B7 "_s));
    this->infoLabel_->setVisible(!details.isEmpty());

    this->loadAvatar(raid->info.targetProfileImage);
    this->refreshGoToButton();

    this->tickCountdown();
    if (raid->info.forceRaidNowSeconds > 0)
    {
        this->countdownTimer_->start();
    }

    this->refreshVisibility();
}

void RaidBannerWidget::refreshVisibility()
{
    const auto *raid = this->channel_ ? this->channel_->getRaidState() : nullptr;
    const bool shouldShow = raid != nullptr &&
                            raid->phase == RaidPhase::Active &&
                            !this->userDismissed_ && this->split_ != nullptr &&
                            this->split_->isVisible();
    this->setVisible(shouldShow);
}

void RaidBannerWidget::refreshGoToButton()
{
    const auto login = this->targetLogin();
    if (login.isEmpty())
    {
        this->goToButton_->setText(u"Open in new tab"_s);
        return;
    }

    if (findSplitForChannel(login) != nullptr)
    {
        this->goToButton_->setText(u"Go to channel"_s);
    }
    else
    {
        this->goToButton_->setText(u"Open in new tab"_s);
    }
}

void RaidBannerWidget::tickCountdown()
{
    const auto *raid = this->channel_ ? this->channel_->getRaidState() : nullptr;
    if (!raid || raid->info.forceRaidNowSeconds <= 0)
    {
        this->countdownLabel_->hide();
        return;
    }

    const int remaining = raid->remainingSeconds();
    const int mins = remaining / 60;
    const int secs = remaining % 60;
    this->countdownLabel_->setText(u"\u23F1 %1:%2"_s.arg(mins, 2, 10, QChar(u'0'))
                                       .arg(secs, 2, 10, QChar(u'0')));
    this->countdownLabel_->show();
}

void RaidBannerWidget::loadAvatar(const QString &url)
{
    if (url.isEmpty() || url == this->avatarUrl_)
    {
        return;
    }

    this->avatarUrl_ = url;
    static auto *manager = new QNetworkAccessManager();
    auto *reply = manager->get(QNetworkRequest(QUrl(url)));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        if (this->avatarUrl_ != url)
        {
            return;
        }
        if (reply->error() != QNetworkReply::NoError)
        {
            this->avatarLabel_->hide();
            return;
        }

        QPixmap avatar;
        if (!avatar.loadFromData(reply->readAll()))
        {
            this->avatarLabel_->hide();
            return;
        }

        this->avatarLabel_->setPixmap(avatar);
        this->avatarLabel_->show();
    });
}

void RaidBannerWidget::openTargetInStreamlink()
{
    const auto login = this->targetLogin();
    if (login.isEmpty())
    {
        return;
    }

    try
    {
        openStreamlinkForChannelOrUrl(login);
    }
    catch (const Exception &ex)
    {
        qCWarning(chatterinoWidget)
            << "Error opening streamlink for raid target:" << ex.what();
    }
}

void RaidBannerWidget::goToTarget()
{
    openOrFocusChannel(this->targetLogin());
}

QString RaidBannerWidget::targetLogin() const
{
    const auto *raid = this->channel_ ? this->channel_->getRaidState() : nullptr;
    if (!raid)
    {
        return {};
    }
    return raid->info.targetLogin;
}

void RaidBannerWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    auto *theme = getTheme();

    painter.fillRect(event->rect(), theme->splits.header.background);
    painter.setPen(theme->splits.header.border);
    painter.drawLine(0, 0, this->width() - 1, 0);
}

void RaidBannerWidget::scaleChangedEvent(float newScale)
{
    QFont titleFont = this->titleLabel_->font();
    titleFont.setPointSizeF(10.0F * newScale);
    this->titleLabel_->setFont(titleFont);
    this->countdownLabel_->setFont(titleFont);

    QFont infoFont = this->infoLabel_->font();
    infoFont.setPointSizeF(9.0F * newScale);
    this->infoLabel_->setFont(infoFont);

    const int avatar = int(40 * newScale);
    this->avatarLabel_->setFixedSize(avatar, avatar);

    const int close = std::max(20, int(24 * newScale));
    this->closeButton_->setFixedSize(close, close);
}

void RaidBannerWidget::mousePressEvent(QMouseEvent * /*event*/)
{
    // ignore to disable the parent's right click menu
}

bool RaidBannerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->split_ &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide))
    {
        this->refreshVisibility();
    }
    return BaseWidget::eventFilter(watched, event);
}

}  // namespace chatterino
