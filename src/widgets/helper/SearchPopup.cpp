// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SearchPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/MessageElement.hpp"
#include "messages/search/AuthorPredicate.hpp"
#include "messages/search/BadgePredicate.hpp"
#include "messages/search/ChannelPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "messages/search/SubtierPredicate.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/ChannelHelpers.hpp"
#include "util/GradualRelease.hpp"
#include "util/ParallelFilter.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QtConcurrent>
#include <QThreadPool>

#include <algorithm>
#include <mutex>

namespace chatterino {

struct SearchPopup::Snapshot {
    std::vector<MessagePtr> messages;

    /// Messages from several channels still have to be deduplicated and
    /// sorted. The first search does that on its thread.
    bool needsMerge = false;
    std::once_flag merged;
};

std::vector<MessagePtr> SearchPopup::filter(
    const QString &text, const std::vector<MessagePtr> &snapshot,
    const std::atomic_bool &cancelled)
{
    return parallelFilterRanges<MessagePtr>(
        snapshot.size(), 4096, [&](size_t begin, size_t end) {
            // Predicates keep state (e.g. regexes), so every range parses its
            // own from "text"
            auto predicates = parsePredicates(text);

            std::vector<MessagePtr> matches;
            for (size_t i = begin; i < end; ++i)
            {
                if (i % 1024 == 0 && cancelled.load(std::memory_order_relaxed))
                {
                    return std::vector<MessagePtr>{};
                }

                const auto &message = snapshot[i];

                // Check for every message whether it fulfills all predicates
                // that have been registered
                bool accept =
                    std::ranges::all_of(predicates, [&](const auto &pred) {
                        return pred->appliesTo(*message);
                    });
                if (accept)
                {
                    matches.push_back(message);
                }
            }
            return matches;
        });
}

SearchPopup::SearchPopup(QWidget *parent, Split *split)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    this->initLayout();
    this->searchTimer_.setSingleShot(true);
    this->searchTimer_.setInterval(150);
    connect(&this->searchTimer_, &QTimer::timeout, this, &SearchPopup::search);
    if (this->split_ && this->split_->getChannelView().hasSelection())
    {
        this->searchInput_->setText(
            this->split_->getChannelView().getSelectedText().trimmed());
        this->searchInput_->selectAll();
    }
    this->resize(400, 600);
    this->addShortcuts();

    this->themeChangedEvent();
}

SearchPopup::~SearchPopup()
{
    if (this->cancelSearch_)
    {
        *this->cancelSearch_ = true;
    }

    // Freeing long histories takes a while. A search that's still running
    // keeps the snapshot and frees it once it's done.
    if (this->snapshot_ && this->snapshot_.use_count() == 1)
    {
        releaseGradually(std::move(this->snapshot_->messages));
    }
    // Only the results view (which is destroyed right after this) and this
    // reference know the results
    if (auto results = this->channelView_->underlyingChannel();
        results && results.use_count() == 2)
    {
        releaseGradually(results->takeMessages());
    }
}

void SearchPopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](const std::vector<QString> &) -> QString {
             this->searchInput_->setFocus();
             this->searchInput_->selectAll();
             return "";
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"scrollPage", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void SearchPopup::addChannel(ChannelView &channel)
{
    if (this->searchChannels_.empty())
    {
        this->channelView_->setSourceChannel(channel.underlyingChannel());
        this->channelName_ = channel.underlyingChannel()->getName();
    }
    else if (this->searchChannels_.size() == 1)
    {
        this->channelView_->setSourceChannel(
            std::make_shared<Channel>("multichannel", Channel::Type::None));

        auto flags = this->channelView_->getFlags();
        flags.set(MessageElementFlag::ChannelName);
        flags.unset(MessageElementFlag::ModeratorTools);
        this->channelView_->setOverrideFlags(flags);
    }

    this->searchChannels_.append(std::ref(channel));

    this->updateWindowTitle();
}

void SearchPopup::goToMessage(const MessagePtr &message)
{
    for (const auto &view : this->searchChannels_)
    {
        const auto type = view.get().underlyingChannel()->getType();
        if (type == Channel::Type::TwitchMentions ||
            type == Channel::Type::TwitchAutomod)
        {
            getApp()->getWindows()->scrollToMessage(message);
            return;
        }

        if (view.get().scrollToMessage(message))
        {
            return;
        }
    }
}

void SearchPopup::goToMessageId(const QString &messageId)
{
    for (const auto &view : this->searchChannels_)
    {
        if (view.get().scrollToMessageId(messageId))
        {
            return;
        }
    }
}

void SearchPopup::updateWindowTitle()
{
    QString historyName;

    if (this->searchChannels_.size() > 1)
    {
        historyName = "multiple channels'";
    }
    else if (this->channelName_ == "/automod")
    {
        historyName = "automod";
    }
    else if (this->channelName_ == "/mentions")
    {
        historyName = "mentions";
    }
    else if (this->channelName_ == "/whispers")
    {
        historyName = "whispers";
    }
    else if (this->channelName_.isEmpty())
    {
        historyName = "<empty>'s";
    }
    else
    {
        historyName = QString("%1's").arg(this->channelName_);
    }
    this->setWindowTitle("Searching in " + historyName + " history");
}

void SearchPopup::showEvent(QShowEvent *e)
{
    this->search();
    BaseWindow::showEvent(e);
}

bool SearchPopup::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->searchInput_ && event->type() == QEvent::KeyPress)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent == QKeySequence::DeleteStartOfWord &&
            this->searchInput_->selectionLength() > 0)
        {
            this->searchInput_->backspace();
            return true;
        }
    }
    return false;
}

void SearchPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->setPalette(getTheme()->palette);
}

void SearchPopup::search()
{
    this->searchTimer_.stop();
    if (!this->snapshot_)
    {
        this->snapshot_ = this->buildSnapshot();
    }

    // Searching hundreds of thousands of messages takes a while, so it runs on
    // the thread pool and the results are shown once they're ready
    if (this->cancelSearch_)
    {
        *this->cancelSearch_ = true;
    }
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    this->cancelSearch_ = cancelled;
    auto generation = ++this->searchGeneration_;

    struct Results {
        /// For the channel the results are shown from
        std::vector<MessagePtr> messages;
        ChannelView::PreparedMessages prepared;
    };

    QtConcurrent::run([snapshot = this->snapshot_,
                       text = this->searchInput_->text(), cancelled] {
        std::call_once(snapshot->merged, [&] {
            if (!snapshot->needsMerge)
            {
                return;
            }
            // remove any duplicate messages from splits containing the same
            // channel
            deduplicateMessages(snapshot->messages);

            // resort by time for presentation
            std::ranges::sort(snapshot->messages,
                              [](const MessagePtr &a, const MessagePtr &b) {
                                  return a->timestamp < b->timestamp;
                              });
        });

        auto matches = filter(text, snapshot->messages, *cancelled);
        if (*cancelled)
        {
            return std::shared_ptr<Results>{};
        }

        // Results are shown in a channel of type None, which doesn't ignore
        // highlights. Both channels get their copy here, since copying
        // hundreds of thousands of messages takes a while.
        auto prepared = ChannelView::prepareMessages(matches, false);
        return std::make_shared<Results>(std::move(matches),
                                         std::move(prepared));
    }).then(this, [this, generation](std::shared_ptr<Results> results) {
        if (!results)
        {
            return;
        }

        // Skip results that a newer search (or one about to start) replaces
        if (generation != this->searchGeneration_ ||
            this->searchTimer_.isActive())
        {
            // The layouts were never shown, so they can be freed off the GUI
            // thread
            QThreadPool::globalInstance()->start(
                [results = std::move(results)] {});
            return;
        }

        this->showResults(std::move(results->messages),
                          std::move(results->prepared));
    });
}

void SearchPopup::showResults(std::vector<MessagePtr> messages,
                              ChannelView::PreparedMessages prepared)
{
    ChannelPtr channel(new Channel(this->channelName_, Channel::Type::None,
                                   std::max<size_t>(1, messages.size())));
    channel->resetMessages(std::move(messages));

    auto previous = this->channelView_->underlyingChannel();
    this->channelView_->setChannel(channel, std::move(prepared));

    // Only this popup knew the previous results, and freeing a long history
    // takes a while
    if (previous && previous.use_count() == 1)
    {
        releaseGradually(previous->takeMessages());
    }
}

