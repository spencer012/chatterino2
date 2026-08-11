// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/ChannelPointsPopup.hpp"

#include "Application.hpp"
#include "controllers/channelpoints/ChannelPointsController.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/splits/ChannelPointsConfirmDialog.hpp"
#include "widgets/splits/Split.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

void clearLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        if (auto *childLayout = item->layout())
        {
            clearLayout(childLayout);
            delete childLayout;
        }
        delete item;
    }
}

QString formatConnectionState(const ChannelPointsViewState &state)
{
    if (!state.transportConnected)
    {
        return "Disconnected";
    }
    if (!state.sessionReady)
    {
        return "Connecting...";
    }
    if (!state.subscribed)
    {
        return "Connected, waiting for subscription";
    }
    if (state.loadingBalance || state.loadingRewards)
    {
        return "Loading rewards...";
    }
    return "Connected";
}

QString favoriteKey(const QString &channelLogin, const QString &rewardId)
{
    return QString("%1|%2").arg(channelLogin, rewardId);
}

QVector<ChannelPointRewardView> sortRewards(QString channelLogin,
                                            QVector<ChannelPointRewardView> rewards)
{
    const auto favorites = getSettings()->channelPointFavorites.getValue();
    std::sort(rewards.begin(), rewards.end(),
              [&favorites, channelLogin = std::move(channelLogin)](
                  const auto &a, const auto &b) {
                  const auto aFav =
                      std::find(favorites.begin(), favorites.end(),
                                favoriteKey(channelLogin, a.reward.id)) !=
                      favorites.end();
                  const auto bFav =
                      std::find(favorites.begin(), favorites.end(),
                                favoriteKey(channelLogin, b.reward.id)) !=
                      favorites.end();
                  if (aFav != bFav)
                  {
                      return aFav > bFav;
                  }
                  if (a.reward.cost != b.reward.cost)
                  {
                      return a.reward.cost < b.reward.cost;
                  }
                  return a.reward.title.toLower() < b.reward.title.toLower();
              });
    for (auto &reward : rewards)
    {
        reward.isFavorite =
            std::find(favorites.begin(), favorites.end(),
                      favoriteKey(channelLogin, reward.reward.id)) !=
                            favorites.end();
    }
    return rewards;
}

}  // namespace

ChannelPointsPopup::ChannelPointsPopup(Split *split, QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(split->getChannel().get());
    if (twitchChannel != nullptr)
    {
        this->channelLogin_ = twitchChannel->getName().trimmed().toLower();
    }

    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QString("Channel Points - #%1").arg(this->channelLogin_));
    this->resize(440, 540);

    auto root = LayoutCreator<QWidget>(this->getLayoutContainer())
                    .setLayoutType<QVBoxLayout>();
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    this->connectionLabel_ =
        root.emplace<QLabel>().assign(&this->connectionLabel_).getElement();
    this->balanceLabel_ =
        root.emplace<QLabel>().assign(&this->balanceLabel_).getElement();
    this->statusLabel_ =
        root.emplace<QLabel>().assign(&this->statusLabel_).getElement();
    this->statusLabel_->setWordWrap(true);

    auto *buttonRow = root.emplace<QHBoxLayout>().getElement();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);

    auto *refreshButton = new QPushButton("Sync", this);
    this->clearQueueButton_ = new QPushButton("Clear", this);
    refreshButton->setToolTip("Refresh balance and rewards");
    this->clearQueueButton_->setToolTip("Emergency clear queue");
    buttonRow->addWidget(refreshButton);
    buttonRow->addWidget(this->clearQueueButton_);
    buttonRow->addStretch(1);

    QObject::connect(refreshButton, &QPushButton::clicked, this, [this] {
        if (!this->channelLogin_.isEmpty())
        {
            getApp()->getChannelPoints()->refreshChannel(this->channelLogin_);
        }
    });
    QObject::connect(this->clearQueueButton_, &QPushButton::clicked, this,
                     [this] {
                         getApp()->getChannelPoints()->clearQueue(
                             this->channelLogin_);
                     });

    this->scrollArea_ =
        root.emplace<QScrollArea>().assign(&this->scrollArea_).getElement();
    this->scrollArea_->setWidgetResizable(true);

    this->content_ = new QWidget(this->scrollArea_);
    this->contentLayout_ = new QVBoxLayout(this->content_);
    this->contentLayout_->setContentsMargins(0, 0, 0, 0);
    this->contentLayout_->setSpacing(10);
    this->scrollArea_->setWidget(this->content_);

    QObject::connect(getApp()->getChannelPoints(),
                     &ChannelPointsController::channelStateChanged, this,
                     [this](const QString &channelLogin) {
                         if (channelLogin.isEmpty() ||
                             channelLogin == this->channelLogin_)
                         {
                             this->refresh();
                         }
                     });
    this->refreshTimer_.setInterval(1000);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this,
                     [this] { this->refresh(); });

    this->refresh();
}

