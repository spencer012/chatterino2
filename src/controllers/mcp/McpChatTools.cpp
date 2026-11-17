// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/mcp/McpChat.hpp"
#include "controllers/mcp/McpTool.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/IvrApi.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"

#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <map>
#include <tuple>

namespace chatterino::mcp {

using namespace Qt::StringLiterals;

namespace {

/// Network lookups for get_user give up after this long
constexpr int USER_LOOKUP_TIMEOUT_MS = 10000;

const std::vector<std::pair<MessageFlag, QString>> &flagNames()
{
    static std::vector<std::pair<MessageFlag, QString>> names{
        {MessageFlag::Highlighted, u"highlighted"_s},
        {MessageFlag::ShowInMentions, u"show_in_mentions"_s},
        {MessageFlag::DoNotShowInMentions, u"suppressed_mentions"_s},
        {MessageFlag::DisableHighlightColor, u"suppressed_highlight_color"_s},
        {MessageFlag::FirstMessage, u"first_message"_s},
        {MessageFlag::Action, u"action"_s},
        {MessageFlag::Subscription, u"subscription"_s},
        {MessageFlag::Whisper, u"whisper"_s},
        {MessageFlag::RedeemedChannelPointReward, u"reward"_s},
        {MessageFlag::RedeemedHighlight, u"redeemed_highlight"_s},
        {MessageFlag::ReplyMessage, u"reply"_s},
        {MessageFlag::CheerMessage, u"cheer"_s},
        {MessageFlag::Announcement, u"announcement"_s},
        {MessageFlag::WatchStreak, u"watch_streak"_s},
        {MessageFlag::AutoMod, u"automod"_s},
        {MessageFlag::RestrictedMessage, u"restricted"_s},
        {MessageFlag::MonitoredMessage, u"monitored"_s},
        {MessageFlag::SharedMessage, u"shared"_s},
        {MessageFlag::Similar, u"similar"_s},
        {MessageFlag::Disabled, u"deleted_or_timed_out"_s},
        {MessageFlag::RecentMessage, u"from_history"_s},
        {MessageFlag::System, u"system"_s},
    };
    return names;
}

QJsonObject channelToJson(const ChannelPtr &channel)
{
    QJsonObject o{
        {"name", channel->getName()},
        {"messages",
         static_cast<qint64>(channel->getMessageSnapshot().size())},
    };
    if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        o.insert("displayName", tc->getDisplayName());
        o.insert("roomId", tc->roomId());
        o.insert("youAreMod", tc->isMod());
        o.insert("youAreBroadcaster", tc->isBroadcaster());
        auto status = tc->accessStreamStatus();
        o.insert("live", status->live);
        if (!status->title.isEmpty())
        {
            o.insert("title", status->title);
        }
        if (!status->game.isEmpty())
        {
            o.insert("game", status->game);
        }
        if (status->live)
        {
            o.insert("viewers", static_cast<int>(status->viewerCount));
            o.insert("uptime", status->uptime);
        }
    }
    return o;
}

bool matchesUser(const Message &message, const QString &user)
{
    return message.loginName.compare(user, Qt::CaseInsensitive) == 0 ||
           message.displayName.compare(user, Qt::CaseInsensitive) == 0;
}

}  // namespace

ChannelPtr findChannel(const QString &rawName)
{
    auto *twitch = getApp()->getTwitch();
    auto name = rawName.trimmed();
    if (name.startsWith('#'))
    {
        name = name.mid(1);
    }
    if (name == u"/mentions")
    {
        return twitch->getMentionsChannel();
    }
    if (name == u"/whispers")
    {
        return twitch->getWhispersChannel();
    }
    if (name == u"/automod")
    {
        return twitch->getAutomodChannel();
    }
    if (name == u"/live")
    {
        return twitch->getLiveChannel();
    }
    auto channel = twitch->getChannelOrEmpty(name);
    if (!channel || channel->isEmpty())
    {
        return nullptr;
    }
    return channel;
}

