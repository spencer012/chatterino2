// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/filters/lang/Filter.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "controllers/highlights/types/AnyHighlight.hpp"
#include "controllers/highlights/types/Common.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "controllers/mcp/McpChat.hpp"
#include "controllers/mcp/McpConfig.hpp"
#include "controllers/mcp/McpJson.hpp"
#include "controllers/mcp/McpTool.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "messages/MessageParseArgs.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"

#include <QColor>

#include <algorithm>
#include <map>

namespace chatterino::mcp {

using namespace Qt::StringLiterals;

namespace {

/// Default and maximum number of messages per channel a simulation looks at
constexpr int DEFAULT_SCAN = 5000;
constexpr int MAX_SCAN = 50000;

struct NamedCheck {
    QString name;
    HighlightCheck check;
};

/// A set of highlights and ignore phrases to evaluate messages against
struct Config {
    std::vector<HighlightCheck> checks;
    std::vector<NamedCheck> namedChecks;
    std::vector<IgnorePhrase> ignores;
};

Config buildConfig(const std::vector<highlights::AnyHighlight> &highlights,
                   std::vector<IgnorePhrase> ignores)
{
    Config config;
    HighlightController::buildChecks(highlights, config.checks);
    for (const auto &highlight : highlights)
    {
        std::vector<HighlightCheck> one;
        HighlightController::buildChecks({highlight}, one);
        if (!one.empty())
        {
            auto name = highlights::getName(highlight);
            if (name.isEmpty())
            {
                name = highlights::getID(highlight).toString();
            }
            config.namedChecks.push_back({name, std::move(one.front())});
        }
    }
    config.ignores = std::move(ignores);
    return config;
}

Config currentConfig()
{
    return buildConfig(*getSettings()->sharedHighlights.readOnly(),
                       *getSettings()->ignoredMessages.readOnly());
}

/// Builds the proposed configuration from the tool arguments: either full
/// replacement lists, or changes on top of the current settings
std::variant<Config, QString> proposedConfig(const QJsonObject &a)
{
    std::vector<highlights::AnyHighlight> highlightList;
    if (a.contains("highlights"))
    {
        for (const auto &v : a.value("highlights").toArray())
        {
            auto parsed = parseHighlight(v.toObject(), nullptr);
            if (auto *error = std::get_if<QString>(&parsed))
            {
                // Built-in highlights are fine in a full list
                auto builtIn =
                    fromJson<highlights::AnyHighlight>(v.toObject());
                if (!builtIn || highlights::isUserDefined(*builtIn) ||
                    std::holds_alternative<highlights::InvalidHighlight>(
                        *builtIn))
                {
                    return u"highlights: %1"_s.arg(*error);
                }
                highlightList.push_back(std::move(*builtIn));
                continue;
            }
            highlightList.push_back(
                std::get<highlights::AnyHighlight>(std::move(parsed)));
        }
    }
    else
    {
        highlightList = *getSettings()->sharedHighlights.readOnly();

        for (const auto &v : a.value("update_highlights").toArray())
        {
            auto update = v.toObject();
            auto id = update.value("id").toString();
            auto it = std::ranges::find_if(highlightList, [&](const auto &h) {
                return highlights::getID(h) == id;
            });
            if (it == highlightList.end())
            {
                return u"update_highlights: no highlight with id \"%1\""_s.arg(
                    id);
            }
            auto parsed = parseHighlight(update.value("value").toObject(), &*it);
            if (auto *error = std::get_if<QString>(&parsed))
            {
                return u"update_highlights: %1"_s.arg(*error);
            }
            *it = std::get<highlights::AnyHighlight>(std::move(parsed));
        }

        for (const auto &v : a.value("remove_highlight_ids").toArray())
        {
            auto id = v.toString();
            auto removed = std::erase_if(highlightList, [&](const auto &h) {
                return highlights::getID(h) == id;
            });
            if (removed == 0)
            {
                return u"remove_highlight_ids: no highlight with id \"%1\""_s
                    .arg(id);
            }
        }

        for (const auto &v : a.value("add_highlights").toArray())
        {
            auto parsed = parseHighlight(v.toObject(), nullptr);
            if (auto *error = std::get_if<QString>(&parsed))
            {
                return u"add_highlights: %1"_s.arg(*error);
            }
            highlightList.push_back(
                std::get<highlights::AnyHighlight>(std::move(parsed)));
        }
    }

    std::vector<IgnorePhrase> ignoreList;
    if (a.contains("ignores"))
    {
        for (const auto &v : a.value("ignores").toArray())
        {
            auto parsed = parseIgnorePhrase(v.toObject());
            if (auto *error = std::get_if<QString>(&parsed))
            {
                return u"ignores: %1"_s.arg(*error);
            }
            ignoreList.push_back(std::get<IgnorePhrase>(std::move(parsed)));
        }
    }
    else
    {
        ignoreList = *getSettings()->ignoredMessages.readOnly();

        std::vector<int> remove;
        for (const auto &v : a.value("remove_ignore_indexes").toArray())
        {
            auto index = v.toInt(-1);
            if (index < 0 || index >= static_cast<int>(ignoreList.size()))
            {
                return u"remove_ignore_indexes: %1 is out of range"_s.arg(
                    index);
            }
            remove.push_back(index);
        }
        std::ranges::sort(remove, std::greater<>());
        auto [first, last] = std::ranges::unique(remove);
        remove.erase(first, last);
        for (auto index : remove)
        {
            ignoreList.erase(ignoreList.begin() + index);
        }

        for (const auto &v : a.value("add_ignores").toArray())
        {
            auto parsed = parseIgnorePhrase(v.toObject());
            if (auto *error = std::get_if<QString>(&parsed))
            {
                return u"add_ignores: %1"_s.arg(*error);
            }
            ignoreList.push_back(std::get<IgnorePhrase>(std::move(parsed)));
        }
    }

    return buildConfig(highlightList, std::move(ignoreList));
}

/// What would happen to a message
struct Outcome {
    bool blocked = false;
    bool highlighted = false;
    bool showInMentions = false;
    bool alert = false;
    bool sound = false;
    std::optional<QColor> color;
    QStringList matchedHighlights;
    QStringList suppressedBy;