ChannelPointsPopup::~ChannelPointsPopup()
{
    this->detachFromController();
}

void ChannelPointsPopup::showEvent(QShowEvent *event)
{
    this->attachToController();
    this->refreshTimer_.start();
    BasePopup::showEvent(event);
}

void ChannelPointsPopup::hideEvent(QHideEvent *event)
{
    this->refreshTimer_.stop();
    this->detachFromController();
    BasePopup::hideEvent(event);
}

void ChannelPointsPopup::refresh()
{
    auto state = getApp()->getChannelPoints()->getViewState(this->channelLogin_);

    this->connectionLabel_->setText(formatConnectionState(state));
    if (state.hasBalance)
    {
        auto claimSuffix = state.availableClaim ? " | Bonus claim ready" : "";
        this->balanceLabel_->setText(QString("Pts %1%2").arg(state.balance).arg(
            claimSuffix));
    }
    else
    {
        this->balanceLabel_->setText("Pts ...");
    }

    this->statusLabel_->setText(state.lastError.isEmpty()
                                    ? QString("Queue %1")
                                          .arg(state.queuedRewardCount)
                                    : state.lastError);
    this->clearQueueButton_->setEnabled(state.queuedRewardCount > 0);

    clearLayout(this->contentLayout_);
    this->rebuildRewardSection("Custom",
                               sortRewards(this->channelLogin_, state.customRewards),
                               this->contentLayout_);
    this->rebuildRewardSection(
        "Auto", sortRewards(this->channelLogin_, state.automaticRewards),
                               this->contentLayout_);
    this->contentLayout_->addStretch(1);
}

