// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Channel.hpp"

#include "Application.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageSimilarity.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Settings.hpp"
#include "util/ChannelHelpers.hpp"

namespace {

constexpr uint8_t MAX_RECURSION = 64;

struct RecursionGuard {
    constexpr RecursionGuard(uint8_t *count) noexcept
        : count(count)
    {
        assert(*count < MAX_RECURSION);
        *this->count += 1;
    }
    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard(RecursionGuard &&) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;
    RecursionGuard &operator=(RecursionGuard &&) = delete;
    constexpr ~RecursionGuard()
    {
        *this->count -= 1;
    }

    uint8_t *count;
};

}  // namespace

namespace chatterino {

//
// Channel
//
Channel::Channel(const QString &name, Type type,
                 std::optional<size_t> messageLimit)
    : completionModel(new TabCompletionModel(*this, nullptr))
    , lastDate_(QDate::currentDate())
    , name_(name)
    , messages_(messageLimit.value_or(getSettings()->channelScrollbackLimit(
          type == Type::Twitch ? name : QString{})))
    , type_(type)
{
    if (this->isTwitchChannel())
    {
        this->platform_ = "twitch";
    }
    if (!messageLimit)
    {
        auto refreshLimit = [this] {
            this->setMessageLimit(getSettings()->channelScrollbackLimit(
                this->type_ == Type::Twitch ? this->name_ : QString{}));
        };
        getSettings()->scrollbackSplitLimit.connect(
            refreshLimit, this->historySettingsConnections_, false);
        getSettings()->channelScrollbackOverrides.connect(
            refreshLimit, this->historySettingsConnections_, false);
    }
}

Channel::~Channel()
{
    auto *app = tryGetApp();
    if (app && !isAppAboutToQuit() && this->anythingLogged_)
    {
        app->getChatLogger()->closeChannel(this->name_, this->platform_);
    }
}

Channel::Type Channel::getType() const
{
    return this->type_;
}

const QString &Channel::getName() const
{
    return this->name_;
}

const QString &Channel::getDisplayName() const
{
    return this->getName();
}

const QString &Channel::getLocalizedName() const
{
    return this->getName();
}

bool Channel::isTwitchChannel() const
{
    return this->type_ >= Type::Twitch && this->type_ < Type::TwitchEnd;
}

bool Channel::isEmpty() const
{
    return this->name_.isEmpty();
}

bool Channel::hasMessages() const
{
    return !this->messages_.empty();
}

size_t Channel::countMessages() const
{
    return this->messages_.size();
}

size_t Channel::messageLimit() const
{
    return this->messages_.limit();
}

size_t Channel::messageIDIndexSize() const
{
    return this->messageIDIndex_.size();
}

void Channel::setMessageLimit(size_t limit)
{
    if (this->messageLimit() == limit)
    {
        return;
    }
    auto removed = this->messages_.setLimit(limit);
    this->messageIDIndexValid_ = false;
    this->messageIDIndex_.clear();
    for (const auto &message : removed)
    {
        this->messageRemovedFromStart(message);
    }
    this->messageLimitChanged.invoke(this->messageLimit());
}

std::vector<MessagePtr> Channel::getMessageSnapshot() const
{
    return this->messages_.getSnapshot();
}

std::vector<MessagePtr> Channel::getMessageSnapshot(size_t nItems) const
{
    return this->messages_.lastN(nItems);
}

std::vector<MessagePtrMut> Channel::getMessageSnapshotMut(size_t nItems) const
{
    return this->messages_.lastNBy<MessagePtrMut>(nItems, [](const auto &msg) {
        return std::const_pointer_cast<Message>(msg);
    });
}

MessagePtr Channel::getLastMessage() const
{
    auto last = this->messages_.last();
    if (last)
    {
        return *std::move(last);
    }
    return nullptr;
}

void Channel::addMessage(MessagePtr message, MessageContext context,
                         std::optional<MessageFlags> overridingFlags)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    message->freeze();

    MessagePtr deleted;

    if (context == MessageContext::Original && this->getType() != Type::None)
    {
        // Only log original messages
        auto isDoNotLogSet =
            (overridingFlags && overridingFlags->has(MessageFlag::DoNotLog)) ||
            message->flags.has(MessageFlag::DoNotLog);

        if (!isDoNotLogSet)
        {
            // Only log messages where the `DoNotLog` flag is not set
            getApp()->getChatLogger()->addMessage(this->name_, message,
                                                  this->platform_,
                                                  this->getCurrentStreamID());
            this->anythingLogged_ = true;
        }
    }

    if (this->messages_.pushBack(message, deleted))
    {
        if (this->messageIDIndexValid_)
        {
            auto it = this->messageIDIndex_.find(deleted->id);
            if (it != this->messageIDIndex_.end() &&
                it->second.lock() == deleted)
            {
                this->messageIDIndex_.erase(it);
            }
        }
        this->messageRemovedFromStart(deleted);
    }