    bool sameEffect(const Outcome &other) const
    {
        return this->blocked == other.blocked &&
               this->highlighted == other.highlighted &&
               this->showInMentions == other.showInMentions &&
               this->color == other.color;
    }

    QJsonObject toJson() const
    {
        QJsonObject o;
        if (this->blocked)
        {
            o.insert("blocked", true);
            return o;
        }
        o.insert("highlighted", this->highlighted);
        if (!this->highlighted)
        {
            return o;
        }
        o.insert("showInMentions", this->showInMentions);
        o.insert("color", this->color ? this->color->name(QColor::HexArgb)
                                      : QJsonValue());
        o.insert("alert", this->alert);
        o.insert("sound", this->sound);
        o.insert("matchedHighlights",
                 QJsonArray::fromStringList(this->matchedHighlights));
        if (!this->suppressedBy.isEmpty())
        {
            o.insert("suppressedBy",
                     QJsonArray::fromStringList(this->suppressedBy));
        }
        return o;
    }
};

/// Copies what highlight checks and filters look at, with the flags that
/// highlighting itself sets cleared, so the copy looks like the message did
/// before it was highlighted.
std::shared_ptr<Message> preHighlightCopy(const Message &message)
{
    auto copy = std::make_shared<Message>();
    copy->flags = message.flags;
    copy->flags.unset(MessageFlag::Highlighted);
    copy->flags.unset(MessageFlag::ShowInMentions);
    copy->flags.unset(MessageFlag::DoNotShowInMentions);
    copy->flags.unset(MessageFlag::DisableHighlightColor);
    copy->id = message.id;
    copy->messageText = message.messageText;
    copy->searchText = message.searchText;
    copy->loginName = message.loginName;
    copy->displayName = message.displayName;
    copy->localizedName = message.localizedName;
    copy->userID = message.userID;
    copy->channelName = message.channelName;
    copy->usernameColor = message.usernameColor;
    copy->timestamp = message.timestamp;
    copy->twitchBadges = message.twitchBadges;
    copy->twitchBadgeInfos = message.twitchBadgeInfos;
    copy->externalBadges = message.externalBadges;
    copy->reward = message.reward;
    copy->bits = message.bits;
    return copy;
}

/// Mirrors MessageBuilder::makeIrcMessage: blocking ignore phrases,
/// highlight-only ignore phrases, then highlights.
Outcome evaluate(const Config &config, const Message &message,
                 Channel *channel, bool withNames)
{
    Outcome outcome;

    bool suppressMentions = false;
    bool suppressHighlight = false;
    for (const auto &phrase : config.ignores)
    {
        if (!phrase.isBlock() || !phrase.isMatch(message.messageText))
        {
            continue;
        }
        if (!phrase.highlightOnly())
        {
            outcome.blocked = true;
            return outcome;
        }
        if (phrase.suppressMentions() || phrase.suppressHighlight())
        {
            suppressMentions |= phrase.suppressMentions();
            suppressHighlight |= phrase.suppressHighlight();
            if (withNames)
            {
                outcome.suppressedBy.append(phrase.getPattern());
            }
        }
    }

    if (getSettings()->isBlacklistedUser(message.loginName))
    {
        return outcome;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    bool self = message.loginName.compare(currentUser->getUserName(),
                                          Qt::CaseInsensitive) == 0;
    bool whisper = message.flags.has(MessageFlag::Whisper);
    MessageParseArgs args{
        .isReceivedWhisper = whisper && !self,
        .isSentWhisper = whisper && self,
        .isSubscriptionMessage = message.flags.has(MessageFlag::Subscription),
        .isAction = message.flags.has(MessageFlag::Action),
        .channelPointRewardId =
            message.reward ? message.reward->id : QString(),
    };
    HighlightCheckParams params{
        .args = args,
        .twitchBadges = message.twitchBadges,
        .senderName = message.loginName,
        .originalMessage = message.messageText,
        .messageFlags = message.flags,
        .self = self,
        .runContext =
            filters::RunContext{
                .message = message,
                .channel = channel,
            },
    };

    auto [highlighted, result] =
        HighlightController::runChecks(config.checks, params);
    if (!highlighted)
    {
        return outcome;
    }

    outcome.highlighted = true;
    outcome.alert = result.alert;
    outcome.sound = result.sound.isValid();
    outcome.showInMentions = result.showInMentions && !suppressMentions;
    if (result.color && result.color->isValid() && !suppressHighlight)
    {
        outcome.color = *result.color;
    }
    if (withNames)
    {
        for (const auto &named : config.namedChecks)
        {
            if (named.check(params))
            {
                outcome.matchedHighlights.append(named.name);
            }
        }
    }
    return outcome;
}

/// Calls `fn(message, channel)` for the newest `scan` user messages of each
/// channel
void forEachUserMessage(
    const std::vector<ChannelPtr> &channels, int scan, const QString &user,
    const std::function<void(const MessagePtr &, Channel *)> &fn)
{
    for (const auto &channel : channels)
    {
        auto snapshot = channel->getMessageSnapshot();
        std::vector<MessagePtr> messages;
        for (const auto &m : snapshot)
        {
            if (!isUserMessage(*m))
            {
                continue;
            }
            if (!user.isEmpty() &&
                m->loginName.compare(user, Qt::CaseInsensitive) != 0 &&
                m->displayName.compare(user, Qt::CaseInsensitive) != 0)
            {
                continue;
            }
            messages.push_back(m);
        }
        auto first = std::max<qsizetype>(
            0, static_cast<qsizetype>(messages.size()) - scan);
        for (auto i = first; i < static_cast<qsizetype>(messages.size()); ++i)
        {
            fn(messages[i], channel.get());
        }
    }
}

QJsonObject proposalProperties()
{
    auto object = schema::anyObject("Value in list_config format");
    return {
        {"highlights",
         schema::array("Complete proposed highlight list (replaces the "
                       "current one). Use either this or the add/update/"
                       "remove fields.",
                       object)},
        {"add_highlights",
         schema::array("Highlights to add after the current ones", object)},
        {"update_highlights",
         schema::array(
             "Changed highlights",
             schema::object({{"id", schema::string("Highlight id")},
                             {"value", object}},
                            {"id", "value"}))},
        {"remove_highlight_ids",
         schema::array("Ids of highlights to drop", schema::string("Id"))},
        {"ignores",
         schema::array("Complete proposed ignore list (replaces the current "
                       "one). Use either this or the add/remove fields.",
                       object)},
        {"add_ignores", schema::array("Ignore phrases to add", object)},
        {"remove_ignore_indexes",
         schema::array("Indexes (from list_config) of ignore phrases to drop",
                       schema::integer("Index"))},
    };
}

}  // namespace

std::vector<Tool> makeSimulationTools()
{
    std::vector<Tool> tools;

    tools.push_back({
        .name = "test_filter",
        .description =
            "Run a filter expression against the user messages loaded in "
            "memory and return how many match, who sent them and the newest "
            "matches. Messages are evaluated as they were before "
            "highlighting, so flags.highlighted is false for all of them.",
        .inputSchema = schema::object(
            {
                {"filter", schema::string("Filter expression")},
                {"channel",
                 schema::string("Channel name. Default: all open channels.")},
                {"user", schema::string("Only test messages from this user")},
                {"scan",
                 schema::integer("Newest messages per channel to test. "
                                 "Default 5000.")},
                {"show",
                 schema::integer("How many matching messages to return. "
                                 "Default 30.")},
                {"show_non_matching",
                 schema::boolean("Return non-matching messages instead. "
                                 "Useful together with \"user\" to find spam "
                                 "the filter misses.")},
            },
            {"filter"}),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto res =
                    filters::Filter::fromString(args::string(a, "filter"));
                if (auto *error = std::get_if<filters::FilterError>(&res))
                {
                    done(ToolResult::fail(
                        u"Invalid filter: %1"_s.arg(error->message)));
                    return;
                }
                const auto &filter = std::get<filters::Filter>(res);

                auto channelName = args::string(a, "channel");
                auto channels = channelsFor(channelName);
                if (!channelName.isEmpty() && channels.empty())
                {
                    done(ToolResult::fail(
                        u"Channel \"%1\" is not open"_s.arg(channelName)));
                    return;
                }
                auto scan = std::clamp(args::integer(a, "scan", DEFAULT_SCAN),
                                       1, MAX_SCAN);
                auto show = std::clamp(args::integer(a, "show", 30), 0, 500);
                auto nonMatching = args::boolean(a, "show_non_matching", false);

                qsizetype scanned = 0;
                qsizetype matched = 0;
                std::map<QString, int> byUser;
                std::vector<MessagePtr> shown;
                forEachUserMessage(
                    channels, scan, args::string(a, "user"),
                    [&](const MessagePtr &m, Channel *channel) {
                        ++scanned;
                        auto copy = preHighlightCopy(*m);
                        bool isMatch =
                            filter
                                .execute({.message = *copy, .channel = channel})
                                .toBool();
                        if (isMatch)
                        {
                            ++matched;
                            ++byUser[m->loginName];
                        }
                        if (isMatch != nonMatching)
                        {
                            shown.push_back(m);
                        }
                    });

                std::vector<std::pair<QString, int>> users(byUser.begin(),
                                                           byUser.end());
                std::ranges::sort(users, [](const auto &l, const auto &r) {
                    return l.second > r.second;
                });
                QJsonArray topUsers;
                for (size_t i = 0; i < users.size() && i < 25; ++i)
                {
                    topUsers.append(QJsonObject{
                        {"user", users[i].first},
                        {"matches", users[i].second},
                    });
                }
                QJsonArray messages;
                auto first = std::max<qsizetype>(
                    0, static_cast<qsizetype>(shown.size()) - show);
                for (auto i = first; i < static_cast<qsizetype>(shown.size());
                     ++i)
                {
                    messages.append(messageToJson(*shown[i]));
                }

                done(ToolResult::ok(QJsonObject{
                    {"scanned", scanned},
                    {"matched", matched},
                    {"topUsers", topUsers},
                    {nonMatching ? "nonMatchingMessages" : "matchingMessages",
                     messages},
                }));
            },
    });

