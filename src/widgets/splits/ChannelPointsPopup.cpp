// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/ChannelPointsPopup.hpp"

#include "Application.hpp"
#include "controllers/channelpoints/ChannelPointsAuth.hpp"
#include "controllers/channelpoints/ChannelPointsController.hpp"
#include "controllers/channelpoints/ChannelPointsLog.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/splits/ChannelPointsConfirmDialog.hpp"
#include "widgets/splits/ChannelPointsFormat.hpp"
#include "widgets/splits/Split.hpp"

#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace fmt = channelpoints::format;

namespace {

/// Results (e.g. "Redeemed") stay on the row this long.
constexpr qint64 RESULT_VISIBLE_SECS = 20;

const QColor SUCCESS_COLOR("#5cb85c");
const QColor ERROR_COLOR("#e5675f");

QString favoriteKey(const QString &channelLogin, const QString &rewardId)
{
    return QStringLiteral("%1|%2").arg(channelLogin, rewardId);
}

QString colorCss(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

/// Avoids re-polishing widgets every refresh when nothing changed.
void setCss(QWidget *widget, const QString &css)
{
    if (widget->styleSheet() != css)
    {
        widget->setStyleSheet(css);
    }
}

QColor dimmed(const QColor &color, int alpha = 150)
{
    auto out = color;
    out.setAlpha(alpha);
    return out;
}

/// Rows that can't become available on their own any time soon.
bool isLongTermUnavailable(RewardBlock block)
{
    switch (block)
    {
        case RewardBlock::Disabled:
        case RewardBlock::Paused:
        case RewardBlock::OutOfStock:
        case RewardBlock::StreamLimit:
        case RewardBlock::UserStreamLimit:
            return true;
        default:
            return false;
    }
}

QLabel *makeSectionHeader(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text.toUpper(), parent);
    auto font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 0.85);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    label->setFont(font);
    label->setContentsMargins(10, 10, 10, 2);
    return label;
}

}  // namespace

/// One reward. Created once per reward and updated in place.
class ChannelPointsRewardRow : public QFrame
{
public:
    ChannelPointsRewardRow(QWidget *parent,
                           std::function<void(const ChannelPointRewardView &)>
                               onAction,
                           std::function<void(const QString &)> onFavorite)
        : QFrame(parent)
        , onAction_(std::move(onAction))
        , onFavorite_(std::move(onFavorite))
    {
        this->setObjectName("cpRow");
        this->setAttribute(Qt::WA_Hover);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 8, 0);
        layout->setSpacing(6);

        this->strip_ = new QWidget(this);
        this->strip_->setFixedWidth(4);
        this->strip_->setAutoFillBackground(true);
        layout->addWidget(this->strip_);

        this->star_ = new QToolButton(this);
        this->star_->setAutoRaise(true);
        this->star_->setFixedSize(24, 24);
        this->star_->setCursor(Qt::PointingHandCursor);
        this->star_->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(this->star_);

        auto *textLayout = new QVBoxLayout;
        textLayout->setContentsMargins(0, 6, 0, 6);
        textLayout->setSpacing(1);
        this->title_ = new QLabel(this);
        this->title_->setTextFormat(Qt::PlainText);
        this->title_->setMinimumWidth(60);
        this->title_->setSizePolicy(QSizePolicy::Ignored,
                                    QSizePolicy::Preferred);
        auto titleFont = this->title_->font();
        titleFont.setBold(true);
        this->title_->setFont(titleFont);
        this->status_ = new QLabel(this);
        this->status_->setTextFormat(Qt::PlainText);
        this->status_->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
        auto statusFont = this->status_->font();
        statusFont.setPointSizeF(statusFont.pointSizeF() * 0.9);
        this->status_->setFont(statusFont);
        textLayout->addWidget(this->title_);
        textLayout->addWidget(this->status_);
        layout->addLayout(textLayout, 1);

        this->cost_ = new QLabel(this);
        this->cost_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        this->cost_->setMinimumWidth(48);
        layout->addWidget(this->cost_);

        this->action_ = new QPushButton(this);
        this->action_->setFixedWidth(78);
        this->action_->setCursor(Qt::PointingHandCursor);
        layout->addWidget(this->action_);