void ChannelPointsPopup::rebuildRewardSection(
    const QString &title, const QVector<ChannelPointRewardView> &rewards,
    QVBoxLayout *layout)
{
    auto *header = new QLabel(QString("<b>%1</b>").arg(title), this->content_);
    header->setTextFormat(Qt::RichText);
    layout->addWidget(header);

    if (rewards.isEmpty())
    {
        layout->addWidget(new QLabel("No rewards available.", this->content_));
        return;
    }

    for (const auto &rewardView : rewards)
    {
        auto *card = new QFrame(this->content_);
        card->setFrameShape(QFrame::StyledPanel);

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(6, 6, 6, 6);
        cardLayout->setSpacing(4);

        auto *headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(4);

        auto *titleLabel =
            new QLabel(QString("<b>%1</b>  %2")
                           .arg(rewardView.reward.title)
                           .arg(rewardView.reward.cost),
                       card);
        titleLabel->setTextFormat(Qt::RichText);
        titleLabel->setWordWrap(true);
        headerRow->addWidget(titleLabel, 1);

        auto *favoriteButton =
            new QPushButton(rewardView.isFavorite ? "Unpin" : "Pin", card);
        favoriteButton->setToolTip("Pin this reward to the top");
        favoriteButton->setMaximumWidth(52);
        headerRow->addWidget(favoriteButton, 0, Qt::AlignRight);
        cardLayout->addLayout(headerRow);

        const auto prompt = rewardView.reward.prompt.trimmed();
        if (!prompt.isEmpty())
        {
            auto *descriptionLabel = new QLabel(prompt, card);
            descriptionLabel->setWordWrap(true);
            cardLayout->addWidget(descriptionLabel);
        }

        auto *statusLabel = new QLabel(rewardView.statusText, card);
        statusLabel->setWordWrap(true);
        cardLayout->addWidget(statusLabel);

        auto *buttonLayout = new QHBoxLayout();
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        buttonLayout->setSpacing(6);

        auto *redeemButton = new QPushButton("Go", card);
        auto *queueOnceButton = new QPushButton(
            rewardView.queueMode == ChannelPointQueueMode::Once
                ? "Stop"
                : "Once",
            card);
        auto *repeatButton = new QPushButton(
            rewardView.queueMode == ChannelPointQueueMode::Repeat
                ? "Stop"
                : "Loop",
            card);
        redeemButton->setMaximumWidth(48);
        queueOnceButton->setMaximumWidth(52);
        repeatButton->setMaximumWidth(52);
        redeemButton->setToolTip("Redeem now");
        queueOnceButton->setToolTip("Queue one redeem when available");
        repeatButton->setToolTip("Redeem repeatedly when available");

        redeemButton->setEnabled(rewardView.reward.isRedeemable &&
                                 !rewardView.inFlight);
        queueOnceButton->setEnabled(!rewardView.inFlight &&
                                    rewardView.reward.isRedeemable);
        repeatButton->setEnabled(!rewardView.inFlight &&
                                 rewardView.reward.isRedeemable);

        buttonLayout->addWidget(redeemButton);
        buttonLayout->addWidget(queueOnceButton);
        buttonLayout->addWidget(repeatButton);
        cardLayout->addLayout(buttonLayout);

        QObject::connect(redeemButton, &QPushButton::clicked, this,
                         [this, reward = rewardView.reward] {
                             this->openConfirmDialog(reward);
                         });
        QObject::connect(queueOnceButton, &QPushButton::clicked, this,
                         [this, rewardId = rewardView.reward.id] {
                             getApp()->getChannelPoints()->armQueueOnce(
                                 this->channelLogin_, rewardId);
                         });
        QObject::connect(repeatButton, &QPushButton::clicked, this,
                         [this, rewardId = rewardView.reward.id,
                          enabled = rewardView.queueMode !=
                                    ChannelPointQueueMode::Repeat] {
                             getApp()->getChannelPoints()->setRepeatMode(
                                 this->channelLogin_, rewardId, enabled);
                         });
        QObject::connect(favoriteButton, &QPushButton::clicked, this,
                         [this, rewardId = rewardView.reward.id] {
                             this->toggleFavorite(rewardId);
                         });

        layout->addWidget(card);
    }
}

void ChannelPointsPopup::openConfirmDialog(const ChannelPointRewardData &reward)
{
    if (!this->confirmDialog_.isNull())
    {
        this->confirmDialog_->close();
    }

    auto *dialog = new ChannelPointsConfirmDialog(this->channelLogin_, reward, this);
    this->confirmDialog_ = dialog;

    QObject::connect(dialog, &ChannelPointsConfirmDialog::confirmed, this,
                     [this, reward](bool /*keepDialogOpen*/) {
                         getApp()->getChannelPoints()->redeemReward(
                             this->channelLogin_, reward);
                     });

    widgets::showAndMoveWindowTo(
        dialog, this->mapToGlobal(QPoint{40, 40}),
        widgets::BoundsChecking::CursorPosition);
}

void ChannelPointsPopup::attachToController()
{
    if (this->attached_ || this->channelLogin_.isEmpty())
    {
        return;
    }

    this->attached_ = true;
    getApp()->getChannelPoints()->watchChannel(this->channelLogin_);
}

void ChannelPointsPopup::toggleFavorite(const QString &rewardId)
{
    auto favorites = getSettings()->channelPointFavorites.getValue();
    const auto key = favoriteKey(this->channelLogin_, rewardId);
    auto it = std::find(favorites.begin(), favorites.end(), key);
    if (it == favorites.end())
    {
        favorites.push_back(key);
    }
    else
    {
        favorites.erase(it);
    }
    getSettings()->channelPointFavorites.setValue(favorites);
    this->refresh();
}

void ChannelPointsPopup::detachFromController()
{
    if (!this->attached_ || this->channelLogin_.isEmpty())
    {
        return;
    }

    this->attached_ = false;
    getApp()->getChannelPoints()->unwatchChannel(this->channelLogin_);
}

}  // namespace chatterino