    QJsonObject simulateProperties = proposalProperties();
    simulateProperties.insert(
        "channel",
        schema::string("Channel name. Default: all open channels."));
    simulateProperties.insert(
        "user", schema::string("Only look at messages from this user"));
    simulateProperties.insert(
        "scan", schema::integer("Newest messages per channel to compare. "
                                "Default 5000."));
    simulateProperties.insert(
        "show", schema::integer("How many changed messages to return. "
                                "Default 50."));

    tools.push_back({
        .name = "simulate_config",
        .description =
            "Dry run: compare what the current highlights and ignores do to "
            "the loaded messages with what a proposed setup would do. Nothing "
            "is saved. Returns counts of changed outcomes and examples of "
            "messages whose outcome changes (blocked, highlighted, shown in "
            "mentions, color). Run it before applying a change and expect "
            "\"changed\" to be 0 for a pure refactor.\n\n\"currentMismatch\" "
            "counts messages where the simulated current outcome differs "
            "from what actually happened; it is normally 0, but settings "
            "changed after a message arrived, or highlights that depend on "
            "state at that time (e.g. channel.live), show up there.\n\n"
            "Limits: messages hidden by the current blocking ignores were "
            "never stored, so they can't be compared; use check_message for "
            "those.",
        .inputSchema = schema::object(simulateProperties),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto proposed = proposedConfig(a);
                if (auto *error = std::get_if<QString>(&proposed))
                {
                    done(ToolResult::fail(*error));
                    return;
                }
                auto &after = std::get<Config>(proposed);
                auto before = currentConfig();

                auto channelName = args::string(a, "channel");
                auto channels = channelsFor(channelName);
                if (!channelName.isEmpty() && channels.empty())
                {
                    done(ToolResult::fail(
                        u"Channel \"%1\" is not open"_s.arg(channelName)));
                    return;
                }
                auto scan = std::clamp(args::integer(a, "scan", DEFAULT_SCAN),
                                       1, MAX_SCAN);
                auto show = std::clamp(args::integer(a, "show", 50), 0, 500);

                qsizetype scanned = 0;
                qsizetype changed = 0;
                qsizetype currentMismatch = 0;
                QJsonObject counts{
                    {"newlyHighlighted", 0},
                    {"noLongerHighlighted", 0},
                    {"newlyBlocked", 0},
                    {"mentionsChanged", 0},
                    {"colorChanged", 0},
                };
                auto bump = [&counts](const char *key) {
                    counts[key] = counts[key].toInt() + 1;
                };
                QJsonArray examples;
                QJsonArray mismatchExamples;

                forEachUserMessage(
                    channels, scan, args::string(a, "user"),
                    [&](const MessagePtr &m, Channel *channel) {
                        ++scanned;
                        auto copy = preHighlightCopy(*m);
                        auto was = evaluate(before, *copy, channel, false);
                        auto will = evaluate(after, *copy, channel, false);

                        bool actuallyHighlighted =
                            m->flags.has(MessageFlag::Highlighted);
                        if (was.highlighted != actuallyHighlighted)
                        {
                            ++currentMismatch;
                            if (mismatchExamples.size() < 10)
                            {
                                mismatchExamples.append(messageToJson(*m));
                            }
                        }

                        if (was.sameEffect(will))
                        {
                            return;
                        }
                        ++changed;
                        if (will.blocked && !was.blocked)
                        {
                            bump("newlyBlocked");
                        }
                        else if (will.highlighted && !was.highlighted)
                        {
                            bump("newlyHighlighted");
                        }
                        else if (!will.highlighted && was.highlighted)
                        {
                            bump("noLongerHighlighted");
                        }
                        else
                        {
                            if (will.showInMentions != was.showInMentions)
                            {
                                bump("mentionsChanged");
                            }
                            if (will.color != was.color)
                            {
                                bump("colorChanged");
                            }
                        }
                        if (examples.size() < show)
                        {
                            examples.append(QJsonObject{
                                {"message", messageToJson(*m)},
                                {"before",
                                 evaluate(before, *copy, channel, true)
                                     .toJson()},
                                {"after",
                                 evaluate(after, *copy, channel, true)
                                     .toJson()},
                            });
                        }
                    });

                QJsonObject result{
                    {"scanned", scanned},
                    {"changed", changed},
                    {"counts", counts},
                    {"examples", examples},
                    {"currentMismatch", currentMismatch},
                    {"proposedHighlightChecks",
                     static_cast<qint64>(after.checks.size())},
                    {"proposedIgnorePhrases",
                     static_cast<qint64>(after.ignores.size())},
                    {"currentHighlightChecks",
                     static_cast<qint64>(before.checks.size())},
                    {"currentIgnorePhrases",
                     static_cast<qint64>(before.ignores.size())},
                };
                if (!mismatchExamples.isEmpty())
                {
                    result.insert("currentMismatchExamples", mismatchExamples);
                }
                done(ToolResult::ok(result));
            },
    });