        QObject::connect(this->star_, &QToolButton::clicked, this, [this] {
            this->onFavorite_(this->view_.reward.id);
        });
        QObject::connect(this->action_, &QPushButton::clicked, this, [this] {
            this->onAction_(this->view_);
        });
    }

    const ChannelPointRewardView &view() const
    {
        return this->view_;
    }

    bool matches(const QString &filter) const
    {
        return filter.isEmpty() ||
               this->view_.reward.title.contains(filter, Qt::CaseInsensitive) ||
               this->view_.reward.prompt.contains(filter, Qt::CaseInsensitive);
    }

    void update(const ChannelPointRewardView &view, std::optional<int> balance,
                bool favorite)
    {
        this->view_ = view;
        const auto &reward = view.reward;
        const auto text = this->palette().color(QPalette::WindowText);
        const auto now = QDateTime::currentDateTime();

        // Accent strip in the reward's own color, like on Twitch.
        QColor stripColor(reward.backgroundColor);
        if (!stripColor.isValid())
        {
            stripColor = dimmed(text, 60);
        }
        auto stripPalette = this->strip_->palette();
        stripPalette.setColor(QPalette::Window, stripColor);
        this->strip_->setPalette(stripPalette);

        this->star_->setText(favorite ? QStringLiteral("★")
                                      : QStringLiteral("☆"));
        this->star_->setToolTip(favorite ? "Remove from favorites"
                                         : "Add to favorites");
        setCss(this->star_,
            QStringLiteral("QToolButton { color: %1; border: none; }")
                .arg(colorCss(favorite ? QColor("#f5c518")
                                       : dimmed(text, 110))));

        this->setElided(this->title_, reward.title);
        const bool ready = view.block == RewardBlock::None;
        setCss(this->title_,
            QStringLiteral("color: %1").arg(colorCss(ready || view.queueMode !=
                                                                  ChannelPointQueueMode::None
                                                         ? text
                                                         : dimmed(text, 170))));

        this->cost_->setText(fmt::points(reward.cost));
        setCss(this->cost_,
            QStringLiteral("color: %1")
                .arg(colorCss(view.block == RewardBlock::NotEnoughPoints
                                  ? ERROR_COLOR
                                  : text)));

        // Status line: the single most useful thing to know right now.
        QString status;
        QColor statusColor = dimmed(text, 150);
        const auto &result = view.lastResult;
        const bool recentResult =
            result.kind != ChannelPointResult::Kind::None &&
            result.at.isValid() && result.at.secsTo(now) < RESULT_VISIBLE_SECS;

        if (view.inFlight)
        {
            status = result.text.isEmpty() ? QStringLiteral("Redeeming…")
                                           : result.text;
        }
        else if (recentResult &&
                 result.kind != ChannelPointResult::Kind::Info)
        {
            status = result.text;
            statusColor = result.kind == ChannelPointResult::Kind::Success
                              ? SUCCESS_COLOR
                              : ERROR_COLOR;
        }
        else if (view.queueMode != ChannelPointQueueMode::None)
        {
            const auto label = view.queueMode == ChannelPointQueueMode::Once
                                   ? QStringLiteral("Queued")
                                   : QStringLiteral("Repeating");
            if (view.block != RewardBlock::None)
            {
                status = QStringLiteral("%1 · waiting: %2")
                             .arg(label, fmt::blockDetail(view, balance));
            }
            else if (view.nextAttemptAt.isValid() &&
                     view.nextAttemptAt > QDateTime::currentDateTimeUtc())
            {
                status = QStringLiteral("%1 · next try in %2")
                             .arg(label, fmt::timeUntil(view.nextAttemptAt));
            }
            else
            {
                status = QStringLiteral("%1 · redeeming soon").arg(label);
            }
            statusColor = this->palette().color(QPalette::Highlight);
            if (statusColor.lightnessF() < 0.45)
            {
                statusColor = statusColor.lighter(160);
            }
        }
        else if (!ready)
        {
            status = fmt::blockDetail(view, balance);
        }
        else
        {
            status = reward.isUserInputRequired
                         ? QStringLiteral("Ready · asks for text")
                         : QStringLiteral("Ready");
        }
        this->setElided(this->status_, status);
        setCss(this->status_,
            QStringLiteral("color: %1").arg(colorCss(statusColor)));

        // One button whose meaning follows the state.
        if (view.inFlight)
        {
            this->action_->setText("Sending…");
            this->action_->setEnabled(false);
            this->action_->setToolTip({});
        }
        else if (view.queueMode != ChannelPointQueueMode::None)
        {
            this->action_->setText("Cancel");
            this->action_->setEnabled(true);
            this->action_->setToolTip("Stop waiting for this reward");
        }
        else if (ready)
        {
            this->action_->setText("Redeem");
            this->action_->setEnabled(true);
            this->action_->setToolTip({});
        }
        else
        {
            this->action_->setText("Queue…");
            this->action_->setEnabled(true);
            this->action_->setToolTip(
                "Redeem automatically once it becomes available");
        }

        QString tooltip = QStringLiteral("<b>%1</b> · %2 points")
                              .arg(reward.title.toHtmlEscaped(),
                                   fmt::points(reward.cost));
        if (!reward.prompt.trimmed().isEmpty())
        {
            tooltip += "<br>" + reward.prompt.trimmed().toHtmlEscaped();
        }
        if (!status.isEmpty())
        {
            tooltip += "<br><i>" + status.toHtmlEscaped() + "</i>";
        }
        this->title_->setToolTip(tooltip);
        this->status_->setToolTip(tooltip);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QFrame::resizeEvent(event);
        this->setElided(this->title_, this->title_->property("fullText").toString());
        this->setElided(this->status_,
                        this->status_->property("fullText").toString());
    }

