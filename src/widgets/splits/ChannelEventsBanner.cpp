// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/ChannelEventsBanner.hpp"

#include "Application.hpp"
#include "controllers/channelpoints/ChannelPointsController.hpp"
#include "providers/twitch/ChannelEvents.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/splits/ChannelPointsFormat.hpp"
#include "widgets/splits/PredictionDialog.hpp"
#include "widgets/splits/Split.hpp"

#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace chatterino::channeleventsbanner {

namespace {

constexpr auto MUTED_STYLE = "color: #adadb8;";

const QColor HYPE_TRAIN_COLOR("#9147ff");
const QColor GOLDEN_KAPPA_COLOR("#e5b53b");
const QColor POLL_COLOR("#9147ff");
const QColor BLUE_OUTCOME_COLOR("#387aff");
const QColor PINK_OUTCOME_COLOR("#f5009b");
const QColor SUCCESS_COLOR("#5fbf77");
const QColor ERROR_COLOR("#e5675f");

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QColor outcomeColor(const QString &color)
{
    return color == "PINK" ? PINK_OUTCOME_COLOR : BLUE_OUTCOME_COLOR;
}

QString countdown(const QDateTime &until)
{
    return QStringLiteral("⏱ ") +
           channelpoints::format::duration(
               QDateTime::currentDateTimeUtc().secsTo(until));
}

QString fullNumber(std::int64_t value)
{
    return QLocale().toString(qint64(value));
}

void setFontSize(QWidget *widget, float pointSize, bool bold = false)
{
    auto font = widget->font();
    font.setPointSizeF(pointSize);
    font.setBold(bold);
    widget->setFont(font);
}

}  // namespace

/// Label that shortens its text with "..." instead of growing.
class ElidedLabel final : public QLabel
{
public:
    explicit ElidedLabel(QWidget *parent)
        : QLabel(parent)
    {
        this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setFullText(const QString &text)
    {
        if (text == this->fullText_)
        {
            return;
        }
        this->fullText_ = text;
        this->setToolTip(text);
        this->update();
        this->updateGeometry();
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        return {this->fontMetrics().horizontalAdvance(this->fullText_) + 2,
                this->fontMetrics().height()};
    }

    [[nodiscard]] QSize minimumSizeHint() const override
    {
        return {0, this->fontMetrics().height()};
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setFont(this->font());
        painter.setPen(this->palette().color(QPalette::WindowText));
        painter.drawText(this->rect(), Qt::AlignVCenter | Qt::AlignLeft,
                         this->fontMetrics().elidedText(
                             this->fullText_, Qt::ElideRight, this->width()));
    }

private:
    QString fullText_;
};

/// Thin bar made of colored segments, e.g. hype train progress or the split
/// of points between prediction outcomes.
class ProgressStrip final : public QWidget
{
public:
    struct Segment {
        double fraction = 0;
        QColor color;
    };

    explicit ProgressStrip(QWidget *parent)
        : QWidget(parent)
    {
        this->setFixedHeight(4);
    }

    void setSegments(std::vector<Segment> segments)
    {
        this->segments_ = std::move(segments);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);

        const QRectF rect = this->rect();
        const auto radius = rect.height() / 2;
        QPainterPath track;
        track.addRoundedRect(rect, radius, radius);
        painter.fillPath(
            track, withAlpha(this->palette().color(QPalette::WindowText), 28));
        painter.setClipPath(track);

        auto x = rect.left();
        for (const auto &segment : this->segments_)
        {
            const auto width =
                rect.width() * std::clamp(segment.fraction, 0.0, 1.0);
            painter.fillRect(QRectF(x, rect.top(), width, rect.height()),
                             segment.color);
            x += width;
        }
    }

private:
    std::vector<Segment> segments_;
};

