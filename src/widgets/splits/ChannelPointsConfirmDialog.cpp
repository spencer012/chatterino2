// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/ChannelPointsConfirmDialog.hpp"

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "widgets/splits/ChannelPointsFormat.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QRadioButton>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

using chatterino::channelpoints::flattenLineBreaks;
using chatterino::channelpoints::MAX_REDEEM_INPUT_LENGTH;
using chatterino::channelpoints::normalizeRedeemInput;

const QColor WARNING_COLOR("#e0b84f");
const QColor ERROR_COLOR("#e5675f");

/// Text box for the reward message. Twitch posts the message to chat, which
/// is a single line, so line breaks in pasted text become spaces. (Enter
/// itself confirms the dialog.)
class RedeemInputEdit final : public QTextEdit
{
public:
    using QTextEdit::QTextEdit;

protected:
    void insertFromMimeData(const QMimeData *source) override
    {
        if (source->hasText())
        {
            this->insertPlainText(flattenLineBreaks(source->text()));
        }
    }
};

/// Highlights text past Twitch's limit instead of cutting it off.
class RedeemInputHighlighter final : public QSyntaxHighlighter
{
public:
    explicit RedeemInputHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        this->overLimitFormat_.setForeground(QColor("#fff59d"));
        this->overLimitFormat_.setBackground(QColor(160, 120, 0, 70));
    }

protected:
    void highlightBlock(const QString &text) override
    {
        const auto blockStart = this->currentBlock().position();
        if (blockStart >= MAX_REDEEM_INPUT_LENGTH)
        {
            this->setFormat(0, int(text.size()), this->overLimitFormat_);
            return;
        }

        const auto overLimitStart = MAX_REDEEM_INPUT_LENGTH - blockStart;
        if (overLimitStart < text.size())
        {
            this->setFormat(overLimitStart, int(text.size()) - overLimitStart,
                            this->overLimitFormat_);
        }
    }

private:
    QTextCharFormat overLimitFormat_;
};

QString colorCss(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

}  // namespace