private:
    void setElided(QLabel *label, const QString &text)
    {
        label->setProperty("fullText", text);
        const auto width = std::max(label->width(), 60);
        label->setText(
            label->fontMetrics().elidedText(text, Qt::ElideRight, width));
    }

    std::function<void(const ChannelPointRewardView &)> onAction_;
    std::function<void(const QString &)> onFavorite_;
    ChannelPointRewardView view_;

    QWidget *strip_{};
    QToolButton *star_{};
    QLabel *title_{};
    QLabel *status_{};
    QLabel *cost_{};
    QPushButton *action_{};
};

ChannelPointsPopup::ChannelPointsPopup(Split *split, QWidget *parent)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(split->getChannel().get());
    if (twitchChannel != nullptr)
    {
        this->channelLogin_ = twitchChannel->getName().trimmed().toLower();
    }

    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(
        QStringLiteral("Channel Points - #%1").arg(this->channelLogin_));
    this->resize(420, 560);

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    this->buildHeader(root);

    this->banner_ = new QLabel(this);
    this->banner_->setWordWrap(true);
    this->banner_->setTextFormat(Qt::PlainText);
    this->banner_->setContentsMargins(10, 6, 10, 6);
    this->banner_->hide();
    root->addWidget(this->banner_);

    this->stack_ = new QStackedWidget(this);
    this->connectPage_ = this->buildConnectPage();
    this->messagePage_ = this->buildMessagePage();
    this->listPage_ = this->buildListPage();
    this->stack_->addWidget(this->connectPage_);
    this->stack_->addWidget(this->messagePage_);
    this->stack_->addWidget(this->listPage_);
    root->addWidget(this->stack_, 1);

    this->buildFooter(root);

    QObject::connect(getApp()->getChannelPoints(),
                     &ChannelPointsController::channelStateChanged, this,
                     [this](const QString &channelLogin) {
                         if (channelLogin.isEmpty() ||
                             channelLogin == this->channelLogin_)
                         {
                             this->refresh();
                         }
                     });

    // Keeps countdowns and "updated Xs ago" current. Rows are updated in
    // place, so this is cheap and doesn't disturb the list.
    this->refreshTimer_.setInterval(1000);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });

    this->themeChangedEvent();
    this->refresh();
}

ChannelPointsPopup::~ChannelPointsPopup()
{
    this->detachFromController();
}

void ChannelPointsPopup::buildHeader(QVBoxLayout *root)
{
    auto *header = new QWidget(this);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(10, 8, 6, 6);
    layout->setSpacing(8);

    this->channelLabel_ = new QLabel("#" + this->channelLogin_, header);
    auto channelFont = this->channelLabel_->font();
    channelFont.setBold(true);
    channelFont.setPointSizeF(channelFont.pointSizeF() * 1.1);
    this->channelLabel_->setFont(channelFont);
    layout->addWidget(this->channelLabel_, 1);

    this->balanceLabel_ = new QLabel(header);
    this->balanceLabel_->setTextFormat(Qt::RichText);
    this->balanceLabel_->setToolTip("Your channel points");
    layout->addWidget(this->balanceLabel_);

    this->menuButton_ = new QToolButton(header);
    this->menuButton_->setText(QStringLiteral("⋯"));
    this->menuButton_->setAutoRaise(true);
    this->menuButton_->setFixedSize(28, 26);
    this->menuButton_->setPopupMode(QToolButton::InstantPopup);
    this->menuButton_->setStyleSheet(
        "QToolButton::menu-indicator { image: none; }");
    this->menuButton_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(this->menuButton_);

    auto *menu = new QMenu(this->menuButton_);
    this->menuButton_->setMenu(menu);
    QObject::connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        auto *controller = getApp()->getChannelPoints();
        const auto state = controller->getViewState(this->channelLogin_);
        const bool signedIn =
            state.auth.state == ChannelPointsAuthState::SignedIn;

        auto *refresh = menu->addAction("Refresh", this, [this] {
            getApp()->getChannelPoints()->refreshChannel(this->channelLogin_);
        });
        refresh->setEnabled(signedIn);

        auto *stopAll = menu->addAction(
            state.queuedCount > 0
                ? QStringLiteral("Stop all queues (%1)").arg(state.queuedCount)
                : QStringLiteral("Stop all queues"),
            this, [this] {
                getApp()->getChannelPoints()->cancelAllQueues(
                    this->channelLogin_);
            });
        stopAll->setEnabled(state.queuedCount > 0);

        menu->addAction("Open log file", this, [] {
            const auto path = channelpoints::logFilePath();
            if (!path.isEmpty() && QFileInfo::exists(path))
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            }
        });

        if (signedIn)
        {
            menu->addSeparator();
            menu->addAction(
                QStringLiteral("Disconnect @%1").arg(state.auth.login), this,
                [] {
                    getApp()->getChannelPoints()->auth()->signOut();
                });
        }
    });

    root->addWidget(header);

    auto *searchRow = new QWidget(this);
    auto *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(8, 0, 8, 6);
    this->searchEdit_ = new QLineEdit(searchRow);
    this->searchEdit_->setPlaceholderText("Search rewards");
    this->searchEdit_->setClearButtonEnabled(true);
    this->searchEdit_->installEventFilter(this);
    searchLayout->addWidget(this->searchEdit_);
    root->addWidget(searchRow);

    QObject::connect(this->searchEdit_, &QLineEdit::textChanged, this,
                     [this] {
                         this->refresh();
                         this->scrollArea_->ensureVisible(0, 0);
                     });
    QObject::connect(this->searchEdit_, &QLineEdit::returnPressed, this,
                     [this] {
                         // Enter acts on the only match.
                         ChannelPointsRewardRow *only = nullptr;
                         for (auto *row : this->rows_)
                         {
                             if (row->isVisible())
                             {
                                 if (only != nullptr)
                                 {
                                     return;
                                 }
                                 only = row;
                             }
                         }
                         if (only != nullptr)
                         {
                             this->handleRowAction(only->view());
                         }
                     });
}