/// A poll choice or prediction outcome. The background fills up to the
/// choice's share of the votes / points.
class ShareRow final : public QWidget
{
public:
    explicit ShareRow(QWidget *parent)
        : QWidget(parent)
        , title(new ElidedLabel(this))
        , stats(new QLabel(this))
        , button(new QPushButton(this))
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 1, 2, 1);
        layout->setSpacing(8);
        layout->addWidget(this->title, 1);
        layout->addWidget(this->stats);
        layout->addWidget(this->button);

        this->stats->setStyleSheet(MUTED_STYLE);
        this->stats->setTextFormat(Qt::PlainText);
        this->button->setCursor(Qt::PointingHandCursor);
        this->button->hide();
    }

    void setShare(double fraction, const QColor &color, bool highlighted)
    {
        this->fraction_ = std::clamp(fraction, 0.0, 1.0);
        this->color_ = color;
        this->highlighted_ = highlighted;
        this->update();
    }

    void applyScale(float scale)
    {
        setFontSize(this->title, 9.0F * scale, this->highlighted_);
        setFontSize(this->stats, 8.5F * scale);
        setFontSize(this->button, 8.5F * scale);
        this->button->setMinimumWidth(int(58 * scale));
    }

    QString id;
    ElidedLabel *title;
    QLabel *stats;
    QPushButton *button;

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);

        auto rect = QRectF(this->rect()).adjusted(0, 1, 0, -1);
        if (!this->button->isHidden())
        {
            // The bar ends before the button, the button sits beside it.
            rect.setRight(this->button->geometry().left() - 4);
        }
        QPainterPath track;
        track.addRoundedRect(rect, 3, 3);
        painter.fillPath(
            track, withAlpha(this->palette().color(QPalette::WindowText), 14));

        if (this->fraction_ > 0)
        {
            painter.setClipPath(track);
            auto fill = rect;
            fill.setWidth(rect.width() * this->fraction_);
            painter.fillRect(fill, withAlpha(this->color_,
                                             this->highlighted_ ? 120 : 64));
        }
    }

private:
    double fraction_ = 0;
    QColor color_;
    bool highlighted_ = false;
};

/// One event: a header line, an optional progress strip and optional rows.
class EventBar final : public QWidget
{
public:
    explicit EventBar(QWidget *parent)
        : QWidget(parent)
        , header(new QWidget(this))
        , kind(new QLabel(this->header))
        , title(new ElidedLabel(this->header))
        , status(new QLabel(this->header))
        , toggle(new QPushButton(this->header))
        , close(new QPushButton(u"×"_s, this->header))
        , strip(new ProgressStrip(this))
        , body(new QWidget(this))
        , bodyLayout(new QVBoxLayout(this->body))
        , message(new QLabel(this))
        , messageTimer_(new QTimer(this))
    {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

        auto *layout = new QVBoxLayout(this);
        // Room on the left for the accent stripe.
        layout->setContentsMargins(10, 5, 6, 5);
        layout->setSpacing(4);

        auto *headerRow = new QHBoxLayout(this->header);
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(6);
        headerRow->addWidget(this->kind);
        headerRow->addWidget(this->title, 1);
        headerRow->addWidget(this->status);
        headerRow->addWidget(this->toggle);
        headerRow->addWidget(this->close);
        layout->addWidget(this->header);

        this->kind->setTextFormat(Qt::PlainText);
        this->status->setTextFormat(Qt::PlainText);
        this->status->setStyleSheet(MUTED_STYLE);

        for (auto *button : {this->toggle, this->close})
        {
            button->setFlat(true);
            button->setCursor(Qt::PointingHandCursor);
        }
        this->close->setToolTip(u"Hide"_s);
        this->toggle->hide();

        layout->addWidget(this->strip);

        this->bodyLayout->setContentsMargins(0, 0, 0, 0);
        this->bodyLayout->setSpacing(2);
        layout->addWidget(this->body);

        this->message->setWordWrap(true);
        this->message->hide();
        layout->addWidget(this->message);

        this->messageTimer_->setSingleShot(true);
        QObject::connect(this->messageTimer_, &QTimer::timeout, this->message,
                         &QWidget::hide);

        this->header->installEventFilter(this);
        this->header->setCursor(Qt::ArrowCursor);
    }

    void setExpandable(bool expandable)
    {
        this->expandable_ = expandable;
        this->toggle->setVisible(expandable);
        this->header->setCursor(expandable ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);
    }

