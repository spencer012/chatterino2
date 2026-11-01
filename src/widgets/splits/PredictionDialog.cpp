// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PredictionDialog.hpp"

#include "Application.hpp"
#include "controllers/channelpoints/ChannelPointsController.hpp"
#include "providers/twitch/ChannelEvents.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/splits/ChannelPointsFormat.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace std::chrono_literals;

namespace chatterino {

namespace {

const QColor BLUE_OUTCOME_COLOR("#387aff");
const QColor PINK_OUTCOME_COLOR("#f5009b");
const QColor ERROR_COLOR("#e5675f");

QString colorCss(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString fullNumber(std::int64_t value)
{
    return QLocale().toString(qint64(value));
}

}  // namespace

PredictionDialog::PredictionDialog(std::weak_ptr<TwitchChannel> channel,
                                   QString eventId, QString outcomeId,
                                   QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
              BaseWindow::Dialog,
          },
          parent)
    , channel_(std::move(channel))
    , eventId_(std::move(eventId))
    , outcomeId_(std::move(outcomeId))
    , timer_(new QTimer(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);

    if (auto locked = this->channel_.lock())
    {
        this->channelLogin_ = locked->getName();
        this->setWindowTitle(
            QStringLiteral("Predict in %1").arg(locked->getDisplayName()));
        this->signalHolder_.managedConnect(locked->channelEventsChanged,
                                           [this] {
                                               this->updateUi();
                                           });
    }

    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(6);

    auto dim = this->palette().color(QPalette::WindowText);
    dim.setAlpha(160);
    const auto dimStyle = QStringLiteral("color: %1").arg(colorCss(dim));

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setTextFormat(Qt::PlainText);
    this->titleLabel_->setWordWrap(true);
    auto titleFont = this->titleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    this->titleLabel_->setFont(titleFont);
    layout->addWidget(this->titleLabel_);

    this->outcomeLabel_ = new QLabel(this);
    this->outcomeLabel_->setTextFormat(Qt::RichText);
    this->outcomeLabel_->setWordWrap(true);
    layout->addWidget(this->outcomeLabel_);

    this->existingLabel_ = new QLabel(this);
    this->existingLabel_->setWordWrap(true);
    this->existingLabel_->setStyleSheet(dimStyle);
    layout->addWidget(this->existingLabel_);

    layout->addSpacing(8);
    // The amount gets a row of its own; next to the buttons it got squeezed
    // under them in narrow windows.
    this->amountBox_ = new QSpinBox(this);
    this->amountBox_->setRange(1, channelevents::MAX_PREDICTION_POINTS);
    this->amountBox_->setGroupSeparatorShown(true);
    this->amountBox_->setSuffix(QStringLiteral(" points"));
    this->amountBox_->setValue(100);
    layout->addWidget(this->amountBox_);

    auto *amountRow = new QHBoxLayout;
    amountRow->setSpacing(4);
    for (const auto amount : {10, 100, 1000, 10000})
    {
        auto *button = new QPushButton(
            amount >= 1000 ? QStringLiteral("%1K").arg(amount / 1000)
                           : QString::number(amount),
            this);
        button->setToolTip(QStringLiteral("%1 points").arg(fullNumber(amount)));
        button->setAutoDefault(false);
        QObject::connect(button, &QPushButton::clicked, this, [this, amount] {
            this->setAmount(amount);
        });
        amountRow->addWidget(button, 1);
    }
    auto *maxButton = new QPushButton(QStringLiteral("Max"), this);
    maxButton->setAutoDefault(false);
    QObject::connect(maxButton, &QPushButton::clicked, this, [this] {
        this->setAmount(this->maxAmount());
    });
    amountRow->addWidget(maxButton, 1);
    layout->addLayout(amountRow);

    this->payoutLabel_ = new QLabel(this);
    this->payoutLabel_->setTextFormat(Qt::RichText);
    layout->addWidget(this->payoutLabel_);

    this->balanceLabel_ = new QLabel(this);
    this->balanceLabel_->setTextFormat(Qt::RichText);
    layout->addWidget(this->balanceLabel_);

    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setStyleSheet(dimStyle);
    layout->addWidget(this->statusLabel_);

    this->errorLabel_ = new QLabel(this);
    this->errorLabel_->setWordWrap(true);
    this->errorLabel_->setStyleSheet(
        QStringLiteral("color: %1").arg(colorCss(ERROR_COLOR)));
    this->errorLabel_->hide();
    layout->addWidget(this->errorLabel_);

    layout->addSpacing(8);
    auto *buttonBox = new QDialogButtonBox(this);
    this->confirmButton_ =
        buttonBox->addButton("Predict", QDialogButtonBox::AcceptRole);
    this->confirmButton_->setDefault(true);
    this->cancelButton_ =
        buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);
    layout->addWidget(buttonBox);

