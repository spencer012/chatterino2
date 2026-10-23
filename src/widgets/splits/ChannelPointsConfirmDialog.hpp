// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsTypes.hpp"
#include "widgets/BasePopup.hpp"

#include <optional>

class QLabel;
class QPushButton;
class QRadioButton;
class QTextEdit;

namespace chatterino {

/// Asks before every redeem: what it costs, what's left afterwards, and
/// whether to redeem now or queue it.
///
/// Stays in sync with the reward while open (see updateReward()), so the cost
/// that gets confirmed is always the one on screen.
class ChannelPointsConfirmDialog : public BasePopup
{
    Q_OBJECT

public:
    /// @param mode Preselected choice. `None` = redeem now.
    ChannelPointsConfirmDialog(const QString &channelName,
                               const ChannelPointRewardView &view,
                               std::optional<int> balance,
                               ChannelPointQueueMode mode,
                               QWidget *parent = nullptr);

    [[nodiscard]] QString rewardId() const;
    void updateReward(const ChannelPointRewardView &view,
                      std::optional<int> balance);

Q_SIGNALS:
    /// @param mode `None` = redeem now.
    /// @param cost The cost shown to the user when they confirmed.
    void confirmed(const QString &rewardId, ChannelPointQueueMode mode,
                   const QString &inputText, int cost);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    [[nodiscard]] ChannelPointQueueMode selectedMode() const;
    void confirm();
    void updateUi();
    void updateInputInfo();

    ChannelPointRewardView view_;
    std::optional<int> balance_;

    QLabel *titleLabel_{};
    QLabel *costLabel_{};
    QLabel *promptLabel_{};
    QTextEdit *inputEdit_{};
    QLabel *inputInfoLabel_{};
    QLabel *balanceLabel_{};
    QRadioButton *nowRadio_{};
    QRadioButton *onceRadio_{};
    QRadioButton *repeatRadio_{};
    QLabel *modeHint_{};
    QPushButton *confirmButton_{};
    QPushButton *cancelButton_{};
};

}  // namespace chatterino