std::vector<ChannelPtr> channelsFor(const QString &name)
{
    std::vector<ChannelPtr> out;
    if (!name.isEmpty())
    {
        if (auto channel = findChannel(name))
        {
            out.push_back(channel);
        }
        return out;
    }
    getApp()->getTwitch()->forEachChannel([&](const ChannelPtr &channel) {
        out.push_back(channel);
    });
    return out;
}

bool isUserMessage(const Message &message)
{
    return !message.loginName.isEmpty() &&
           !message.flags.has(MessageFlag::System) &&
           !message.flags.has(MessageFlag::Timeout);
}

QJsonObject messageToJson(const Message &message)
{
    QJsonObject o{
        {"id", message.id},
        {"channel", message.channelName},
        {"time", message.timestamp.isValid()
                     ? message.timestamp.toLocalTime().toString(
                           Qt::ISODate)
                     : QString()},
        {"user", message.loginName},
        {"text", message.messageText},
    };
    if (message.displayName.compare(message.loginName, Qt::CaseInsensitive) !=
        0)
    {
        o.insert("displayName", message.displayName);
    }
    if (!message.userID.isEmpty())
    {
        o.insert("userId", message.userID);
    }

    QJsonArray flags;
    for (const auto &[flag, name] : flagNames())
    {
        if (message.flags.has(flag))
        {
            flags.append(name);
        }
    }
    if (!flags.isEmpty())
    {
        o.insert("flags", flags);
    }
    if (message.flags.has(MessageFlag::Highlighted) && message.highlightColor)
    {
        o.insert("highlightColor",
                 message.highlightColor->name(QColor::HexArgb));
    }

    QJsonArray badges;
    for (const auto &badge : message.twitchBadges)
    {
        badges.append(badge.value_.isEmpty()
                          ? badge.key_
                          : u"%1/%2"_s.arg(badge.key_, badge.value_));
    }
    if (!badges.isEmpty())
    {
        o.insert("badges", badges);
    }
    if (message.reward)
    {
        o.insert("reward", message.reward->title);
    }
    return o;
}

QString messageToLine(const Message &message)
{
    QString line = message.timestamp.isValid()
                       ? message.timestamp.toLocalTime().toString(Qt::ISODate)
                       : u"-"_s;
    line += u" #"_s + message.channelName + u' ' + message.loginName;
    if (!message.displayName.isEmpty() &&
        message.displayName.compare(message.loginName, Qt::CaseInsensitive) !=
            0)
    {
        line += u" ("_s + message.displayName + u')';
    }
    QStringList flags;
    for (const auto &[flag, name] : flagNames())
    {
        if (message.flags.has(flag))
        {
            flags.append(name);
        }
    }
    if (message.reward)
    {
        flags.append(u"reward:"_s + message.reward->title);
    }
    if (!flags.isEmpty())
    {
        line += u" ["_s + flags.join(u',') + u']';
    }
    line += u": "_s + message.messageText;
    return line;
}

QJsonObject messageFormatSchema()
{
    return schema::stringEnum(
        "\"json\" (default): one object per message with id, badges and "
        "highlight color. \"lines\": one string per message, \"<time> "
        "#<channel> <login> [flags]: <text>\", about a fifth of the size.",
        {u"json"_s, u"lines"_s});
}

bool wantsLines(const QJsonObject &args)
{
    return args::string(args, "format").compare(u"lines"_s,
                                                Qt::CaseInsensitive) == 0;
}

QJsonValue formatMessage(const Message &message, bool lines)
{
    if (lines)
    {
        return messageToLine(message);
    }
    return messageToJson(message);
}