QWidget *ChannelPointsPopup::buildConnectPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(10);
    layout->addStretch(1);

    this->connectTitle_ = new QLabel(page);
    auto titleFont = this->connectTitle_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    this->connectTitle_->setFont(titleFont);
    this->connectTitle_->setAlignment(Qt::AlignCenter);
    this->connectTitle_->setWordWrap(true);
    layout->addWidget(this->connectTitle_);

    this->connectText_ = new QLabel(page);
    this->connectText_->setAlignment(Qt::AlignCenter);
    this->connectText_->setWordWrap(true);
    layout->addWidget(this->connectText_);

    this->codeLabel_ = new QLabel(page);
    auto codeFont = QFont("Consolas");
    codeFont.setStyleHint(QFont::Monospace);
    codeFont.setPointSizeF(this->font().pointSizeF() * 2.2);
    codeFont.setBold(true);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 4);
    this->codeLabel_->setFont(codeFont);
    this->codeLabel_->setAlignment(Qt::AlignCenter);
    this->codeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->codeLabel_->setContentsMargins(0, 8, 0, 8);
    layout->addWidget(this->codeLabel_);

    auto *codeButtons = new QHBoxLayout;
    codeButtons->setSpacing(8);
    codeButtons->addStretch(1);
    this->copyCodeButton_ = new QPushButton("Copy code", page);
    this->openActivateButton_ = new QPushButton("Open twitch.tv/activate", page);
    this->openActivateButton_->setDefault(true);
    codeButtons->addWidget(this->copyCodeButton_);
    codeButtons->addWidget(this->openActivateButton_);
    codeButtons->addStretch(1);
    layout->addLayout(codeButtons);

    this->codeHint_ = new QLabel(page);
    this->codeHint_->setAlignment(Qt::AlignCenter);
    this->codeHint_->setWordWrap(true);
    layout->addWidget(this->codeHint_);

    auto *mainButtons = new QHBoxLayout;
    mainButtons->addStretch(1);
    this->connectButton_ = new QPushButton("Connect to Twitch", page);
    this->connectButton_->setMinimumWidth(160);
    this->cancelLoginButton_ = new QPushButton("Cancel", page);
    mainButtons->addWidget(this->connectButton_);
    mainButtons->addWidget(this->cancelLoginButton_);
    mainButtons->addStretch(1);
    layout->addLayout(mainButtons);

    layout->addStretch(2);

    QObject::connect(this->connectButton_, &QPushButton::clicked, this, [] {
        getApp()->getChannelPoints()->auth()->startLogin();
    });
    QObject::connect(this->cancelLoginButton_, &QPushButton::clicked, this,
                     [] {
                         getApp()->getChannelPoints()->auth()->cancelLogin();
                     });
    QObject::connect(this->copyCodeButton_, &QPushButton::clicked, this,
                     [this] {
                         crossPlatformCopy(this->codeLabel_->text());
                         this->copyCodeButton_->setText("Copied");
                         QTimer::singleShot(1500, this->copyCodeButton_,
                                            [button = this->copyCodeButton_] {
                                                button->setText("Copy code");
                                            });
                     });
    QObject::connect(this->openActivateButton_, &QPushButton::clicked, this,
                     [this] {
                         QDesktopServices::openUrl(QUrl(
                             this->verificationUri_.isEmpty()
                                 ? QStringLiteral("https://www.twitch.tv/activate")
                                 : this->verificationUri_));
                     });

    return page;
}

