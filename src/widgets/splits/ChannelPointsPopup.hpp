// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsModels.hpp"
#include "widgets/BasePopup.hpp"

#include <QPointer>
#include <QTimer>

class QLabel;
class QScrollArea;
class QVBoxLayout;
class QPushButton;

namespace chatterino {

class Split;
class ChannelPointsConfirmDialog;

class ChannelPointsPopup : public BasePopup
{
    Q_OBJECT

public:
    ChannelPointsPopup(Split *split, QWidget *parent = nullptr);
    ~ChannelPointsPopup() override;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void refresh();
    void rebuildRewardSection(const QString &title,
                              const QVector<ChannelPointRewardView> &rewards,
                              QVBoxLayout *layout);
    void openConfirmDialog(const ChannelPointRewardData &reward);
    void toggleFavorite(const QString &rewardId);
    void attachToController();
    void detachFromController();

    Split *split_{};
    QString channelLogin_;
    bool attached_ = false;

    QLabel *statusLabel_{};
    QLabel *balanceLabel_{};
    QLabel *connectionLabel_{};
    QPushButton *clearQueueButton_{};
    QScrollArea *scrollArea_{};
    QWidget *content_{};
    QVBoxLayout *contentLayout_{};
    QPointer<ChannelPointsConfirmDialog> confirmDialog_;
    QTimer refreshTimer_;
};

}  // namespace chatterino