std::vector<Tool> makeChatTools()
{
    std::vector<Tool> tools;

    tools.push_back({
        .name = "list_channels",
        .description =
            "List the open Twitch channels with how many messages are loaded, "
            "live status and whether you are a moderator there.",
        .inputSchema = schema::object({}),
        .run =
            [](const QJsonObject & /*args*/, const ToolDone &done) {
                QJsonArray channels;
                for (const auto &channel : channelsFor({}))
                {
                    channels.append(channelToJson(channel));
                }
                QJsonArray special;
                for (const auto *name :
                     {"/mentions", "/whispers", "/automod", "/live"})
                {
                    if (auto channel = findChannel(QString::fromLatin1(name)))
                    {
                        special.append(QJsonObject{
                            {"name", name},
                            {"messages", static_cast<qint64>(
                                             channel->getMessageSnapshot()
                                                 .size())},
                        });
                    }
                }
                done(ToolResult::ok(QJsonObject{
                    {"channels", channels},
                    {"special", special},
                }));
            },
    });

    tools.push_back({
        .name = "get_messages",
        .description =
            "Get messages loaded in memory, oldest first. Without "
            "\"channel\", searches every open channel. Filters combine with "
            "AND. Only what Chatterino has loaded is available (recent "
            "history plus what arrived since); messages hidden by blocking "
            "ignore phrases were never stored.",
        .inputSchema = schema::object({
            {"channel",
             schema::string("Channel name, or /mentions, /whispers, "
                            "/automod, /live. Default: all open channels.")},
            {"user", schema::string("Only messages from this login or "
                                    "display name")},
            {"contains",
             schema::string("Only messages containing this text (ignores "
                            "case)")},
            {"regex", schema::string("Only messages matching this regex")},
            {"highlighted",
             schema::boolean("Only highlighted (true) or not highlighted "
                             "(false) messages")},
            {"include_system",
             schema::boolean("Include system messages (timeouts, notices...). "
                             "Default false.")},
            {"limit",
             schema::integer("Maximum messages, newest kept. Default 100, "
                             "max 2000.")},
            {"before_id",
             schema::string("Only messages older than the message with this "
                            "id, for paging back (use \"oldestId\" from the "
                            "previous result)")},
            {"format", messageFormatSchema()},
            {"distinct",
             schema::boolean(
                 "Collapse messages with the same channel, user and text into "
                 "one entry with \"count\" (lines start with \"xN \"). "
                 "\"limit\" then counts distinct entries. Good for spotting "
                 "bot templates and spam bursts. Default false.")},
        }),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto channelName = args::string(a, "channel");
                auto channels = channelsFor(channelName);
                if (!channelName.isEmpty() && channels.empty())
                {
                    done(ToolResult::fail(
                        u"Channel \"%1\" is not open"_s.arg(channelName)));
                    return;
                }

                auto user = args::string(a, "user");
                auto contains = args::string(a, "contains");
                auto regexText = args::string(a, "regex");
                QRegularExpression regex(regexText);
                if (!regexText.isEmpty() && !regex.isValid())
                {
                    done(ToolResult::fail(
                        u"Invalid regex: %1"_s.arg(regex.errorString())));
                    return;
                }
                auto highlighted = a.value("highlighted");
                auto includeSystem = args::boolean(a, "include_system", false);
                auto limit =
                    std::clamp(args::integer(a, "limit", 100), 1, 2000);
                auto beforeId = args::string(a, "before_id");

                std::vector<MessagePtr> matched;
                qsizetype scanned = 0;
                for (const auto &channel : channels)
                {
                    auto snapshot = channel->getMessageSnapshot();
                    auto end = snapshot.end();
                    if (!beforeId.isEmpty())
                    {
                        auto it = std::ranges::find_if(
                            snapshot, [&](const MessagePtr &m) {
                                return m->id == beforeId;
                            });
                        if (it != snapshot.end())
                        {
                            end = it;
                        }
                        else if (channels.size() == 1)
                        {
                            end = snapshot.begin();
                        }
                    }
                    for (auto it = snapshot.begin(); it != end; ++it)
                    {
                        const auto &m = **it;
                        ++scanned;
                        if (!includeSystem && !isUserMessage(m))
                        {
                            continue;
                        }
                        if (!user.isEmpty() && !matchesUser(m, user))
                        {
                            continue;
                        }
                        if (!contains.isEmpty() &&
                            !m.messageText.contains(contains,
                                                    Qt::CaseInsensitive))
                        {
                            continue;
                        }
                        if (!regexText.isEmpty() &&
                            !regex.match(m.messageText).hasMatch())
                        {
                            continue;
                        }
                        if (highlighted.isBool() &&
                            m.flags.has(MessageFlag::Highlighted) !=
                                highlighted.toBool())
                        {
                            continue;
                        }
                        matched.push_back(*it);
                    }
                }

                if (channels.size() > 1)
                {
                    std::ranges::stable_sort(
                        matched, [](const MessagePtr &l, const MessagePtr &r) {
                            return l->timestamp <
                                   r->timestamp;
                        });
                }

                auto lines = wantsLines(a);
                auto total = static_cast<qsizetype>(matched.size());

                if (args::boolean(a, "distinct", false))
                {
                    // Grouped in order of first appearance
                    struct Group {
                        MessagePtr first;
                        MessagePtr last;
                        int count = 0;
                    };
                    std::vector<Group> groups;
                    std::map<std::tuple<QString, QString, QString>, size_t>
                        index;
                    for (const auto &m : matched)
                    {
                        auto key = std::make_tuple(m->channelName,
                                                   m->loginName.toLower(),
                                                   m->messageText);
                        auto [it, inserted] = index.try_emplace(
                            std::move(key), groups.size());
                        if (inserted)
                        {
                            groups.push_back({.first = m, .last = m});
                        }
                        auto &group = groups[it->second];
                        group.last = m;
                        ++group.count;
                    }
                    auto groupCount = static_cast<qsizetype>(groups.size());
                    auto first = std::max<qsizetype>(0, groupCount - limit);
                    QJsonArray out;
                    for (auto i = first; i < groupCount; ++i)
                    {
                        const auto &group = groups[i];
                        if (lines)
                        {
                            auto line = messageToLine(*group.first);
                            out.append(group.count > 1
                                           ? u"x%1 "_s.arg(group.count) + line
                                           : line);
                            continue;
                        }
                        auto o = messageToJson(*group.last);
                        o.insert("count", group.count);
                        if (group.count > 1)
                        {
                            o.insert("firstTime",
                                     group.first->timestamp.toLocalTime()
                                         .toString(Qt::ISODate));
                        }
                        out.append(o);
                    }
                    done(ToolResult::ok(QJsonObject{
                        {"scanned", scanned},
                        {"matched", total},
                        {"distinct", groupCount},
                        {"returned", out.size()},
                        {"truncated", first > 0},
                        {"messages", out},
                    }));
                    return;
                }

                auto first = std::max<qsizetype>(0, total - limit);
                QJsonArray out;
                for (auto i = first; i < total; ++i)
                {
                    out.append(formatMessage(*matched[i], lines));
                }
                QJsonObject result{
                    {"scanned", scanned},
                    {"matched", total},
                    {"returned", out.size()},
                    {"truncated", first > 0},
                    {"messages", out},
                };
                if (first < total)
                {
                    // For paging back with before_id, which "lines" omits
                    result.insert("oldestId", matched[first]->id);
                }
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "get_user",
        .description =
            "Everything Chatterino knows about a user: their loaded messages "
            "per channel, badges, your notes and custom color for them, "
            "nickname, whether they are blocked or on the highlight "
            "blacklist. With \"remote\" (default true) it also fetches what "
            "the user card shows: account age, description, follower count, "
            "live status, follow age in \"channel\" (only where you are a "
            "moderator) and sub status in \"channel\".",
        .inputSchema = schema::object(
            {
                {"user", schema::string("Login or display name")},
                {"channel",
                 schema::string("Channel for follow/sub info and to limit the "
                                "message search. Default: all open "
                                "channels.")},
                {"recent_messages",
                 schema::integer("How many of their latest messages to "
                                 "include. Default 20, max 200.")},
                {"remote", schema::boolean("Fetch Twitch info. Default "
                                           "true.")},
                {"format", messageFormatSchema()},
            },
            {"user"}),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto user = args::string(a, "user").trimmed();
                if (user.startsWith('@'))
                {
                    user = user.mid(1);
                }
                if (user.isEmpty())
                {
                    done(ToolResult::fail(u"\"user\" is empty"_s));
                    return;
                }
                auto channelName = args::string(a, "channel");
                auto recentLimit =
                    std::clamp(args::integer(a, "recent_messages", 20), 0, 200);
                auto remote = args::boolean(a, "remote", true);
                auto lines = wantsLines(a);

                QString login = user.toLower();
                QString userId;
                QString displayName;
                std::vector<MessagePtr> messages;
                QJsonObject perChannel;
                for (const auto &channel : channelsFor(channelName))
                {
                    int count = 0;
                    MessagePtr latest;
                    for (const auto &m : channel->getMessageSnapshot())
                    {
                        if (!isUserMessage(*m) || !matchesUser(*m, user))
                        {
                            continue;
                        }
                        ++count;
                        latest = m;
                        messages.push_back(m);
                    }
                    if (latest)
                    {
                        login = latest->loginName;
                        if (!latest->userID.isEmpty())
                        {
                            userId = latest->userID;
                        }
                        displayName = latest->displayName;
                        QJsonArray badges;
                        for (const auto &badge : latest->twitchBadges)
                        {
                            badges.append(badge.key_);
                        }
                        perChannel.insert(
                            channel->getName(),
                            QJsonObject{
                                {"messages", count},
                                {"badges", badges},
                                {"lastSeen",
                                 latest->timestamp.toLocalTime()
                                     .toString(Qt::ISODate)},
                            });
                    }
                }
                std::ranges::stable_sort(
                    messages, [](const MessagePtr &l, const MessagePtr &r) {
                        return l->timestamp < r->timestamp;
                    });
                QJsonArray recent;
                auto firstRecent = std::max<qsizetype>(
                    0, static_cast<qsizetype>(messages.size()) - recentLimit);
                for (auto i = firstRecent;
                     i < static_cast<qsizetype>(messages.size()); ++i)
                {
                    recent.append(formatMessage(*messages[i], lines));
                }

                auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
                QJsonObject local{
                    {"loadedMessages", static_cast<qint64>(messages.size())},
                    {"channels", perChannel},
                    {"recentMessages", recent},
                    {"blocked",
                     currentUser->blockedUserLogins().contains(login)},
                    {"highlightBlacklisted",
                     getSettings()->isBlacklistedUser(login)},
                };
                if (auto nickname = getSettings()->matchNickname(login))
                {
                    local.insert("nickname", *nickname);
                }

                struct State {
                    QJsonObject result;
                    QJsonObject twitch;
                    int pending = 0;
                    bool finished = false;
                    ToolDone done;
                    QString login;
                    QString userId;
                    QJsonObject local;

                    void finish()
                    {
                        if (this->finished)
                        {
                            return;
                        }
                        this->finished = true;
                        if (!this->userId.isEmpty())
                        {
                            if (auto data =
                                    getApp()->getUserData()->getUser(
                                        this->userId))
                            {
                                if (!data->notes.isEmpty())
                                {
                                    this->local.insert("notes", data->notes);
                                }
                                if (data->color)
                                {
                                    this->local.insert(
                                        "customColor",
                                        data->color->name(QColor::HexRgb));
                                }
                            }
                        }
                        this->result.insert("login", this->login);
                        if (!this->userId.isEmpty())
                        {
                            this->result.insert("userId", this->userId);
                        }
                        this->result.insert("local", this->local);
                        if (!this->twitch.isEmpty())
                        {
                            this->result.insert("twitch", this->twitch);
                        }
                        this->done(ToolResult::ok(this->result));
                    }

                    void complete()
                    {
                        if (--this->pending <= 0)
                        {
                            this->finish();
                        }
                    }
                };
                auto state = std::make_shared<State>();
                state->done = done;
                state->login = login;
                state->userId = userId;
                state->local = local;
                if (!displayName.isEmpty())
                {
                    state->result.insert("displayName", displayName);
                }

                if (!remote)
                {
                    state->finish();
                    return;
                }

                QTimer::singleShot(USER_LOOKUP_TIMEOUT_MS, [state] {
                    if (!state->finished)
                    {
                        state->twitch.insert("timedOut", true);
                        state->finish();
                    }
                });

                auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(
                    channelName.isEmpty() ? nullptr : findChannel(channelName));

                state->pending = 1;
                getHelix()->getUserByName(
                    login,
                    [state, twitchChannel](const HelixUser &helixUser) {
                        state->userId = helixUser.id;
                        state->login = helixUser.login;
                        state->twitch.insert("displayName",
                                             helixUser.displayName);
                        state->twitch.insert("createdAt", helixUser.createdAt);
                        state->twitch.insert("description",
                                             helixUser.description);

                        // The lookups that need the user id
                        state->pending += 2;
                        getHelix()->getChannelFollowers(
                            helixUser.id, {},
                            [state](const auto &response) {
                                state->twitch.insert("followers",
                                                     response.total);
                                state->complete();
                            },
                            [state](const auto &) {
                                state->complete();
                            });
                        getHelix()->getStreamById(
                            helixUser.id,
                            [state](bool live, const HelixStream &stream) {
                                state->twitch.insert("live", live);
                                if (live)
                                {
                                    state->twitch.insert("streamTitle",
                                                         stream.title);
                                    state->twitch.insert("streamGame",
                                                         stream.gameName);
                                }
                            },
                            [] {},
                            [state] {
                                state->complete();
                            });

                        if (twitchChannel && (twitchChannel->isMod() ||
                                              twitchChannel->isBroadcaster()))
                        {
                            state->pending += 1;
                            getHelix()->getChannelFollowers(
                                twitchChannel->roomId(), helixUser.id,
                                [state](const auto &response) {
                                    if (response.specifiedFollower)
                                    {
                                        state->twitch.insert(
                                            "followingChannelSince",
                                            response.specifiedFollower
                                                ->followedAt.toLocalTime()
                                                .toString(Qt::ISODate));
                                    }
                                    else
                                    {
                                        state->twitch.insert(
                                            "followingChannel", false);
                                    }
                                    state->complete();
                                },
                                [state](const auto &) {
                                    state->complete();
                                });
                        }
                        state->complete();
                    },
                    [state] {
                        state->twitch.insert("error",
                                             "Could not find the user on "
                                             "Twitch");
                        state->complete();
                    });

                if (twitchChannel)
                {
                    state->pending += 1;
                    getIvr()->getSubage(
                        login, twitchChannel->getName(),
                        [state](const IvrSubage &sub) {
                            if (sub.isSubHidden)
                            {
                                state->twitch.insert("subscription", "hidden");
                            }
                            else
                            {
                                state->twitch.insert(
                                    "subscription",
                                    QJsonObject{
                                        {"subscribed", sub.isSubbed},
                                        {"tier", sub.subTier},
                                        {"totalMonths", sub.totalSubMonths},
                                    });
                            }
                            state->complete();
                        },
                        [state] {
                            state->complete();
                        });
                }
            },
    });

    return tools;
}

}  // namespace chatterino::mcp