std::shared_ptr<SearchPopup::Snapshot> SearchPopup::buildSnapshot()
{
    auto snapshot = std::make_shared<Snapshot>();

    // no point in filtering/sorting if it's a single channel search
    if (this->searchChannels_.length() == 1)
    {
        const auto channelPtr = this->searchChannels_.at(0);
        snapshot->messages = channelPtr.get().channel()->getMessageSnapshot();
        return snapshot;
    }

    for (auto &channel : this->searchChannels_)
    {
        ChannelView &sharedView = channel.get();

        const FilterSetPtr filterSet = sharedView.getFilterSet();
        std::vector<MessagePtr> messages =
            sharedView.channel()->getMessageSnapshot();

        for (auto &message : messages)
        {
            if (filterSet &&
                !filterSet->filter(message, sharedView.underlyingChannel()))
            {
                continue;
            }

            snapshot->messages.push_back(std::move(message));
        }
    }
    snapshot->needsMerge = true;

    return snapshot;
}

void SearchPopup::initLayout()
{
    // VBOX
    {
        auto *layout1 = new QVBoxLayout(this);
        layout1->setContentsMargins(0, 0, 0, 0);
        layout1->setSpacing(0);

        // HBOX
        {
            auto *layout2 = new QHBoxLayout();
            layout2->setContentsMargins(8, 8, 8, 8);
            layout2->setSpacing(8);

            // SEARCH INPUT
            {
                this->searchInput_ = new QLineEdit(this);
                layout2->addWidget(this->searchInput_);

                this->searchInput_->setPlaceholderText("Type to search");
                this->searchInput_->setClearButtonEnabled(true);
                this->searchInput_->findChild<QAbstractButton *>()->setIcon(
                    QPixmap(":/buttons/clearSearch.png"));
                QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                                 this, [this] {
                                     this->searchTimer_.start();
                                 });
                this->searchInput_->installEventFilter(this);
            }

            layout1->addLayout(layout2);
        }

        // CHANNELVIEW
        {
            this->channelView_ = new ChannelView(
                this, this->split_, ChannelView::Context::Search,
                getSettings()->scrollbackSplitLimit);

            layout1->addWidget(this->channelView_);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    // This regex captures all name:value predicate pairs into named capturing
    // groups and matches all other inputs seperated by spaces as normal
    // strings.
    // It also ignores whitespaces in values when being surrounded by quotation
    // marks, to enable inputs like this => regex:"kappa 123"
    // Searches parse their query on several threads at once
    static thread_local QRegularExpression predicateRegex(
        R"lit((?<negation>[!\-])?(?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    static thread_local QRegularExpression trimQuotationMarksRegex(R"(^"|"$)");

    QRegularExpressionMatchIterator it = predicateRegex.globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");
        bool isNegated = !match.captured("negation").isEmpty();
        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex);

        // match predicates

        if (name == "from")
        {
            predicates.push_back(
                std::make_unique<AuthorPredicate>(value, isNegated));
        }
        else if (name == "badge")
        {
            predicates.push_back(
                std::make_unique<BadgePredicate>(value, isNegated));
        }
        else if (name == "subtier")
        {
            predicates.push_back(
                std::make_unique<SubtierPredicate>(value, isNegated));
        }
        else if (name == "has" && value == "link")
        {
            predicates.push_back(std::make_unique<LinkPredicate>(isNegated));
        }
        else if (name == "in")
        {
            predicates.push_back(
                std::make_unique<ChannelPredicate>(value, isNegated));
        }
        else if (name == "is")
        {
            predicates.push_back(
                std::make_unique<MessageFlagsPredicate>(value, isNegated));
        }
        else if (name == "regex")
        {
            predicates.push_back(
                std::make_unique<RegexPredicate>(value, isNegated));
        }
        else
        {
            predicates.push_back(
                std::make_unique<SubstringPredicate>(match.captured()));
        }
    }

    return predicates;
}

}  // namespace chatterino
