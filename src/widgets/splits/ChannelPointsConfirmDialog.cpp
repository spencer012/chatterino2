// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/ChannelPointsConfirmDialog.hpp"

#include "util/LayoutCreator.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace chatterino {

ChannelPointsConfirmDialog::ChannelPointsConfirmDialog(
    const QString &channelLogin, const ChannelPointRewardData &reward,
    QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
              BaseWindow::Dialog,
          },
          parent)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(
        QString("Redeem in #%1").arg(channelLogin.trimmed().toLower()));

    auto layout = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QVBoxLayout>();
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *titleLabel = layout.emplace<QLabel>(QString("<b>%1</b>").arg(reward.title))
                           .getElement();
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setWordWrap(true);

    layout.emplace<QLabel>(QString("Cost: %1").arg(reward.cost));

    auto prompt = reward.prompt.trimmed();
    if (prompt.isEmpty())
    {
        prompt = "No description provided.";
    }
    this->descriptionLabel_ =
        layout.emplace<QLabel>(prompt).assign(&this->descriptionLabel_).getElement();
    this->descriptionLabel_->setWordWrap(true);
    this->descriptionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *hintLabel = layout
                          .emplace<QLabel>(
                              "Hold Shift while confirming to keep this confirm dialog open.")
                          .getElement();
    hintLabel->setWordWrap(true);

    auto *buttonBox = layout.emplace<QDialogButtonBox>().getElement();
    this->confirmButton_ =
        buttonBox->addButton("Redeem", QDialogButtonBox::AcceptRole);
    this->cancelButton_ =
        buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);

    QObject::connect(this->confirmButton_, &QPushButton::clicked, this,
                     &ChannelPointsConfirmDialog::confirm);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this,
                     &QWidget::close);

    this->resize(360, 180);
}

void ChannelPointsConfirmDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        this->confirm();
        return;
    }

    BasePopup::keyPressEvent(event);
}

void ChannelPointsConfirmDialog::confirm()
{
    const bool keepDialogOpen =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    Q_EMIT this->confirmed(keepDialogOpen);
    if (!keepDialogOpen)
    {
        this->close();
    }
}

}  // namespace chatterino