QWidget *ChannelPointsPopup::buildMessagePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addStretch(1);

    this->messageLabel_ = new QLabel(page);
    this->messageLabel_->setAlignment(Qt::AlignCenter);
    this->messageLabel_->setWordWrap(true);
    layout->addWidget(this->messageLabel_);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    this->retryButton_ = new QPushButton("Try again", page);
    buttons->addWidget(this->retryButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    layout->addStretch(2);

    QObject::connect(this->retryButton_, &QPushButton::clicked, this, [this] {
        getApp()->getChannelPoints()->refreshChannel(this->channelLogin_);
    });

    return page;
}

QWidget *ChannelPointsPopup::buildListPage()
{
    this->scrollArea_ = new QScrollArea(this);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget(this->scrollArea_);
    this->listLayout_ = new QVBoxLayout(content);
    this->listLayout_->setContentsMargins(0, 0, 0, 8);
    this->listLayout_->setSpacing(1);

    this->favoritesHeader_ = makeSectionHeader("Favorites", content);
    this->availableHeader_ = makeSectionHeader("Rewards", content);
    this->unavailableHeader_ = makeSectionHeader("Unavailable", content);
    this->noMatchesLabel_ = new QLabel("No rewards match your search.", content);
    this->noMatchesLabel_->setAlignment(Qt::AlignCenter);
    this->noMatchesLabel_->setContentsMargins(0, 24, 0, 24);

    this->listLayout_->addWidget(this->favoritesHeader_);
    this->listLayout_->addWidget(this->availableHeader_);
    this->listLayout_->addWidget(this->unavailableHeader_);
    this->listLayout_->addWidget(this->noMatchesLabel_);
    this->listLayout_->addStretch(1);

    this->scrollArea_->setWidget(content);
    return this->scrollArea_;
}

void ChannelPointsPopup::buildFooter(QVBoxLayout *root)
{
    auto *footer = new QWidget(this);
    auto *layout = new QHBoxLayout(footer);
    layout->setContentsMargins(10, 4, 8, 6);
    layout->setSpacing(8);

    this->liveLabel_ = new QLabel(footer);
    this->updatedLabel_ = new QLabel(footer);
    auto small = this->liveLabel_->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    this->liveLabel_->setFont(small);
    this->updatedLabel_->setFont(small);
    layout->addWidget(this->liveLabel_);
    layout->addWidget(this->updatedLabel_);
    layout->addStretch(1);

    this->stopAllButton_ = new QPushButton(footer);
    this->stopAllButton_->setToolTip("Stop every queued redeem in this channel");
    layout->addWidget(this->stopAllButton_);
    QObject::connect(this->stopAllButton_, &QPushButton::clicked, this, [this] {
        getApp()->getChannelPoints()->cancelAllQueues(this->channelLogin_);
    });

    root->addWidget(footer);
}

void ChannelPointsPopup::showEvent(QShowEvent *event)
{
    this->attachToController();
    this->refreshTimer_.start();
    this->refresh();
    BasePopup::showEvent(event);
    this->searchEdit_->setFocus();
}

void ChannelPointsPopup::hideEvent(QHideEvent *event)
{
    this->refreshTimer_.stop();
    this->detachFromController();
    if (!this->confirmDialog_.isNull())
    {
        this->confirmDialog_->close();
    }
    BasePopup::hideEvent(event);
}

void ChannelPointsPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    if (this->listLayout_ == nullptr)
    {
        return;
    }

    const auto text = this->palette().color(QPalette::WindowText);
    const auto headerCss =
        QStringLiteral("color: %1").arg(colorCss(dimmed(text, 140)));
    this->favoritesHeader_->setStyleSheet(headerCss);
    this->availableHeader_->setStyleSheet(headerCss);
    this->unavailableHeader_->setStyleSheet(headerCss);
    this->noMatchesLabel_->setStyleSheet(headerCss);
    this->updatedLabel_->setStyleSheet(headerCss);
    this->codeHint_->setStyleSheet(headerCss);
    this->connectText_->setStyleSheet(
        QStringLiteral("color: %1").arg(colorCss(dimmed(text, 200))));
    this->scrollArea_->widget()->setStyleSheet(
        QStringLiteral("QFrame#cpRow:hover { background: %1; }")
            .arg(colorCss(dimmed(text, 18))));
    this->banner_->setStyleSheet(
        QStringLiteral("background: %1; color: %2;")
            .arg(colorCss(dimmed(ERROR_COLOR, 50)), colorCss(text)));

    this->refresh();
}