    void setExpanded(bool expanded)
    {
        this->body->setVisible(expanded);
        this->toggle->setText(expanded ? u"▾"_s : u"▸"_s);
        this->toggle->setToolTip(expanded ? u"Collapse"_s : u"Expand"_s);
    }

    void showMessage(const QString &text, bool error)
    {
        this->message->setText(text);
        this->message->setStyleSheet(
            QStringLiteral("color: %1;")
                .arg((error ? ERROR_COLOR : SUCCESS_COLOR).name()));
        this->message->show();
        this->messageTimer_->start(error ? 10s : 4s);
    }

    void applyScale(float scale)
    {
        setFontSize(this->kind, 9.5F * scale, true);
        setFontSize(this->title, 9.5F * scale);
        setFontSize(this->status, 9.0F * scale);
        setFontSize(this->message, 8.5F * scale);
        const int buttonSize = std::max(18, int(20 * scale));
        this->toggle->setFixedSize(buttonSize, buttonSize);
        this->close->setFixedSize(buttonSize, buttonSize);
        this->strip->setFixedHeight(std::max(3, int(4 * scale)));
        this->scale_ = scale;
    }

    [[nodiscard]] float scale() const
    {
        return this->scale_;
    }

    QString eventId;
    QColor accent = HYPE_TRAIN_COLOR;
    /// Expanded state while the user hasn't chosen one for this event.
    bool defaultExpanded = true;
    std::function<void()> onToggle;

    QWidget *header;
    QLabel *kind;
    ElidedLabel *title;
    QLabel *status;
    QPushButton *toggle;
    QPushButton *close;
    ProgressStrip *strip;
    QWidget *body;
    QVBoxLayout *bodyLayout;
    QLabel *message;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        auto *theme = getTheme();
        painter.fillRect(event->rect(), theme->splits.header.background);
        painter.setPen(theme->splits.header.border);
        painter.drawLine(0, 0, this->width() - 1, 0);
        painter.fillRect(QRect(0, 1, 3, this->height() - 1), this->accent);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->header && this->expandable_ &&
            event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton &&
            this->onToggle)
        {
            this->onToggle();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QTimer *messageTimer_;
    bool expandable_ = false;
    float scale_ = 1.0F;
};

}  // namespace chatterino::channeleventsbanner

namespace chatterino {

using namespace channeleventsbanner;
using channelevents::shortNumber;

namespace {

/// Grows or shrinks `rows` to `count`, connecting new rows' buttons to
/// `onClick(row id)`.
void resizeRows(std::vector<ShareRow *> &rows, EventBar *bar, size_t count,
                const std::function<void(const QString &)> &onClick)
{
    while (rows.size() < count)
    {
        auto *row = new ShareRow(bar->body);
        row->applyScale(bar->scale());
        bar->bodyLayout->addWidget(row);
        QObject::connect(row->button, &QPushButton::clicked, row,
                         [row, onClick] {
                             onClick(row->id);
                         });
        rows.push_back(row);
    }
    while (rows.size() > count)
    {
        delete rows.back();
        rows.pop_back();
    }
}

}  // namespace

ChannelEventsBanner::ChannelEventsBanner(Split *split)
    : BaseWidget(split)
    , split_(split)
    , hypeTrainBar_(new EventBar(this))
    , pollBar_(new EventBar(this))
    , predictionBar_(new EventBar(this))
    , tickTimer_(new QTimer(this))
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(this->hypeTrainBar_);
    layout->addWidget(this->pollBar_);
    layout->addWidget(this->predictionBar_);

    auto *bottomBorder = new QWidget(this);
    bottomBorder->setFixedHeight(1);
    bottomBorder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomBorder->setAutoFillBackground(true);
    {
        QPalette pal = bottomBorder->palette();
        pal.setColor(QPalette::Window, pal.color(QPalette::Mid));
        bottomBorder->setPalette(pal);
    }
    layout->addWidget(bottomBorder);

    this->hypeTrainBar_->body->hide();
    this->pollBar_->strip->hide();
    this->pollBar_->setExpandable(true);
    this->predictionBar_->setExpandable(true);
    this->pollBar_->accent = POLL_COLOR;