    if (this->messageIDIndexValid_ && !message->id.isEmpty())
    {
        this->messageIDIndex_[message->id] = message;
    }

    this->messageAppended.invoke(message, overridingFlags);
}

void Channel::addSystemMessage(const QString &contents)
{
    auto msg = makeSystemMessage(contents);
    this->addMessage(msg, MessageContext::Original);
}

void Channel::addSystemMessage(const QString &contents,
                               const QString &timeoutUser)
{
    MessageBuilder builder(systemMessage, contents);
    builder->timeoutUser = timeoutUser;
    this->addMessage(builder.release(), MessageContext::Original);
}

void Channel::addOrReplaceTimeout(MessagePtr message, const QDateTime &now)
{
    const auto user = message->timeoutUser;
    MessagePtr pending;
    addOrReplaceChannelTimeout(
        this->getMessageSnapshot(20), std::move(message), now,
        [this](auto /*idx*/, auto msg, auto replacement) {
            this->replaceMessage(msg, replacement);
        },
        [&pending](auto msg) {
            pending = std::move(msg);
        },
        false);
    this->messages_.forEach([&](const auto &msg) {
        if (msg->loginName == user &&
            msg->flags.hasNone(
                {MessageFlag::ModerationAction, MessageFlag::Whisper}))
        {
            msg->flags.set(MessageFlag::Disabled);
            msg->flags.set(MessageFlag::InvalidReplyTarget);
        }
    });
    if (pending)
    {
        this->addMessage(pending, MessageContext::Original);
    }
}

void Channel::addOrReplaceClearChat(MessagePtr message, const QDateTime &now)
{
    addOrReplaceChannelClear(
        this->getMessageSnapshot(20), std::move(message), now,
        [this](auto /*idx*/, auto msg, auto replacement) {
            this->replaceMessage(msg, replacement);
        },
        [this](auto msg) {
            this->addMessage(msg, MessageContext::Original);
        });
}

void Channel::disableAllMessages()
{
    for (const auto &message : this->getMessageSnapshot())
    {
        if (message->flags.hasAny({MessageFlag::System, MessageFlag::Timeout,
                                   MessageFlag::Whisper}))
        {
            continue;
        }

        message->flags.set(MessageFlag::Disabled);
    }
}

void Channel::addMessagesAtStart(const std::vector<MessagePtr> &_messages)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    for (const auto &msg : _messages)
    {
        msg->freeze();
    }

    std::vector<MessagePtr> addedMessages =
        this->messages_.pushFront(_messages);

    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;

    if (addedMessages.size() != 0)
    {
        this->messagesAddedAtStart.invoke(addedMessages);
    }
}

void Channel::fillInMissingMessages(const std::vector<MessagePtr> &messages)
{
    if (messages.empty())
    {
        return;
    }

    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    for (const auto &msg : messages)
    {
        msg->freeze();
    }

    auto snapshot = this->getMessageSnapshot();
    if (snapshot.size() == 0)
    {
        // There are no messages in this channel yet so we can just insert them
        // at the front in order
        this->messages_.pushFront(messages);
        this->filledInMessages.invoke(messages);
        return;
    }

    std::unordered_set<QString> existingMessageIds;
    existingMessageIds.reserve(snapshot.size());

    // First, collect the ids of every message already present in the channel
    for (const auto &msg : snapshot)
    {
        if (msg->flags.has(MessageFlag::System) || msg->id.isEmpty())
        {
            continue;
        }

        existingMessageIds.insert(msg->id);
    }

    std::vector<MessagePtr> incoming;
    incoming.reserve(messages.size());
    for (const auto &msg : messages)
    {
        if (!msg->id.isEmpty() && !existingMessageIds.insert(msg->id).second)
        {
            continue;
        }
        incoming.push_back(msg);
    }
    if (incoming.empty())
    {
        return;
    }
    std::stable_sort(incoming.begin(), incoming.end(),
                     [](const auto &a, const auto &b) {
                         return a->timestamp < b->timestamp;
                     });
    std::vector<MessagePtr> merged;
    merged.reserve(snapshot.size() + incoming.size());
    size_t next = 0;
    for (const auto &msg : incoming)
    {
        while (next < snapshot.size() &&
               (snapshot[next]->flags.has(MessageFlag::System) ||
                snapshot[next]->timestamp <= msg->timestamp))
        {
            merged.push_back(snapshot[next++]);
        }
        merged.push_back(msg);
    }
    merged.insert(merged.end(), snapshot.begin() + next, snapshot.end());
    this->messages_.replaceAll(merged);
    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;
    this->filledInMessages.invoke(messages);
}

void Channel::replaceMessage(const MessagePtr &message,
                             const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    replacement->freeze();
    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;
    int index = this->messages_.replaceItem(message, replacement);

    if (index >= 0)
    {
        this->messageReplaced.invoke((size_t)index, message, replacement);
    }
}