    QObject::connect(this->amountBox_, &QSpinBox::valueChanged, this,
                     [this] {
                         this->error_.clear();
                         this->updateUi();
                     });
    QObject::connect(this->confirmButton_, &QPushButton::clicked, this,
                     &PredictionDialog::confirm);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this,
                     &QWidget::close);

    // The balance comes from the channel points controller. Watching the
    // channel loads it and keeps it up to date.
    if (auto *channelPoints = getApp()->getChannelPoints();
        channelPoints != nullptr && !this->channelLogin_.isEmpty())
    {
        channelPoints->watchChannel(this->channelLogin_);
        this->balance_ = channelPoints->getViewState(this->channelLogin_).balance;
        QObject::connect(
            channelPoints, &ChannelPointsController::channelStateChanged, this,
            [this](const QString &login) {
                if (!login.isEmpty() && login != this->channelLogin_)
                {
                    return;
                }
                this->balance_ = getApp()
                                     ->getChannelPoints()
                                     ->getViewState(this->channelLogin_)
                                     .balance;
                this->updateUi();
            });
    }

    this->timer_->setInterval(1s);
    QObject::connect(this->timer_, &QTimer::timeout, this, [this] {
        this->updateUi();
    });
    this->timer_->start();

    this->updateUi();
    this->setMinimumWidth(400);
    this->resize(420, this->sizeHint().height());
    this->amountBox_->setFocus();
    this->amountBox_->selectAll();
}

PredictionDialog::~PredictionDialog()
{
    if (this->channelLogin_.isEmpty())
    {
        return;
    }
    if (auto *channelPoints = getApp()->getChannelPoints())
    {
        channelPoints->unwatchChannel(this->channelLogin_);
    }
}

std::int64_t PredictionDialog::maxAmount() const
{
    std::int64_t already = 0;
    if (auto channel = this->channel_.lock())
    {
        if (const auto own =
                channel->channelEvents().ownPrediction(this->eventId_))
        {
            already = own->points;
        }
    }

    auto max = std::int64_t(channelevents::MAX_PREDICTION_POINTS) - already;
    if (this->balance_)
    {
        max = std::min<std::int64_t>(max, *this->balance_);
    }
    return std::max<std::int64_t>(max, 0);
}

void PredictionDialog::setAmount(std::int64_t amount)
{
    const auto max = this->maxAmount();
    this->amountBox_->setValue(
        int(std::clamp<std::int64_t>(amount, 1, std::max<std::int64_t>(max, 1))));
}