    for (auto *bar :
         {this->hypeTrainBar_, this->pollBar_, this->predictionBar_})
    {
        bar->hide();
        QObject::connect(bar->close, &QPushButton::clicked, this, [this, bar] {
            this->dismissed_.insert(bar->eventId);
            this->refresh();
        });
        QObject::connect(bar->toggle, &QPushButton::clicked, this, [this, bar] {
            this->toggleExpanded(bar->eventId, bar->defaultExpanded);
        });
        bar->onToggle = [this, bar] {
            this->toggleExpanded(bar->eventId, bar->defaultExpanded);
        };
    }

    this->tickTimer_->setInterval(1s);
    QObject::connect(this->tickTimer_, &QTimer::timeout, this, [this] {
        this->tick();
    });

    this->scaleChangedEvent(this->scale());
    this->hide();
}

void ChannelEventsBanner::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    this->channel_ = channel;
    this->votePending_.clear();

    if (channel != nullptr)
    {
        this->signalHolder_.managedConnect(channel->channelEventsChanged,
                                           [this] {
                                               this->refresh();
                                           });
    }

    this->refresh();
}

void ChannelEventsBanner::refresh()
{
    if (this->channel_ == nullptr)
    {
        this->hypeTrainBar_->hide();
        this->pollBar_->hide();
        this->predictionBar_->hide();
    }
    else
    {
        this->refreshHypeTrain();
        this->refreshPoll();
        this->refreshPrediction();
    }

    const bool anyShown = !this->hypeTrainBar_->isHidden() ||
                          !this->pollBar_->isHidden() ||
                          !this->predictionBar_->isHidden();
    this->setVisible(anyShown);
    this->updateTimer();
}

void ChannelEventsBanner::refreshHypeTrain()
{
    auto *bar = this->hypeTrainBar_;
    const auto &train = this->channel_->channelEvents().hypeTrain();
    if (!train || this->dismissed_.contains(train->id))
    {
        bar->hide();
        return;
    }

    bar->eventId = train->id;
    const auto color = train->color.isValid()   ? train->color
                       : train->isGoldenKappaTrain ? GOLDEN_KAPPA_COLOR
                                                   : HYPE_TRAIN_COLOR;
    bar->accent = color;
    bar->kind->setText(train->kindName());

    auto details = QStringLiteral("Level %1 · %2%")
                       .arg(train->level)
                       .arg(train->percent());
    if (train->isTreasureTrain && train->treasureDiscountPercent > 0)
    {
        details += train->treasureDiscountReached
                       ? QStringLiteral(" · %1% sub discount unlocked")
                             .arg(train->treasureDiscountPercent)
                       : QStringLiteral(" · %1% sub discount at level %2")
                             .arg(train->treasureDiscountPercent)
                             .arg(train->treasureDiscountLevel);
    }
    bar->title->setFullText(details);

    QStringList tooltip{
        QStringLiteral("%1 / %2 points to level %3")
            .arg(channelpoints::format::points(train->progress),
                 channelpoints::format::points(train->goal))
            .arg(train->level + 1),
        QStringLiteral("Total: %1 points")
            .arg(channelpoints::format::points(train->total)),
    };
    if (train->allTimeHighLevel > 0)
    {
        tooltip.append(QStringLiteral("All-time high: level %1")
                           .arg(train->allTimeHighLevel));
    }
    bar->setToolTip(tooltip.join('\n'));

    bar->status->setText(train->expiresAt.isValid()
                             ? countdown(train->expiresAt)
                             : QString());
    bar->strip->setSegments({{train->percent() / 100.0, color}});
    bar->update();
    bar->show();
}