void Channel::replaceMessage(size_t index, const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    replacement->freeze();

    MessagePtr prev;
    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;
    if (this->messages_.replaceItem(index, replacement, &prev))
    {
        this->messageReplaced.invoke(index, prev, replacement);
    }
}

void Channel::replaceMessage(size_t hint, const MessagePtr &message,
                             const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    replacement->freeze();

    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;
    auto index = this->messages_.replaceItem(hint, message, replacement);
    if (index >= 0)
    {
        this->messageReplaced.invoke(static_cast<size_t>(index), message,
                                     replacement);
    }
}

void Channel::disableMessage(const QString &messageID)
{
    auto msg = this->findMessageByID(messageID);
    if (msg != nullptr)
    {
        msg->flags.set(MessageFlag::Disabled);
    }
}

void Channel::clearMessages()
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    this->messages_.clear();
    this->messageIDIndex_.clear();
    this->messageIDIndexValid_ = false;
    this->messagesCleared.invoke();
}

MessagePtr Channel::findMessageByID(QStringView messageID)
{
    // Small queues are cheaper to scan than to maintain a hash table.
    if (this->messages_.size() >= 10000 && !messageID.isEmpty())
    {
        if (!this->messageIDIndexValid_)
        {
            this->messageIDIndex_.clear();
            this->messages_.forEach([this](const auto &msg) {
                if (!msg->id.isEmpty())
                {
                    this->messageIDIndex_[msg->id] = msg;
                }
            });
            this->messageIDIndexValid_ = true;
        }
        auto it = this->messageIDIndex_.find(messageID.toString());
        return it == this->messageIDIndex_.end() ? nullptr : it->second.lock();
    }
    MessagePtr res;

    if (auto msg = this->messages_.rfind([messageID](const MessagePtr &msg) {
            return msg->id == messageID;
        });
        msg)
    {
        res = *msg;
    }

    return res;
}

void Channel::applySimilarityFilters(const MessagePtr &message) const
{
    if (!getSettings()->similarityEnabled)
    {
        return;
    }
    setSimilarityFlags(
        message,
        this->messages_.lastN(static_cast<size_t>(std::max(
            0, getSettings()->hideSimilarMaxMessagesToCheck.getValue()))));
}

MessageSinkTraits Channel::sinkTraits() const
{
    return {
        MessageSinkTrait::AddMentionsToGlobalChannel,
        MessageSinkTrait::RequiresKnownChannelPointReward,
        MessageSinkTrait::UpdateCurrentUserState,
    };
}

bool Channel::canSendMessage() const
{
    return false;
}

bool Channel::isWritable() const
{
    using Type = Channel::Type;
    auto type = this->getType();
    return type != Type::TwitchMentions && type != Type::TwitchLive &&
           type != Type::TwitchAutomod;
}

void Channel::sendMessage(const QString &message)
{
}

bool Channel::isMod() const
{
    return false;
}

bool Channel::isBroadcaster() const
{
    return false;
}

bool Channel::hasModRights() const
{
    return this->isMod() || this->isBroadcaster();
}

bool Channel::hasHighRateLimit() const
{
    return this->isMod() || this->isBroadcaster();
}

bool Channel::isLive() const
{
    return false;
}

bool Channel::isRerun() const
{
    return false;
}

bool Channel::shouldIgnoreHighlights() const
{
    return this->type_ == Type::TwitchAutomod ||
           this->type_ == Type::TwitchMentions ||
           this->type_ == Type::TwitchWhispers;
}

bool Channel::canReconnect() const
{
    return false;
}

void Channel::reconnect()
{
}

QString Channel::getCurrentStreamID() const
{
    return {};
}

std::shared_ptr<Channel> Channel::getEmpty()
{
    static std::shared_ptr<Channel> channel(new Channel("", Type::None));
    return channel;
}

void Channel::onConnected()
{
}

void Channel::messageRemovedFromStart(const MessagePtr &msg)
{
}

bool Channel::canRecurse() const noexcept
{
    return this->recursionCount_ < MAX_RECURSION;
}

//
// Indirect channel
//
IndirectChannel::Data::Data(ChannelPtr _channel, Channel::Type _type)
    : channel(std::move(_channel))
    , type(_type)
{
}

IndirectChannel::IndirectChannel(ChannelPtr channel, Channel::Type type)
    : data_(std::make_unique<Data>(std::move(channel), type))
{
}

ChannelPtr IndirectChannel::get() const
{
    return this->data_->channel;
}

void IndirectChannel::reset(ChannelPtr channel)
{
    assert(this->data_->type != Channel::Type::Direct);

    this->data_->channel = std::move(channel);
    this->data_->changed.invoke();
}

pajlada::Signals::NoArgSignal &IndirectChannel::getChannelChanged()
{
    return this->data_->changed;
}

Channel::Type IndirectChannel::getType() const
{
    if (this->data_->type == Channel::Type::Direct)
    {
        return this->get()->getType();
    }

    return this->data_->type;
}

}  // namespace chatterino
