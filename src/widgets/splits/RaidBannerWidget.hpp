// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QString>

class QLabel;
class QPushButton;
class QCheckBox;
class QTimer;

namespace chatterino {

class Split;
class TwitchChannel;

/**
 * Banner shown between the pinned-message banner and the chat view
 * while the watched channel is raiding out.
 */
class RaidBannerWidget final : public BaseWidget
{
    Q_OBJECT

public:
    explicit RaidBannerWidget(Split *split);

    // Pass nullptr to detach from any channel.
    void setChannel(TwitchChannel *channel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void scaleChangedEvent(float newScale) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void refreshVisibility();
    void refreshGoToButton();
    void tickCountdown();
    void loadAvatar(const QString &url);
    void openTargetInStreamlink();
    void goToTarget();
    QString targetLogin() const;

    Split *split_ = nullptr;
    TwitchChannel *channel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    QLabel *avatarLabel_ = nullptr;
    QLabel *titleLabel_ = nullptr;
    QLabel *infoLabel_ = nullptr;
    QLabel *countdownLabel_ = nullptr;
    QPushButton *closeButton_ = nullptr;
    QPushButton *streamlinkButton_ = nullptr;
    QPushButton *goToButton_ = nullptr;
    QCheckBox *autoCheck_ = nullptr;

    QTimer *countdownTimer_ = nullptr;
    QString avatarUrl_;
    QString lastRaidId_;
    bool userDismissed_ = false;
};

}  // namespace chatterino