    QJsonObject checkProperties = proposalProperties();
    checkProperties.insert("channel",
                           schema::string("Channel the message is sent in"));
    checkProperties.insert("user", schema::string("Sender login name"));
    checkProperties.insert(
        "display_name",
        schema::string("Sender display name (what author.name compares). "
                       "Default: the login name."));
    checkProperties.insert("text", schema::string("Message text"));
    checkProperties.insert(
        "badges",
        schema::array("Badges, e.g. \"moderator\", \"subscriber/12\"",
                      schema::string("Badge")));
    checkProperties.insert("action",
                           schema::boolean("Sent with /me. Default false."));
    checkProperties.insert(
        "first_message",
        schema::boolean("First message in the channel. Default false."));

    tools.push_back({
        .name = "check_message",
        .description =
            "Dry run for one made-up message: what the current highlights and "
            "ignores do to it, and, if any proposal fields are given, what "
            "the proposed setup would do. Use it for messages that haven't "
            "appeared in loaded chat, such as known spam.",
        .inputSchema = schema::object(checkProperties, {"user", "text"}),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto message = std::make_shared<Message>();
                message->loginName = args::string(a, "user").toLower();
                message->displayName =
                    args::string(a, "display_name", message->loginName);
                message->messageText = args::string(a, "text");
                message->searchText = message->messageText;
                message->timestamp = QDateTime::currentDateTime();
                for (const auto &v : a.value("badges").toArray())
                {
                    auto parts = v.toString().split('/');
                    message->twitchBadges.emplace_back(parts.value(0),
                                                       parts.value(1));
                    if (parts.size() > 1)
                    {
                        message->twitchBadgeInfos[parts.value(0)] =
                            parts.value(1);
                    }
                }
                if (args::boolean(a, "action", false))
                {
                    message->flags.set(MessageFlag::Action);
                }
                if (args::boolean(a, "first_message", false))
                {
                    message->flags.set(MessageFlag::FirstMessage);
                }

                ChannelPtr channel;
                auto channelName = args::string(a, "channel");
                if (!channelName.isEmpty())
                {
                    channel = findChannel(channelName);
                    message->channelName =
                        channel ? channel->getName() : channelName.toLower();
                }

                QJsonObject result{
                    {"current", evaluate(currentConfig(), *message,
                                         channel.get(), true)
                                    .toJson()},
                };

                bool hasProposal = false;
                for (auto it = a.begin(); it != a.end(); ++it)
                {
                    if (proposalProperties().contains(it.key()))
                    {
                        hasProposal = true;
                    }
                }
                if (hasProposal)
                {
                    auto proposed = proposedConfig(a);
                    if (auto *error = std::get_if<QString>(&proposed))
                    {
                        done(ToolResult::fail(*error));
                        return;
                    }
                    result.insert("proposed",
                                  evaluate(std::get<Config>(proposed), *message,
                                           channel.get(), true)
                                      .toJson());
                }
                if (!channelName.isEmpty() && !channel)
                {
                    result.insert("note",
                                  "The channel isn't open, so channel.live "
                                  "and similar are false.");
                }
                done(ToolResult::ok(result));
            },
    });

    return tools;
}

}  // namespace chatterino::mcp
