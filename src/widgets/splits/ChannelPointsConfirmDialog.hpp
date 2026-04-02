// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsModels.hpp"
#include "widgets/BasePopup.hpp"

class QLabel;
class QPushButton;

namespace chatterino {

class ChannelPointsConfirmDialog : public BasePopup
{
    Q_OBJECT

public:
    ChannelPointsConfirmDialog(const QString &channelLogin,
                               const ChannelPointRewardData &reward,
                               QWidget *parent = nullptr);

Q_SIGNALS:
    void confirmed(bool keepPopupOpen);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void confirm();

    QLabel *descriptionLabel_{};
    QPushButton *confirmButton_{};
    QPushButton *cancelButton_{};
};

}  // namespace chatterino