void ChannelPointsPopup::refresh()
{
    if (this->stack_ == nullptr)
    {
        return;
    }

    const auto state =
        getApp()->getChannelPoints()->getViewState(this->channelLogin_);
    this->balance_ = state.balance;

    this->refreshHeader(state);
    this->refreshFooter(state);

    const auto authState = state.auth.state;
    const bool showConnect =
        authState == ChannelPointsAuthState::NoChatterinoAccount ||
        authState == ChannelPointsAuthState::SignedOut ||
        authState == ChannelPointsAuthState::RequestingCode ||
        authState == ChannelPointsAuthState::AwaitingActivation;

    QString bannerText;
    if (showConnect)
    {
        this->refreshConnectPage(state);
        this->stack_->setCurrentWidget(this->connectPage_);
    }
    else if (authState == ChannelPointsAuthState::Validating && !state.loaded)
    {
        this->messageLabel_->setText("Checking your channel points sign-in…");
        this->retryButton_->hide();
        this->stack_->setCurrentWidget(this->messagePage_);
    }
    else if (!state.loaded)
    {
        if (!state.error.isEmpty() && !state.loading)
        {
            this->messageLabel_->setText(state.error);
            this->retryButton_->show();
        }
        else
        {
            this->messageLabel_->setText("Loading rewards…");
            this->retryButton_->hide();
        }
        this->stack_->setCurrentWidget(this->messagePage_);
    }
    else if (state.rewards.isEmpty())
    {
        this->messageLabel_->setText(
            "This channel has no custom rewards you can redeem.");
        this->retryButton_->hide();
        this->stack_->setCurrentWidget(this->messagePage_);
    }
    else
    {
        // Data is still usable; mention the failed reload above the list.
        bannerText = state.error;
        this->refreshList(state);
        this->stack_->setCurrentWidget(this->listPage_);
    }

    this->banner_->setText(bannerText);
    this->banner_->setVisible(!bannerText.isEmpty());
    this->searchEdit_->parentWidget()->setVisible(
        this->stack_->currentWidget() == this->listPage_);

    // Keep an open confirm dialog in sync with the reward.
    if (!this->confirmDialog_.isNull())
    {
        const auto id = this->confirmDialog_->rewardId();
        auto it = std::find_if(state.rewards.begin(), state.rewards.end(),
                               [&id](const auto &view) {
                                   return view.reward.id == id;
                               });
        if (it == state.rewards.end() ||
            authState != ChannelPointsAuthState::SignedIn)
        {
            this->confirmDialog_->close();
        }
        else
        {
            this->confirmDialog_->updateReward(*it, state.balance);
        }
    }
}

void ChannelPointsPopup::refreshHeader(const ChannelPointsViewState &state)
{
    const auto name = state.channelDisplayName.isEmpty()
                          ? this->channelLogin_
                          : state.channelDisplayName;
    this->channelLabel_->setText("#" + name);

    if (state.balance)
    {
        const auto dim = dimmed(this->palette().color(QPalette::WindowText), 150);
        this->balanceLabel_->setText(
            QStringLiteral("<b>%1</b> <span style=\"color:%2\">points</span>")
                .arg(fmt::points(*state.balance), colorCss(dim)));
        this->balanceLabel_->show();
    }
    else
    {
        this->balanceLabel_->hide();
    }
}

void ChannelPointsPopup::refreshConnectPage(const ChannelPointsViewState &state)
{
    const auto &auth = state.auth;
    const bool awaiting =
        auth.state == ChannelPointsAuthState::AwaitingActivation;

    this->codeLabel_->setVisible(awaiting);
    this->copyCodeButton_->setVisible(awaiting);
    this->openActivateButton_->setVisible(awaiting);
    this->codeHint_->setVisible(awaiting);
    this->cancelLoginButton_->setVisible(
        awaiting || auth.state == ChannelPointsAuthState::RequestingCode);
    this->connectButton_->setVisible(auth.state ==
                                     ChannelPointsAuthState::SignedOut);
    this->connectButton_->setEnabled(true);

    switch (auth.state)
    {
        case ChannelPointsAuthState::NoChatterinoAccount:
            this->connectTitle_->setText("Not logged in");
            this->connectText_->setText(
                "Log in to Chatterino with your Twitch account to use channel "
                "points.");
            break;

        case ChannelPointsAuthState::SignedOut: {
            this->connectTitle_->setText("Connect channel points");
            QString text =
                "Redeeming rewards needs a separate sign-in through Twitch's "
                "TV login. Chatterino's own login can't redeem rewards.";
            if (!auth.error.isEmpty())
            {
                text = auth.error + "\n\n" + text;
            }
            this->connectText_->setText(text);
        }
        break;

        case ChannelPointsAuthState::RequestingCode:
            this->connectTitle_->setText("Connect channel points");
            this->connectText_->setText("Getting a code from Twitch…");
            break;

        case ChannelPointsAuthState::AwaitingActivation: {
            this->connectTitle_->setText("Enter this code on Twitch");
            this->connectText_->setText(
                "Open twitch.tv/activate, sign in as the account you use in "
                "Chatterino and enter:");
            this->codeLabel_->setText(auth.userCode);
            this->verificationUri_ = auth.verificationUri;
            QString hint =
                QStringLiteral("Waiting for you to confirm on Twitch…");
            if (auth.codeExpiresAt.isValid())
            {
                hint += QStringLiteral(" Code expires in %1.")
                            .arg(fmt::timeUntil(auth.codeExpiresAt));
            }
            this->codeHint_->setText(hint);
        }
        break;

        default:
            break;
    }
}