void ChannelEventsBanner::refreshPoll()
{
    auto *bar = this->pollBar_;
    const auto &events = this->channel_->channelEvents();
    const auto &poll = events.poll();
    if (!poll || this->dismissed_.contains(poll->id))
    {
        bar->hide();
        return;
    }

    bar->eventId = poll->id;
    const auto myVotes = events.pollVotes(poll->id);
    const bool voted = !myVotes.isEmpty();
    const bool voting = this->votePending_ == poll->id;
    const bool canVote = poll->isActive() && !voting &&
                         (!voted || poll->multichoice);

    bar->defaultExpanded = poll->isActive() ? !voted : poll->isEnded();
    const bool expanded = this->isExpanded(poll->id, bar->defaultExpanded);
    bar->setExpanded(expanded);

    bar->kind->setText(u"Poll"_s);
    auto title = poll->title;
    const auto leaders = poll->leaders();
    if (!expanded && leaders.size() == 1)
    {
        title += QStringLiteral(" · %1 %2%")
                     .arg(leaders.front()->title)
                     .arg(poll->percent(*leaders.front()));
    }
    bar->title->setFullText(title);

    QString status;
    if (poll->isActive())
    {
        status = voting  ? QStringLiteral("Voting...")
                 : voted ? QStringLiteral("Voted")
                         : QString();
        if (poll->endsAt.isValid())
        {
            status += (status.isEmpty() ? QString() : u" · "_s) +
                      countdown(poll->endsAt);
        }
    }
    else
    {
        status = poll->status == PollStatus::Terminated
                     ? QStringLiteral("Ended early")
                     : QStringLiteral("Ended");
    }
    bar->status->setText(status);

    QStringList tooltip{
        QStringLiteral("%1 from %2")
            .arg(fullNumber(poll->totalVotes) +
                     QStringLiteral(" votes"),
                 channelpoints::format::points(poll->totalVoters) +
                     QStringLiteral(" voters")),
    };
    if (poll->subscriberOnly)
    {
        tooltip.append(QStringLiteral("Subscribers only"));
    }
    if (poll->channelPointsVotingEnabled)
    {
        tooltip.append(
            QStringLiteral("Extra votes for %1 channel points each (vote on "
                           "twitch.tv for those)")
                .arg(channelpoints::format::points(poll->channelPointsCost)));
    }
    bar->setToolTip(tooltip.join('\n'));

    resizeRows(this->pollRows_, bar, poll->choices.size(),
               [this](const QString &choiceId) {
                   this->vote(choiceId);
               });
    for (size_t i = 0; i < poll->choices.size(); i++)
    {
        const auto &choice = poll->choices[i];
        auto *row = this->pollRows_[i];
        const bool mine = myVotes.contains(choice.id);
        const bool leading =
            std::find(leaders.begin(), leaders.end(), &choice) != leaders.end();

        row->id = choice.id;
        row->title->setFullText(mine ? u"✓ "_s + choice.title
                                     : choice.title);
        row->stats->setText(QStringLiteral("%1% · %2")
                                .arg(poll->percent(choice))
                                .arg(shortNumber(choice.votes)));
        row->stats->setToolTip(
            QStringLiteral("%1 votes from %2 voters")
                .arg(fullNumber(choice.votes),
                     channelpoints::format::points(choice.voters)));
        const auto fraction =
            poll->totalVotes > 0
                ? double(choice.votes) / double(poll->totalVotes)
                : 0.0;
        row->setShare(fraction, POLL_COLOR,
                      mine || (poll->isEnded() && leading));
        row->applyScale(bar->scale());

        row->button->setVisible(poll->isActive());
        row->button->setText(u"Vote"_s);
        row->button->setEnabled(canVote && !mine);
    }

    bar->update();
    bar->show();
}

