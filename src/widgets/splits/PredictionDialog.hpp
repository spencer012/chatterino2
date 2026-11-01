// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BasePopup.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <cstdint>
#include <memory>
#include <optional>

class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace chatterino {

class TwitchChannel;

/// Asks how many points to put on a prediction outcome, shows the possible
/// payout and what's left of the balance, then makes the prediction.
///
/// Follows the prediction while open: if it locks or ends, predicting is
/// disabled.
class PredictionDialog : public BasePopup
{
    Q_OBJECT

public:
    PredictionDialog(std::weak_ptr<TwitchChannel> channel, QString eventId,
                     QString outcomeId, QWidget *parent = nullptr);
    ~PredictionDialog() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void updateUi();
    void confirm();
    void setAmount(std::int64_t amount);
    /// Most points we may add right now, given the balance (if known) and
    /// Twitch's per-prediction limit.
    [[nodiscard]] std::int64_t maxAmount() const;

    std::weak_ptr<TwitchChannel> channel_;
    QString channelLogin_;
    QString eventId_;
    QString outcomeId_;
    std::optional<int> balance_;
    bool pending_ = false;
    /// Why the last attempt failed.
    QString error_;

    pajlada::Signals::SignalHolder signalHolder_;

    QLabel *titleLabel_{};
    QLabel *outcomeLabel_{};
    QLabel *existingLabel_{};
    QSpinBox *amountBox_{};
    QLabel *payoutLabel_{};
    QLabel *balanceLabel_{};
    QLabel *statusLabel_{};
    QLabel *errorLabel_{};
    QPushButton *confirmButton_{};
    QPushButton *cancelButton_{};
    QTimer *timer_{};
};

}  // namespace chatterino