void PredictionDialog::updateUi()
{
    auto channel = this->channel_.lock();
    const auto *prediction =
        channel != nullptr && channel->channelEvents().prediction() &&
                channel->channelEvents().prediction()->id == this->eventId_
            ? &*channel->channelEvents().prediction()
            : nullptr;
    const auto *outcome = prediction != nullptr
                              ? prediction->findOutcome(this->outcomeId_)
                              : nullptr;

    if (prediction == nullptr || outcome == nullptr)
    {
        this->titleLabel_->setText(QStringLiteral("Prediction ended"));
        this->outcomeLabel_->hide();
        this->existingLabel_->hide();
        this->payoutLabel_->hide();
        this->statusLabel_->setText(
            QStringLiteral("This prediction is no longer running."));
        this->confirmButton_->setEnabled(false);
        this->timer_->stop();
        return;
    }

    const auto own = channel->channelEvents().ownPrediction(this->eventId_);
    const auto amount = std::int64_t(this->amountBox_->value());
    const auto color = outcome->color == "PINK" ? PINK_OUTCOME_COLOR
                                                : BLUE_OUTCOME_COLOR;

    this->titleLabel_->setText(prediction->title);

    const auto ratio = prediction->payoutRatio(*outcome);
    this->outcomeLabel_->setText(
        QStringLiteral("<span style=\"color:%1\">●</span> <b>%2</b> "
                       "<span style=\"color:%3\">· %4% · %5 points "
                       "· %6</span>")
            .arg(color.name(), outcome->title.toHtmlEscaped(),
                 colorCss([&] {
                     auto c = this->palette().color(QPalette::WindowText);
                     c.setAlpha(160);
                     return c;
                 }()))
            .arg(prediction->percent(*outcome))
            .arg(fullNumber(outcome->points),
                 ratio > 0 ? QStringLiteral("1:%1").arg(ratio, 0, 'f', 2)
                           : QStringLiteral("no predictions yet")));
    this->outcomeLabel_->show();

    if (own)
    {
        this->existingLabel_->setText(
            QStringLiteral("You already predicted %1 points on this. These "
                           "are added on top.")
                .arg(fullNumber(own->points)));
        this->existingLabel_->show();
    }
    else
    {
        this->existingLabel_->hide();
    }

    // If this outcome wins, the winners split all points by their share of
    // the winning side. Includes the points you put in.
    const auto stake = amount + (own ? own->points : 0);
    const auto newTotal = prediction->totalPoints() + amount;
    const auto newOutcome = outcome->points + amount;
    const auto payout =
        newOutcome > 0 ? std::int64_t(double(stake) * double(newTotal) /
                                      double(newOutcome))
                       : stake;
    this->payoutLabel_->setText(
        QStringLiteral("If it wins: about <b>%1</b> points back")
            .arg(fullNumber(payout)));
    this->payoutLabel_->show();

    const auto max = this->maxAmount();
    // Twitch's LOCKED update can lag a little behind the timer.
    const bool windowOver =
        prediction->locksAt.isValid() &&
        prediction->locksAt <= QDateTime::currentDateTimeUtc();
    bool canConfirm = !this->pending_ && prediction->isOpen() && !windowOver &&
                      max > 0 && amount <= max;

    if (this->balance_)
    {
        const auto after = std::int64_t(*this->balance_) - amount;
        if (after >= 0)
        {
            auto dim = this->palette().color(QPalette::WindowText);
            dim.setAlpha(160);
            this->balanceLabel_->setText(
                QStringLiteral("<span style=\"color:%1\">Balance</span> "
                               "%2 → <b>%3</b>")
                    .arg(colorCss(dim), fullNumber(*this->balance_),
                         fullNumber(after)));
        }
        else
        {
            this->balanceLabel_->setText(
                QStringLiteral("<span style=\"color:%1\">You have %2 points, "
                               "%3 short.</span>")
                    .arg(colorCss(ERROR_COLOR), fullNumber(*this->balance_),
                         fullNumber(-after)));
        }
    }
    else
    {
        this->balanceLabel_->setText(QStringLiteral("Loading balance..."));
    }

    auto errorText = this->error_;
    if (errorText.isEmpty() && own && amount > max &&
        max < channelevents::MAX_PREDICTION_POINTS)
    {
        errorText = QStringLiteral("At most %1 more points on this prediction.")
                        .arg(fullNumber(max));
    }
    this->errorLabel_->setText(errorText);
    this->errorLabel_->setVisible(!errorText.isEmpty());

    if (this->pending_)
    {
        this->statusLabel_->setText(QStringLiteral("Predicting..."));
    }
    else if (prediction->isOpen() && windowOver)
    {
        this->statusLabel_->setText(QStringLiteral("Predictions are closing..."));
    }
    else if (prediction->isOpen() && prediction->locksAt.isValid())
    {
        this->statusLabel_->setText(
            QStringLiteral("Predictions close in %1")
                .arg(channelpoints::format::timeUntil(prediction->locksAt)));
    }
    else if (prediction->isOpen())
    {
        this->statusLabel_->setText(QString());
    }
    else
    {
        this->statusLabel_->setText(
            QStringLiteral("Predictions are closed (%1).")
                .arg(channelevents::describePredictionStatus(prediction->status)
                         .toLower()));
    }

    this->confirmButton_->setText(
        QStringLiteral("Predict %1 points").arg(fullNumber(amount)));
    this->confirmButton_->setEnabled(canConfirm);
}

void PredictionDialog::confirm()
{
    this->updateUi();
    if (!this->confirmButton_->isEnabled())
    {
        return;
    }

    auto *channelPoints = getApp()->getChannelPoints();
    if (channelPoints == nullptr)
    {
        return;
    }

    const auto amount = std::int64_t(this->amountBox_->value());
    this->pending_ = true;
    this->error_.clear();
    this->updateUi();

    QPointer<PredictionDialog> self(this);
    auto weakChannel = this->channel_;
    const auto eventId = this->eventId_;
    const auto outcomeId = this->outcomeId_;
    channelPoints->makePrediction(
        this->channelLogin_, eventId, outcomeId, amount,
        [self, weakChannel, eventId, outcomeId, amount](bool ok,
                                                        const QString &message) {
            if (ok)
            {
                if (auto channel = weakChannel.lock())
                {
                    channel->recordPrediction(eventId, outcomeId, amount);
                }
            }
            if (self.isNull())
            {
                return;
            }
            self->pending_ = false;
            if (ok)
            {
                self->close();
                return;
            }
            self->error_ = message;
            self->updateUi();
        });
}

void PredictionDialog::keyPressEvent(QKeyEvent *event)
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

}  // namespace chatterino
