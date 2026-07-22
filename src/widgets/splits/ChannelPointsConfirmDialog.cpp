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
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

constexpr int REDEEM_INPUT_SOFT_LIMIT = 500;

}  // namespace

namespace chatterino {

ChannelPointsConfirmDialog::ChannelPointsConfirmDialog(
    const QString &channelLogin, const ChannelPointRewardData &reward,
    ChannelPointQueueMode action, QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
              BaseWindow::Dialog,
          },
          parent)
    , action_(action)
    , requiresInput_(reward.isUserInputRequired)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    const auto channel = channelLogin.trimmed().toLower();
    const auto actionText = [action] {
        switch (action)
        {
            case ChannelPointQueueMode::Once:
                return QString("Queue Once");
            case ChannelPointQueueMode::Repeat:
                return QString("Start Loop");
            case ChannelPointQueueMode::None:
            default:
                return QString("Redeem");
        }
    }();
    this->setWindowTitle(QString("%1 in #%2").arg(actionText, channel));

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

    if (this->requiresInput_)
    {
        auto *inputLabel = layout.emplace<QLabel>("Text input:").getElement();
        inputLabel->setWordWrap(true);

        this->inputEdit_ =
            layout.emplace<QTextEdit>().assign(&this->inputEdit_).getElement();
        this->inputEdit_->setPlaceholderText("Enter redeem text");
        this->inputEdit_->setAcceptRichText(false);
        this->inputEdit_->setMinimumHeight(120);
        this->inputEdit_->setTabChangesFocus(true);
        QObject::connect(this->inputEdit_, &QTextEdit::textChanged, this,
                         [this] {
                             this->updateConfirmEnabled();
                             this->updateInputHighlights();
                         });

        this->inputLengthLabel_ =
            layout.emplace<QLabel>().assign(&this->inputLengthLabel_).getElement();
        this->inputLengthLabel_->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
    }

    auto *hintLabel = layout
                          .emplace<QLabel>(
                              "Hold Shift while confirming to keep this confirm dialog open.")
                          .getElement();
    hintLabel->setWordWrap(true);

    auto *buttonBox = layout.emplace<QDialogButtonBox>().getElement();
    this->confirmButton_ = buttonBox->addButton(actionText, QDialogButtonBox::AcceptRole);
    this->cancelButton_ =
        buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);

    QObject::connect(this->confirmButton_, &QPushButton::clicked, this,
                     &ChannelPointsConfirmDialog::confirm);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this,
                     &QWidget::close);

    this->updateConfirmEnabled();
    this->updateInputHighlights();
    this->resize(this->requiresInput_ ? QSize(460, 320) : QSize(360, 180));
}

void ChannelPointsConfirmDialog::keyPressEvent(QKeyEvent *event)
{
    if (this->requiresInput_ && this->inputEdit_ != nullptr)
    {
        BasePopup::keyPressEvent(event);
        return;
    }

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
    const auto inputText =
        this->inputEdit_ == nullptr ? QString() : this->inputEdit_->toPlainText();
    Q_EMIT this->confirmed(this->action_, keepDialogOpen, inputText);
    if (!keepDialogOpen)
    {
        this->close();
    }
}

void ChannelPointsConfirmDialog::updateConfirmEnabled()
{
    if (this->confirmButton_ == nullptr)
    {
        return;
    }

    if (!this->requiresInput_ || this->inputEdit_ == nullptr)
    {
        this->confirmButton_->setEnabled(true);
        return;
    }

    this->confirmButton_->setEnabled(
        !this->inputEdit_->toPlainText().trimmed().isEmpty());
}

void ChannelPointsConfirmDialog::updateInputHighlights()
{
    if (!this->requiresInput_ || this->inputEdit_ == nullptr)
    {
        return;
    }

    const auto text = this->inputEdit_->toPlainText();

    QList<QTextEdit::ExtraSelection> selections;
    if (text.size() > REDEEM_INPUT_SOFT_LIMIT)
    {
        QTextEdit::ExtraSelection overLimit;
        QTextCursor cursor(this->inputEdit_->document());
        QTextCharFormat format;
        format.setBackground(QColor("#fff59d"));
        format.setForeground(Qt::black);
        cursor.setPosition(REDEEM_INPUT_SOFT_LIMIT, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        overLimit.cursor = cursor;
        overLimit.format = format;
        selections.append(overLimit);
    }
    this->inputEdit_->setExtraSelections(selections);

    if (this->inputLengthLabel_ == nullptr)
    {
        return;
    }

    if (text.isEmpty())
    {
        this->inputLengthLabel_->clear();
        return;
    }

    if (text.size() > REDEEM_INPUT_SOFT_LIMIT)
    {
        this->inputLengthLabel_->setStyleSheet("color: #9a7d00");
        this->inputLengthLabel_->setText(
            QString("%1 chars (%2 over 500)")
                .arg(text.size())
                .arg(text.size() - REDEEM_INPUT_SOFT_LIMIT));
        return;
    }

    this->inputLengthLabel_->setStyleSheet("");
    this->inputLengthLabel_->setText(QString("%1 chars").arg(text.size()));
}

}  // namespace chatterino
