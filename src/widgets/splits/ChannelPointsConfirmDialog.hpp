// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsModels.hpp"
#include "widgets/BasePopup.hpp"

class QLabel;
class QPushButton;
class QTextEdit;

namespace chatterino {

class ChannelPointsConfirmDialog : public BasePopup
{
    Q_OBJECT

public:
    ChannelPointsConfirmDialog(const QString &channelLogin,
                               const ChannelPointRewardData &reward,
                               ChannelPointQueueMode action,
                               QWidget *parent = nullptr);

Q_SIGNALS:
    void confirmed(ChannelPointQueueMode action, bool keepDialogOpen,
                   const QString &inputText);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void confirm();
    void updateConfirmEnabled();
    void updateInputHighlights();

    ChannelPointQueueMode action_{ChannelPointQueueMode::None};
    bool requiresInput_{false};
    QLabel *descriptionLabel_{};
    QLabel *inputLengthLabel_{};
    QTextEdit *inputEdit_{};
    QPushButton *confirmButton_{};
    QPushButton *cancelButton_{};
};

}  // namespace chatterino