void ChannelEventsBanner::refreshPrediction()
{
    auto *bar = this->predictionBar_;
    const auto &events = this->channel_->channelEvents();
    const auto &prediction = events.prediction();
    if (!prediction || this->dismissed_.contains(prediction->id))
    {
        bar->hide();
        return;
    }

    bar->eventId = prediction->id;
    const auto own = events.ownPrediction(prediction->id);

    bar->defaultExpanded =
        prediction->isOpen() ? !own.has_value() : prediction->isEnded();
    const bool expanded = this->isExpanded(prediction->id, bar->defaultExpanded);
    bar->setExpanded(expanded);

    const auto *winner = prediction->findOutcome(prediction->winningOutcomeId);
    bar->accent = winner != nullptr ? outcomeColor(winner->color)
                                    : BLUE_OUTCOME_COLOR;
    bar->kind->setText(u"Prediction"_s);
    bar->title->setFullText(prediction->title);

    // Rich text so the outcome you picked shows in its color.
    QString status;
    if (own)
    {
        status = QStringLiteral("You: %1").arg(shortNumber(own->points));
        if (const auto *picked = prediction->findOutcome(own->outcomeId))
        {
            const auto name =
                bar->status->fontMetrics().elidedText(
                    picked->title, Qt::ElideRight,
                    int(140 * bar->scale()));
            status += QStringLiteral(" on <span style=\"color:%1\">%2</span>")
                          .arg(outcomeColor(picked->color).name(),
                               name.toHtmlEscaped());
        }
        status += u" · "_s;
    }
    if (prediction->isOpen() && prediction->locksAt.isValid())
    {
        status += countdown(prediction->locksAt).toHtmlEscaped();
    }
    else
    {
        status += channelevents::describePredictionStatus(prediction->status)
                      .toHtmlEscaped();
    }
    bar->status->setTextFormat(Qt::RichText);
    bar->status->setText(status);
    bar->status->setToolTip(
        own ? QStringLiteral("You predicted %1 points").arg(fullNumber(own->points))
            : QString());

    const auto total = prediction->totalPoints();
    std::vector<ProgressStrip::Segment> segments;
    for (const auto &outcome : prediction->outcomes)
    {
        segments.push_back({
            total > 0 ? double(outcome.points) / double(total) : 0.0,
            outcomeColor(outcome.color),
        });
    }
    bar->strip->setSegments(std::move(segments));
    bar->strip->setVisible(!expanded && total > 0);
    bar->setToolTip(QStringLiteral("%1 points from %2 users")
                        .arg(fullNumber(total),
                             channelpoints::format::points(
                                 prediction->totalUsers())));

    resizeRows(this->predictionRows_, bar, prediction->outcomes.size(),
               [this](const QString &outcomeId) {
                   this->predict(outcomeId);
               });
    for (size_t i = 0; i < prediction->outcomes.size(); i++)
    {
        const auto &outcome = prediction->outcomes[i];
        auto *row = this->predictionRows_[i];
        const bool mine = own && own->outcomeId == outcome.id;
        const bool won = winner != nullptr && winner->id == outcome.id;

        row->id = outcome.id;
        auto title = outcome.title;
        if (won)
        {
            title = u"✓ "_s + title;
        }
        if (mine)
        {
            title += QStringLiteral(" (you: %1)").arg(shortNumber(own->points));
        }
        row->title->setFullText(title);

        const auto ratio = prediction->payoutRatio(outcome);
        row->stats->setText(
            QStringLiteral("%1% · %2 · %3")
                .arg(prediction->percent(outcome))
                .arg(shortNumber(outcome.points))
                .arg(ratio > 0 ? QStringLiteral("1:%1").arg(ratio, 0, 'f', 2)
                               : QStringLiteral("1:–")));
        row->stats->setToolTip(
            QStringLiteral("%1 points from %2 users")
                .arg(fullNumber(outcome.points),
                     channelpoints::format::points(outcome.users)));
        row->setShare(total > 0 ? double(outcome.points) / double(total) : 0.0,
                      outcomeColor(outcome.color), won || mine);
        row->applyScale(bar->scale());

        row->button->setVisible(prediction->isOpen());
        row->button->setText(mine ? u"Add"_s : u"Predict"_s);
        row->button->setEnabled(!own || mine);
    }

    bar->update();
    bar->show();
}

void ChannelEventsBanner::tick()
{
    if (this->channel_ == nullptr)
    {
        return;
    }
    // Refreshes through channelEventsChanged if something was dropped.
    this->channel_->expireChannelEvents();
    this->refresh();
}

void ChannelEventsBanner::updateTimer()
{
    const bool needed = this->channel_ != nullptr && !this->isHidden();
    if (needed && !this->tickTimer_->isActive())
    {
        this->tickTimer_->start();
    }
    else if (!needed)
    {
        this->tickTimer_->stop();
    }
}

bool ChannelEventsBanner::isExpanded(const QString &eventId,
                                     bool defaultExpanded) const
{
    return this->expanded_.value(eventId, defaultExpanded);
}