namespace chatterino {

namespace fmt = channelpoints::format;

ChannelPointsConfirmDialog::ChannelPointsConfirmDialog(
    const QString &channelName, const ChannelPointRewardView &view,
    std::optional<int> balance, ChannelPointQueueMode mode, QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
              BaseWindow::Dialog,
          },
          parent)
    , view_(view)
    , balance_(balance)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("Redeem in %1").arg(channelName));

    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(6);

    const auto text = this->palette().color(QPalette::WindowText);
    auto dim = text;
    dim.setAlpha(160);

    // What: title, where and how much.
    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setTextFormat(Qt::PlainText);
    this->titleLabel_->setWordWrap(true);
    auto titleFont = this->titleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    this->titleLabel_->setFont(titleFont);
    layout->addWidget(this->titleLabel_);

    this->costLabel_ = new QLabel(this);
    this->costLabel_->setStyleSheet(QStringLiteral("color: %1").arg(colorCss(dim)));
    layout->addWidget(this->costLabel_);

    this->promptLabel_ = new QLabel(this);
    this->promptLabel_->setTextFormat(Qt::PlainText);
    this->promptLabel_->setWordWrap(true);
    this->promptLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->promptLabel_->setContentsMargins(0, 6, 0, 0);
    layout->addWidget(this->promptLabel_);

    if (view.reward.isUserInputRequired)
    {
        layout->addSpacing(6);
        this->inputEdit_ = new RedeemInputEdit(this);
        this->inputEdit_->setPlaceholderText("Your message");
        this->inputEdit_->setAcceptRichText(false);
        this->inputEdit_->setTabChangesFocus(true);
        this->inputEdit_->setFixedHeight(84);
        this->inputEdit_->installEventFilter(this);
        new RedeemInputHighlighter(this->inputEdit_->document());
        layout->addWidget(this->inputEdit_);

        this->inputInfoLabel_ = new QLabel(this);
        this->inputInfoLabel_->setAlignment(Qt::AlignRight);
        layout->addWidget(this->inputInfoLabel_);

        QObject::connect(this->inputEdit_, &QTextEdit::textChanged, this,
                         [this] {
                             this->updateInputInfo();
                             this->updateUi();
                         });
    }

    // Separator between "what" and "when".
    layout->addSpacing(6);
    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet(QStringLiteral("color: %1").arg(colorCss([&] {
        auto c = text;
        c.setAlpha(40);
        return c;
    }())));
    layout->addWidget(line);
    layout->addSpacing(4);

    this->nowRadio_ = new QRadioButton("Redeem now", this);
    this->onceRadio_ = new QRadioButton("Redeem once, as soon as available", this);
    this->repeatRadio_ = new QRadioButton("Redeem every time it's available", this);
    auto *group = new QButtonGroup(this);
    group->addButton(this->nowRadio_);
    group->addButton(this->onceRadio_);
    group->addButton(this->repeatRadio_);
    layout->addWidget(this->nowRadio_);
    layout->addWidget(this->onceRadio_);
    layout->addWidget(this->repeatRadio_);

    this->modeHint_ = new QLabel(this);
    this->modeHint_->setWordWrap(true);
    this->modeHint_->setContentsMargins(22, 0, 0, 0);
    this->modeHint_->setStyleSheet(QStringLiteral("color: %1").arg(colorCss(dim)));
    layout->addWidget(this->modeHint_);

    layout->addSpacing(4);
    this->balanceLabel_ = new QLabel(this);
    this->balanceLabel_->setTextFormat(Qt::RichText);
    layout->addWidget(this->balanceLabel_);

    layout->addSpacing(8);
    auto *buttonBox = new QDialogButtonBox(this);
    this->confirmButton_ =
        buttonBox->addButton("Redeem", QDialogButtonBox::AcceptRole);
    this->confirmButton_->setDefault(true);
    this->cancelButton_ =
        buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);
    layout->addWidget(buttonBox);

    switch (mode)
    {
        case ChannelPointQueueMode::Once:
            this->onceRadio_->setChecked(true);
            break;
        case ChannelPointQueueMode::Repeat:
            this->repeatRadio_->setChecked(true);
            break;
        case ChannelPointQueueMode::None:
            this->nowRadio_->setChecked(true);
            break;
    }

    QObject::connect(group, &QButtonGroup::buttonToggled, this,
                     [this](QAbstractButton *, bool checked) {
                         if (checked)
                         {
                             this->updateUi();
                         }
                     });
    QObject::connect(this->confirmButton_, &QPushButton::clicked, this,
                     &ChannelPointsConfirmDialog::confirm);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this,
                     &QWidget::close);

    this->updateInputInfo();
    this->updateUi();
    this->setMinimumWidth(380);
    this->resize(400, this->sizeHint().height());

    if (this->inputEdit_ != nullptr)
    {
        this->inputEdit_->setFocus();
    }
    else
    {
        this->confirmButton_->setFocus();
    }
}

QString ChannelPointsConfirmDialog::rewardId() const
{
    return this->view_.reward.id;
}

void ChannelPointsConfirmDialog::updateReward(const ChannelPointRewardView &view,
                                              std::optional<int> balance)
{
    this->view_ = view;
    this->balance_ = balance;
    this->updateUi();
}

ChannelPointQueueMode ChannelPointsConfirmDialog::selectedMode() const
{
    if (this->onceRadio_->isChecked())
    {
        return ChannelPointQueueMode::Once;
    }
    if (this->repeatRadio_->isChecked())
    {
        return ChannelPointQueueMode::Repeat;
    }
    return ChannelPointQueueMode::None;
}