void ChannelPointsPopup::refreshList(const ChannelPointsViewState &state)
{
    enum Section { Favorites, Available, Unavailable };

    struct Entry {
        const ChannelPointRewardView *view;
        Section section;
    };

    std::vector<Entry> entries;
    entries.reserve(size_t(state.rewards.size()));
    QSet<QString> ids;
    for (const auto &view : state.rewards)
    {
        ids.insert(view.reward.id);
        Section section = Available;
        if (this->isFavorite(view.reward.id))
        {
            section = Favorites;
        }
        else if (isLongTermUnavailable(view.block) &&
                 view.queueMode == ChannelPointQueueMode::None &&
                 !view.inFlight)
        {
            section = Unavailable;
        }
        entries.push_back({&view, section});
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.section != b.section)
                  {
                      return a.section < b.section;
                  }
                  if (a.view->reward.cost != b.view->reward.cost)
                  {
                      return a.view->reward.cost < b.view->reward.cost;
                  }
                  return a.view->reward.title.compare(
                             b.view->reward.title, Qt::CaseInsensitive) < 0;
              });

    // Drop rows of rewards that are gone.
    for (auto it = this->rows_.begin(); it != this->rows_.end();)
    {
        if (ids.contains(it.key()))
        {
            ++it;
            continue;
        }
        it.value()->deleteLater();
        it = this->rows_.erase(it);
    }

    const auto filter = this->searchEdit_->text().trimmed();
    QStringList order;
    int visibleCount[3] = {0, 0, 0};
    for (const auto &entry : entries)
    {
        const auto &id = entry.view->reward.id;
        auto *&row = this->rows_[id];
        if (row == nullptr)
        {
            row = new ChannelPointsRewardRow(
                this->scrollArea_->widget(),
                [this](const ChannelPointRewardView &view) {
                    this->handleRowAction(view);
                },
                [this](const QString &rewardId) {
                    this->toggleFavorite(rewardId);
                });
        }
        row->update(*entry.view, state.balance,
                    entry.section == Favorites);
        const bool visible = row->matches(filter);
        row->setVisible(visible);
        if (visible)
        {
            visibleCount[entry.section]++;
        }
        order.append(QString::number(entry.section) + id);
    }

    // Only touch the layout when the order actually changed.
    if (order != this->currentOrder_)
    {
        this->currentOrder_ = order;

        while (this->listLayout_->count() > 0)
        {
            delete this->listLayout_->takeAt(0);
        }

        int lastSection = -1;
        for (const auto &entry : entries)
        {
            if (entry.section != lastSection)
            {
                lastSection = entry.section;
                this->listLayout_->addWidget(
                    lastSection == Favorites    ? this->favoritesHeader_
                    : lastSection == Available ? this->availableHeader_
                                               : this->unavailableHeader_);
            }
            this->listLayout_->addWidget(this->rows_.value(entry.view->reward.id));
        }
        this->listLayout_->addWidget(this->noMatchesLabel_);
        this->listLayout_->addStretch(1);
    }

    this->favoritesHeader_->setVisible(visibleCount[Favorites] > 0);
    // With no favorites a "Rewards" header is just noise.
    this->availableHeader_->setVisible(
        visibleCount[Available] > 0 &&
        (visibleCount[Favorites] > 0 || visibleCount[Unavailable] > 0));
    this->unavailableHeader_->setVisible(visibleCount[Unavailable] > 0);
    this->noMatchesLabel_->setVisible(visibleCount[Favorites] +
                                          visibleCount[Available] +
                                          visibleCount[Unavailable] ==
                                      0);
}