void ChannelEventsBanner::toggleExpanded(const QString &eventId,
                                         bool defaultExpanded)
{
    this->expanded_[eventId] = !this->isExpanded(eventId, defaultExpanded);
    this->refresh();
}

bool ChannelEventsBanner::ensureSignedIn(const QString &action)
{
    auto *channelPoints = getApp()->getChannelPoints();
    if (channelPoints == nullptr)
    {
        return false;
    }
    if (channelPoints->isSignedIn())
    {
        return true;
    }

    const auto answer = QMessageBox::question(
        this, u"Sign in needed"_s,
        QStringLiteral("To %1, Chatterino uses its channel points sign-in, "
                       "which isn't set up for this account yet.\n\n"
                       "Open channel points to sign in?")
            .arg(action),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes && this->split_ != nullptr)
    {
        this->split_->openChannelPointsPopup();
    }
    return false;
}

void ChannelEventsBanner::vote(const QString &choiceId)
{
    if (this->channel_ == nullptr)
    {
        return;
    }

    const auto findChoice = [this, &choiceId]() -> const PollChoice * {
        const auto &poll = this->channel_->channelEvents().poll();
        if (!poll || !poll->isActive())
        {
            return nullptr;
        }
        for (const auto &choice : poll->choices)
        {
            if (choice.id == choiceId)
            {
                return &choice;
            }
        }
        return nullptr;
    };

    const auto *choice = findChoice();
    if (choice == nullptr || !this->ensureSignedIn(u"vote in polls"_s))
    {
        return;
    }

    const auto pollId = this->channel_->channelEvents().poll()->id;
    const auto pollTitle = this->channel_->channelEvents().poll()->title;
    const auto choiceTitle = choice->title;

    const auto answer = QMessageBox::question(
        this, u"Vote"_s,
        QStringLiteral("Vote for “%1”?\n\n%2")
            .arg(choiceTitle, pollTitle),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    // The poll may have changed while the question was open.
    if (answer != QMessageBox::Yes || this->channel_ == nullptr ||
        findChoice() == nullptr ||
        this->channel_->channelEvents().poll()->id != pollId)
    {
        return;
    }

    this->votePending_ = pollId;
    this->refresh();

    QPointer<ChannelEventsBanner> self(this);
    auto weakChannel = this->channel_->weakFromThis();
    getApp()->getChannelPoints()->voteInPoll(
        this->channel_->getName(), pollId, choiceId,
        [self, weakChannel, pollId, choiceId](bool ok, const QString &message) {
            if (auto channel = weakChannel.lock(); channel && ok)
            {
                channel->recordPollVote(pollId, choiceId);
            }
            if (self.isNull())
            {
                return;
            }
            if (self->votePending_ == pollId)
            {
                self->votePending_.clear();
            }
            self->pollBar_->showMessage(ok ? u"Vote counted."_s : message, !ok);
            self->refresh();
        });
}

void ChannelEventsBanner::predict(const QString &outcomeId)
{
    if (this->channel_ == nullptr)
    {
        return;
    }
    const auto &prediction = this->channel_->channelEvents().prediction();
    if (!prediction || !prediction->isOpen() ||
        prediction->findOutcome(outcomeId) == nullptr ||
        !this->ensureSignedIn(u"make predictions"_s))
    {
        return;
    }

    auto *dialog = new PredictionDialog(this->channel_->weakFromThis(),
                                        prediction->id, outcomeId, this);
    widgets::showAndMoveWindowTo(dialog, QCursor::pos(),
                                 widgets::BoundsChecking::CursorPosition);
}

void ChannelEventsBanner::scaleChangedEvent(float newScale)
{
    for (auto *bar :
         {this->hypeTrainBar_, this->pollBar_, this->predictionBar_})
    {
        bar->applyScale(newScale);
    }
    for (auto *row : this->pollRows_)
    {
        row->applyScale(newScale);
    }
    for (auto *row : this->predictionRows_)
    {
        row->applyScale(newScale);
    }
}

void ChannelEventsBanner::mousePressEvent(QMouseEvent * /*event*/)
{
    // ignore to disable the parent's right click menu
}

}  // namespace chatterino