void ChannelPointsConfirmDialog::updateUi()
{
    const auto &reward = this->view_.reward;
    const bool ready = this->view_.block == RewardBlock::None;

    this->titleLabel_->setText(reward.title);
    this->costLabel_->setText(
        QStringLiteral("%1 points").arg(fmt::points(reward.cost)));

    const auto prompt = reward.prompt.trimmed();
    this->promptLabel_->setText(prompt);
    this->promptLabel_->setVisible(!prompt.isEmpty());

    // "Now" only makes sense when the reward is available.
    this->nowRadio_->setEnabled(ready && !this->view_.inFlight);
    this->nowRadio_->setText(
        ready ? QStringLiteral("Redeem now")
              : QStringLiteral("Redeem now (%1)")
                    .arg(fmt::blockDetail(this->view_, this->balance_)));
    if (!this->nowRadio_->isEnabled() && this->nowRadio_->isChecked())
    {
        this->onceRadio_->setChecked(true);
    }

    const auto mode = this->selectedMode();
    switch (mode)
    {
        case ChannelPointQueueMode::None:
            this->confirmButton_->setText(
                QStringLiteral("Redeem for %1").arg(fmt::points(reward.cost)));
            this->modeHint_->hide();
            break;

        case ChannelPointQueueMode::Once:
            this->confirmButton_->setText("Queue");
            this->modeHint_->setText(
                ready ? QStringLiteral("Available now, so this redeems right "
                                       "away.")
                      : QStringLiteral("Waits until it's available (%1), then "
                                       "redeems once. Keeps waiting while "
                                       "Chatterino is open.")
                            .arg(fmt::blockDetail(this->view_, this->balance_)));
            this->modeHint_->show();
            break;

        case ChannelPointQueueMode::Repeat:
            this->confirmButton_->setText("Start repeating");
            this->modeHint_->setText(
                "Redeems whenever it's available until you cancel it or run "
                "out of points.");
            this->modeHint_->show();
            break;
    }

    // Balance after, so the cost is never a surprise.
    const auto text = this->palette().color(QPalette::WindowText);
    if (this->balance_)
    {
        const auto after = *this->balance_ - reward.cost;
        if (after >= 0)
        {
            auto dim = text;
            dim.setAlpha(160);
            this->balanceLabel_->setText(
                QStringLiteral("<span style=\"color:%1\">Balance</span> "
                               "%2 → <b>%3</b>")
                    .arg(colorCss(dim), fmt::points(*this->balance_),
                         fmt::points(after)));
        }
        else
        {
            this->balanceLabel_->setText(
                QStringLiteral("<span style=\"color:%1\">You have %2 points, "
                               "%3 short.</span>")
                    .arg(colorCss(ERROR_COLOR), fmt::points(*this->balance_),
                         fmt::points(-after)));
        }
        this->balanceLabel_->show();
    }
    else
    {
        this->balanceLabel_->hide();
    }

    bool canConfirm = !this->view_.inFlight;
    if (this->inputEdit_ != nullptr)
    {
        const auto size =
            normalizeRedeemInput(this->inputEdit_->toPlainText()).size();
        if (size == 0)
        {
            canConfirm = false;
        }
    }
    if (mode == ChannelPointQueueMode::None && !ready)
    {
        canConfirm = false;
    }
    this->confirmButton_->setEnabled(canConfirm);
    this->confirmButton_->setToolTip(
        canConfirm ? QStringLiteral("Hold Shift to keep this dialog open")
                   : QString());
}

void ChannelPointsConfirmDialog::updateInputInfo()
{
    if (this->inputEdit_ == nullptr || this->inputInfoLabel_ == nullptr)
    {
        return;
    }

    const auto size =
        normalizeRedeemInput(this->inputEdit_->toPlainText()).size();
    if (size > MAX_REDEEM_INPUT_LENGTH)
    {
        this->inputInfoLabel_->setStyleSheet(
            QStringLiteral("color: %1").arg(colorCss(WARNING_COLOR)));
        this->inputInfoLabel_->setText(
            QStringLiteral("%1 / %2, Twitch may cut the rest")
                .arg(size)
                .arg(MAX_REDEEM_INPUT_LENGTH));
        return;
    }

    auto dim = this->palette().color(QPalette::WindowText);
    dim.setAlpha(140);
    this->inputInfoLabel_->setStyleSheet(
        QStringLiteral("color: %1").arg(colorCss(dim)));
    this->inputInfoLabel_->setText(
        QStringLiteral("%1 / %2").arg(size).arg(MAX_REDEEM_INPUT_LENGTH));
}

void ChannelPointsConfirmDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (this->focusWidget() == this->cancelButton_)
        {
            this->close();
        }
        else
        {
            this->confirm();
        }
        return;
    }

    BasePopup::keyPressEvent(event);
}

bool ChannelPointsConfirmDialog::eventFilter(QObject *watched, QEvent *event)
{
    // Enter in the text box confirms; reward input is a single message.
    if (watched == this->inputEdit_ && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter)
        {
            this->confirm();
            return true;
        }
    }
    return BasePopup::eventFilter(watched, event);
}

void ChannelPointsConfirmDialog::confirm()
{
    if (!this->confirmButton_->isEnabled())
    {
        return;
    }

    // Shift keeps the dialog open to fire several in a row.
    const bool keepOpen =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    const auto inputText = this->inputEdit_ == nullptr
                               ? QString()
                               : normalizeRedeemInput(
                                     this->inputEdit_->toPlainText());

    Q_EMIT this->confirmed(this->view_.reward.id, this->selectedMode(),
                           inputText, this->view_.reward.cost);

    if (!keepOpen)
    {
        this->close();
    }
}

}  // namespace chatterino