void ChannelPointsPopup::refreshFooter(const ChannelPointsViewState &state)
{
    const bool signedIn = state.auth.state == ChannelPointsAuthState::SignedIn;
    const auto text = this->palette().color(QPalette::WindowText);

    if (signedIn)
    {
        this->liveLabel_->setText(state.liveUpdatesConnected
                                      ? QStringLiteral("● Live")
                                      : QStringLiteral("○ Live updates off"));
        setCss(this->liveLabel_,
            QStringLiteral("color: %1")
                .arg(colorCss(state.liveUpdatesConnected ? SUCCESS_COLOR
                                                         : dimmed(text, 140))));
        this->liveLabel_->setToolTip(
            state.liveUpdatesConnected
                ? "Balance and rewards update as they change on Twitch"
                : "Not connected to Twitch's live updates. Rewards are "
                  "reloaded periodically instead.");
        this->liveLabel_->show();
    }
    else
    {
        this->liveLabel_->hide();
    }

    if (signedIn && state.loading)
    {
        this->updatedLabel_->setText(QStringLiteral("· updating…"));
        this->updatedLabel_->show();
    }
    else if (signedIn && state.lastLoadedAt.isValid())
    {
        const auto secs =
            state.lastLoadedAt.secsTo(QDateTime::currentDateTimeUtc());
        this->updatedLabel_->setText(
            secs < 5 ? QStringLiteral("· updated just now")
                     : QStringLiteral("· updated %1 ago").arg(fmt::duration(secs)));
        this->updatedLabel_->setToolTip(
            "Rewards were last fully loaded at " +
            state.lastLoadedAt.toLocalTime().toString("HH:mm:ss"));
        this->updatedLabel_->show();
    }
    else
    {
        this->updatedLabel_->hide();
    }

    this->stopAllButton_->setText(
        QStringLiteral("Stop all (%1)").arg(state.queuedCount));
    this->stopAllButton_->setVisible(state.queuedCount > 0);
}

void ChannelPointsPopup::handleRowAction(const ChannelPointRewardView &view)
{
    if (view.inFlight)
    {
        return;
    }
    if (view.queueMode != ChannelPointQueueMode::None)
    {
        getApp()->getChannelPoints()->cancelQueue(this->channelLogin_,
                                                  view.reward.id);
        return;
    }
    this->openConfirmDialog(view);
}

void ChannelPointsPopup::openConfirmDialog(const ChannelPointRewardView &view)
{
    if (!this->confirmDialog_.isNull())
    {
        this->confirmDialog_->close();
    }

    const auto mode = view.block == RewardBlock::None
                          ? ChannelPointQueueMode::None
                          : ChannelPointQueueMode::Once;
    auto *dialog = new ChannelPointsConfirmDialog(
        this->channelLabel_->text(), view, this->balance_, mode, this);
    this->confirmDialog_ = dialog;

    QObject::connect(
        dialog, &ChannelPointsConfirmDialog::confirmed, this,
        [this](const QString &rewardId, ChannelPointQueueMode mode,
               const QString &inputText, int cost) {
            auto *controller = getApp()->getChannelPoints();
            if (mode == ChannelPointQueueMode::None)
            {
                controller->redeemNow(this->channelLogin_, rewardId, inputText,
                                      cost);
            }
            else
            {
                controller->queueReward(this->channelLogin_, rewardId, mode,
                                        inputText, cost);
            }
        });

    widgets::showAndMoveWindowTo(dialog, this->mapToGlobal(QPoint{30, 60}),
                                 widgets::BoundsChecking::CursorPosition);
}

bool ChannelPointsPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->searchEdit_ && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        // Esc clears the search first, the next one closes the popup.
        if (keyEvent->key() == Qt::Key_Escape &&
            !this->searchEdit_->text().isEmpty())
        {
            this->searchEdit_->clear();
            return true;
        }
    }
    return BasePopup::eventFilter(watched, event);
}

bool ChannelPointsPopup::isFavorite(const QString &rewardId) const
{
    const auto favorites = getSettings()->channelPointFavorites.getValue();
    return std::find(favorites.begin(), favorites.end(),
                     favoriteKey(this->channelLogin_, rewardId)) !=
           favorites.end();
}

void ChannelPointsPopup::toggleFavorite(const QString &rewardId)
{
    auto favorites = getSettings()->channelPointFavorites.getValue();
    const auto key = favoriteKey(this->channelLogin_, rewardId);
    auto it = std::find(favorites.begin(), favorites.end(), key);
    if (it == favorites.end())
    {
        favorites.push_back(key);
    }
    else
    {
        favorites.erase(it);
    }
    getSettings()->channelPointFavorites.setValue(favorites);
    this->refresh();
}

void ChannelPointsPopup::attachToController()
{
    if (this->attached_ || this->channelLogin_.isEmpty())
    {
        return;
    }

    this->attached_ = true;
    getApp()->getChannelPoints()->watchChannel(this->channelLogin_);
}

void ChannelPointsPopup::detachFromController()
{
    if (!this->attached_ || this->channelLogin_.isEmpty())
    {
        return;
    }

    this->attached_ = false;
    getApp()->getChannelPoints()->unwatchChannel(this->channelLogin_);
}

}  // namespace chatterino
