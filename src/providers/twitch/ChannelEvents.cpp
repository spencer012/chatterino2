// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/ChannelEvents.hpp"

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "providers/twitch/api/Helix.hpp"

#include <QJsonArray>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <initializer_list>

namespace chatterino {

namespace {

using channelpoints::parseTwitchTime;

/// Hype trains that passed their expiry are dropped after this many seconds,
/// in case the end message got lost.
constexpr int HYPE_TRAIN_EXPIRY_GRACE_SECONDS = 30;
/// Same for polls that should have ended.
constexpr int POLL_EXPIRY_GRACE_SECONDS = 60;
/// Predictions close on their own when the window runs out. If Twitch's
/// LOCKED update hasn't arrived shortly after, close it ourselves.
constexpr int PREDICTION_LOCK_GRACE_SECONDS = 2;

/// How far along a prediction is. It only ever moves forward.
int predictionStage(PredictionStatus status)
{
    switch (status)
    {
        case PredictionStatus::Active:
            return 0;
        case PredictionStatus::Locked:
            return 1;
        case PredictionStatus::ResolvePending:
        case PredictionStatus::CancelPending:
            return 2;
        case PredictionStatus::Resolved:
        case PredictionStatus::Canceled:
            return 3;
        case PredictionStatus::Unknown:
            break;
    }
    return -1;
}

/// Persisted query of the Twitch website's MakePrediction mutation.
const QString MAKE_PREDICTION_HASH = QStringLiteral(
    "b44682ecc88358817009f20e69d75081b1e58825bb40aa53d5dbadcc17c881d8");

const QString MAKE_PREDICTION_QUERY = QStringLiteral(
    "mutation MakePrediction($input: MakePredictionInput!) { "
    "makePrediction(input: $input) { prediction { id points outcome { id } } "
    "error { code } } }");

/// Persisted query of the Twitch website's ChannelPollContext_VoteInPoll
/// mutation.
const QString VOTE_IN_POLL_HASH = QStringLiteral(
    "1280e27b0f3c7ae60b5714bd569771ea50635778473182e6e959e2dcfcc16e3c");

const QString VOTE_IN_POLL_QUERY = QStringLiteral(
    "mutation VoteInPoll($input: VoteInPollInput!) { "
    "voteInPoll(input: $input) { error { code } } }");

const QString PINNED_MESSAGE_QUERY = QStringLiteral(
    "query PinnedChatMessage($login: String!) { user(login: $login) { id "
    "channel { pinnedChatMessages(first: 5) { edges { node { id type "
    "startsAt endsAt pinnedBy { id login displayName } "
    "pinnedMessage { id sender { id login displayName } "
    "content { text } } } } } } } }");

const QString SNAPSHOT_QUERY = QStringLiteral(
    "query ChannelEvents($login: String!) { user(login: $login) { id "
    "viewablePoll { id title status durationSeconds "
    "remainingDurationMilliseconds totalVoters votes { total } "
    "settings { multichoice { isEnabled } subscriberOnly { isEnabled } "
    "communityPointsVotes { isEnabled cost } } "
    "choices { id title totalVoters votes { total } } } "
    "channel { hypeTrain { execution { id expiresAt endReason "
    "isGoldenKappaTrain isTreasureTrain "
    "treasureTrainDetails { discountPercentage discountLevelThreshold status } "
    "sharedHypeTrainDetails { __typename } "
    "progress { goal progression total remainingSeconds level { value } } "
    "allTimeHigh { level { value } } "
    "config { primaryHexColor willUseCreatorColor } } } "
    "activePredictionEvents { ...prediction } "
    "lockedPredictionEvents { ...prediction } } } } "
    "fragment prediction on PredictionEvent { id title status createdAt "
    "predictionWindowSeconds winningOutcome { id } "
    "outcomes { id title color totalPoints totalUsers } }");

/// The first of `keys` that's present and not null.
QJsonValue pick(const QJsonObject &obj, std::initializer_list<const char *> keys)
{
    for (const auto *key : keys)
    {
        const auto value = obj.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull())
        {
            return value;
        }
    }
    return {};
}

std::int64_t toInt64(const QJsonValue &value)
{
    if (value.isDouble())
    {
        return static_cast<std::int64_t>(value.toDouble());
    }
    if (value.isString())
    {
        return value.toString().toLongLong();
    }
    return 0;
}

int toInt(const QJsonValue &value)
{
    return static_cast<int>(toInt64(value));
}

bool isEnabled(const QJsonObject &setting)
{
    return pick(setting, {"is_enabled", "isEnabled"}).toBool(false);
}

QString formatNumber(std::int64_t value)
{
    return QLocale().toString(static_cast<qlonglong>(value));
}

QString plural(std::int64_t count, const QString &singular,
               const QString &pluralForm)
{
    return QStringLiteral("%1 %2").arg(formatNumber(count),
                                       count == 1 ? singular : pluralForm);
}

QString quoted(const QString &text)
{
    return QStringLiteral("\"%1\"").arg(text);
}

QString joinNames(const QStringList &names)
{
    if (names.size() <= 1)
    {
        return names.join(QString());
    }
    return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) +
           QStringLiteral(" and ") + names.last();
}

std::optional<channelevents::ActionResponse> parseActionResponse(
    const QJsonObject &root, const QString &field, QString &error)
{
    const auto data = root.value("data").toObject();
    const auto payload = data.value(field);
    if (!payload.isObject())
    {
        const auto errors = channelpoints::inspectGqlErrors(root);
        error = errors.hasErrors ? errors.message
                                 : QStringLiteral("Twitch sent no result");
        return std::nullopt;
    }

    channelevents::ActionResponse response;
    const auto errorObj = payload.toObject().value("error").toObject();
    response.errorCode = errorObj.value("code").toString();
    response.ok = errorObj.isEmpty();
    if (!response.ok && response.errorCode.isEmpty())
    {
        response.errorCode = QStringLiteral("UNKNOWN");
    }
    return response;
}

}  // namespace

// ---------------------------------------------------------------------------
// State

QString HypeTrainState::kindName() const
{
    if (this->isTreasureTrain)
    {
        return QStringLiteral("Treasure Train");
    }
    if (this->isGoldenKappaTrain)
    {
        return QStringLiteral("Golden Kappa Train");
    }
    if (this->isSharedTrain)
    {
        return QStringLiteral("Shared Hype Train");
    }
    return QStringLiteral("Hype Train");
}

int HypeTrainState::percent() const
{
    if (this->goal <= 0)
    {
        return 0;
    }
    return std::clamp(static_cast<int>(static_cast<std::int64_t>(this->progress) *
                                       100 / this->goal),
                      0, 100);
}

bool PollState::isActive() const
{
    return this->status == PollStatus::Active;
}

bool PollState::isEnded() const
{
    return this->status == PollStatus::Completed ||
           this->status == PollStatus::Terminated;
}

int PollState::percent(const PollChoice &choice) const
{
    if (this->totalVotes <= 0)
    {
        return 0;
    }
    return static_cast<int>((choice.votes * 100 + this->totalVotes / 2) /
                            this->totalVotes);
}

std::vector<const PollChoice *> PollState::leaders() const
{
    std::vector<const PollChoice *> result;
    std::int64_t best = 0;
    for (const auto &choice : this->choices)
    {
        if (choice.votes <= 0 || choice.votes < best)
        {
            continue;
        }
        if (choice.votes > best)
        {
            best = choice.votes;
            result.clear();
        }
        result.push_back(&choice);
    }
    return result;
}

bool PredictionState::isOpen() const
{
    return this->status == PredictionStatus::Active;
}

bool PredictionState::isEnded() const
{
    return this->status == PredictionStatus::Resolved ||
           this->status == PredictionStatus::Canceled;
}

std::int64_t PredictionState::totalPoints() const
{
    std::int64_t total = 0;
    for (const auto &outcome : this->outcomes)
    {
        total += outcome.points;
    }
    return total;
}

int PredictionState::totalUsers() const
{
    int total = 0;
    for (const auto &outcome : this->outcomes)
    {
        total += outcome.users;
    }
    return total;
}

int PredictionState::percent(const PredictionOutcome &outcome) const
{
    const auto total = this->totalPoints();
    if (total <= 0)
    {
        return 0;
    }
    return static_cast<int>((outcome.points * 100 + total / 2) / total);
}

double PredictionState::payoutRatio(const PredictionOutcome &outcome) const
{
    if (outcome.points <= 0)
    {
        return 0;
    }
    return static_cast<double>(this->totalPoints()) /
           static_cast<double>(outcome.points);
}

const PredictionOutcome *PredictionState::findOutcome(const QString &id) const
{
    for (const auto &outcome : this->outcomes)
    {
        if (outcome.id == id)
        {
            return &outcome;
        }
    }
    return nullptr;
}

namespace channelevents {

PollStatus parsePollStatus(const QString &value)
{
    const auto upper = value.toUpper();
    if (upper == "ACTIVE")
    {
        return PollStatus::Active;
    }
    if (upper == "COMPLETED")
    {
        return PollStatus::Completed;
    }
    if (upper == "TERMINATED")
    {
        return PollStatus::Terminated;
    }
    if (upper == "ARCHIVED")
    {
        return PollStatus::Archived;
    }
    if (upper == "MODERATED")
    {
        return PollStatus::Moderated;
    }
    return PollStatus::Unknown;
}

PredictionStatus parsePredictionStatus(const QString &value)
{
    const auto upper = value.toUpper();
    if (upper == "ACTIVE")
    {
        return PredictionStatus::Active;
    }
    if (upper == "LOCKED")
    {
        return PredictionStatus::Locked;
    }
    if (upper == "RESOLVE_PENDING")
    {
        return PredictionStatus::ResolvePending;
    }
    if (upper == "RESOLVED")
    {
        return PredictionStatus::Resolved;
    }
    if (upper == "CANCEL_PENDING")
    {
        return PredictionStatus::CancelPending;
    }
    if (upper == "CANCELED" || upper == "CANCELLED")
    {
        return PredictionStatus::Canceled;
    }
    return PredictionStatus::Unknown;
}

QString describePredictionStatus(PredictionStatus status)
{
    switch (status)
    {
        case PredictionStatus::Active:
            return QStringLiteral("Open");
        case PredictionStatus::Locked:
            return QStringLiteral("Locked");
        case PredictionStatus::ResolvePending:
            return QStringLiteral("Resolving");
        case PredictionStatus::Resolved:
            return QStringLiteral("Resolved");
        case PredictionStatus::CancelPending:
        case PredictionStatus::Canceled:
            return QStringLiteral("Cancelled");
        case PredictionStatus::Unknown:
            break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Parsing

std::optional<HypeTrainState> parseHypeTrainExecution(const QJsonObject &exec,
                                                      const QDateTime &now)
{
    HypeTrainState train;
    train.id = exec.value("id").toString();
    if (train.id.isEmpty())
    {
        return std::nullopt;
    }

    const auto progress = exec.value("progress").toObject();
    train.level =
        std::max(1, toInt(progress.value("level").toObject().value("value")));
    train.progress = toInt(progress.value("progression"));
    train.goal = toInt(progress.value("goal"));
    train.total = toInt(progress.value("total"));

    const auto remaining = toInt(progress.value("remainingSeconds"));
    if (remaining > 0)
    {
        train.expiresAt = now.addSecs(remaining);
    }
    else
    {
        train.expiresAt = parseTwitchTime(exec.value("expiresAt").toString());
    }

    train.isTreasureTrain = exec.value("isTreasureTrain").toBool(false);
    train.isGoldenKappaTrain = exec.value("isGoldenKappaTrain").toBool(false);
    train.isSharedTrain = exec.value("sharedHypeTrainDetails").isObject();

    auto details = exec.value("treasureTrainDetails").toObject();
    if (details.isEmpty())
    {
        const auto variant = exec.value("variantTrainDetails").toObject();
        if (variant.value("__typename").toString() == "TreasureTrainDetails")
        {
            details = variant;
        }
    }
    applyTreasureDetails(train, details);

    const auto config = exec.value("config").toObject();
    const auto hex = config.value("primaryHexColor").toString();
    if (config.value("willUseCreatorColor").toBool(false) && !hex.isEmpty())
    {
        train.color = QColor(hex.startsWith('#') ? hex : "#" + hex);
    }

    train.allTimeHighLevel = toInt(exec.value("allTimeHigh")
                                       .toObject()
                                       .value("level")
                                       .toObject()
                                       .value("value"));

    const auto endReason = exec.value("endReason").toString();
    if (endReason != "IN_PROGRESS")
    {
        train.endReason = endReason;
    }
    return train;
}

void applyHypeTrainProgress(HypeTrainState &state, const QJsonObject &progress,
                            const QDateTime &now)
{
    if (progress.isEmpty())
    {
        return;
    }

    const auto level = toInt(progress.value("level").toObject().value("value"));
    if (level > 0)
    {
        state.level = level;
    }
    state.progress = toInt(pick(progress, {"value", "progression"}));
    state.goal = toInt(progress.value("goal"));
    const auto total = toInt(progress.value("total"));
    if (total > 0)
    {
        state.total = total;
    }

    const auto remaining =
        toInt(pick(progress, {"remaining_seconds", "remainingSeconds"}));
    if (remaining > 0)
    {
        state.expiresAt = now.addSecs(remaining);
    }
}

void applyTreasureDetails(HypeTrainState &state, const QJsonObject &details)
{
    if (details.isEmpty())
    {
        return;
    }

    state.isTreasureTrain = true;
    state.treasureDiscountPercent =
        toInt(pick(details, {"discountPercentage", "discount_percentage"}));
    state.treasureDiscountLevel = toInt(
        pick(details, {"discountLevelThreshold", "discount_level_threshold"}));
    state.treasureDiscountReached =
        pick(details, {"status", "discount_status"}).toString() == "REACHED";
}

std::optional<PollState> parsePoll(const QJsonObject &poll, const QDateTime &now)
{
    PollState state;
    state.id = pick(poll, {"poll_id", "id"}).toString();
    if (state.id.isEmpty())
    {
        return std::nullopt;
    }

    state.title = poll.value("title").toString();
    state.status = parsePollStatus(poll.value("status").toString());
    state.durationSeconds =
        toInt(pick(poll, {"duration_seconds", "durationSeconds"}));
    state.totalVoters = toInt(pick(poll, {"total_voters", "totalVoters"}));

    const auto remainingMs = toInt64(pick(
        poll,
        {"remaining_duration_milliseconds", "remainingDurationMilliseconds"}));
    if (state.isActive())
    {
        state.endsAt = now.addMSecs(std::max<std::int64_t>(0, remainingMs));
    }

    const auto settings = poll.value("settings").toObject();
    const auto pointsVotes =
        pick(settings, {"channel_points_votes", "channelPointsVotes",
                        "communityPointsVotes"})
            .toObject();
    state.channelPointsVotingEnabled = isEnabled(pointsVotes);
    state.channelPointsCost = toInt(pointsVotes.value("cost"));
    state.subscriberOnly =
        isEnabled(pick(settings, {"subscriber_only", "subscriberOnly"}).toObject());
    state.multichoice =
        isEnabled(pick(settings, {"multi_choice", "multichoice", "multiChoice"})
                      .toObject());

    std::int64_t summed = 0;
    for (const auto &value : poll.value("choices").toArray())
    {
        const auto obj = value.toObject();
        PollChoice choice;
        choice.id = pick(obj, {"choice_id", "id"}).toString();
        choice.title = obj.value("title").toString();
        choice.votes = toInt64(obj.value("votes").toObject().value("total"));
        choice.voters = toInt(pick(obj, {"total_voters", "totalVoters"}));
        summed += choice.votes;
        state.choices.push_back(choice);
    }

    const auto votes = poll.value("votes").toObject();
    state.totalVotes = votes.contains("total") ? toInt64(votes.value("total"))
                                               : summed;
    return state;
}

std::optional<PredictionState> parsePrediction(const QJsonObject &event,
                                               const QDateTime &now,
                                               const QDateTime &serverNow)
{
    PredictionState state;
    state.id = event.value("id").toString();
    if (state.id.isEmpty())
    {
        return std::nullopt;
    }

    state.title = event.value("title").toString();
    state.status = parsePredictionStatus(event.value("status").toString());

    state.winningOutcomeId = event.value("winning_outcome_id").toString();
    if (state.winningOutcomeId.isEmpty())
    {
        state.winningOutcomeId =
            event.value("winningOutcome").toObject().value("id").toString();
    }

    const auto createdAt =
        parseTwitchTime(pick(event, {"created_at", "createdAt"}).toString());
    const auto window = toInt(
        pick(event, {"prediction_window_seconds", "predictionWindowSeconds"}));
    if (state.isOpen() && createdAt.isValid())
    {
        const auto locksAt = createdAt.addSecs(window);
        // Measure against Twitch's own clock where possible.
        state.locksAt = serverNow.isValid()
                            ? now.addMSecs(serverNow.msecsTo(locksAt))
                            : locksAt;
    }

    for (const auto &value : event.value("outcomes").toArray())
    {
        const auto obj = value.toObject();
        PredictionOutcome outcome;
        outcome.id = obj.value("id").toString();
        outcome.title = obj.value("title").toString();
        outcome.color = obj.value("color").toString().toUpper();
        outcome.points = toInt64(pick(obj, {"total_points", "totalPoints"}));
        outcome.users = toInt(pick(obj, {"total_users", "totalUsers"}));
        state.outcomes.push_back(outcome);
    }
    return state;
}

// ---------------------------------------------------------------------------
// GQL

QJsonObject buildPinnedMessageRequest(const QString &channelLogin)
{
    return {
        {"operationName", "PinnedChatMessage"},
        {"query", PINNED_MESSAGE_QUERY},
        {"variables", QJsonObject{{"login", channelLogin}}},
    };
}

std::optional<HelixPinnedChatMessage> parsePinnedMessage(
    const QJsonObject &root, const QDateTime &now, QString &error)
{
    const auto data = root.value("data").toObject();
    if (data.isEmpty())
    {
        const auto errors = channelpoints::inspectGqlErrors(root);
        error = errors.hasErrors ? errors.message
                                 : QStringLiteral("Response has no data");
        return std::nullopt;
    }

    const auto user = data.value("user").toObject();
    if (user.isEmpty())
    {
        error = QStringLiteral("Channel not found");
        return std::nullopt;
    }

    const auto edges = user.value("channel")
                           .toObject()
                           .value("pinnedChatMessages")
                           .toObject()
                           .value("edges")
                           .toArray();
    for (const auto &edge : edges)
    {
        const auto node = edge.toObject().value("node").toObject();
        // Helix only knows mod pins; paid pins are something else.
        const auto type = node.value("type").toString();
        if (!type.isEmpty() && type != "MOD")
        {
            continue;
        }
        const auto endsAt = parseTwitchTime(node.value("endsAt").toString());
        if (endsAt.isValid() && endsAt <= now)
        {
            continue;
        }
        const auto message = node.value("pinnedMessage").toObject();
        if (message.isEmpty())
        {
            continue;
        }

        // Same shape as Helix's response, so both build the same struct.
        const auto sender = message.value("sender").toObject();
        const auto pinnedBy = node.value("pinnedBy").toObject();
        QJsonObject helix{
            {"message_id", message.value("id")},
            {"sender_user_id", sender.value("id")},
            {"sender_user_login", sender.value("login")},
            {"sender_user_name", sender.value("displayName")},
            {"pinned_by_user_id", pinnedBy.value("id")},
            {"pinned_by_user_login", pinnedBy.value("login")},
            {"pinned_by_user_name", pinnedBy.value("displayName")},
            {"message",
             QJsonObject{{"text", message.value("content")
                                      .toObject()
                                      .value("text")}}},
            {"starts_at", node.value("startsAt")},
        };
        if (endsAt.isValid())
        {
            helix.insert("ends_at", node.value("endsAt"));
        }
        return HelixPinnedChatMessage(helix);
    }

    return std::nullopt;
}

QJsonObject buildSnapshotRequest(const QString &channelLogin)
{
    return {
        {"operationName", "ChannelEvents"},
        {"query", SNAPSHOT_QUERY},
        {"variables", QJsonObject{{"login", channelLogin}}},
    };
}

std::optional<Snapshot> parseSnapshot(const QJsonObject &root,
                                      const QDateTime &now, QString &error)
{
    const auto data = root.value("data").toObject();
    if (data.isEmpty())
    {
        const auto errors = channelpoints::inspectGqlErrors(root);
        error = errors.hasErrors ? errors.message
                                 : QStringLiteral("Response has no data");
        return std::nullopt;
    }

    const auto user = data.value("user").toObject();
    if (user.isEmpty())
    {
        error = QStringLiteral("Channel not found");
        return std::nullopt;
    }

    Snapshot snapshot;
    snapshot.channelId = user.value("id").toString();

    const auto pollObj = user.value("viewablePoll").toObject();
    if (!pollObj.isEmpty())
    {
        snapshot.poll = parsePoll(pollObj, now);
    }

    const auto channel = user.value("channel").toObject();
    const auto exec = channel.value("hypeTrain")
                          .toObject()
                          .value("execution")
                          .toObject();
    if (!exec.isEmpty())
    {
        auto train = parseHypeTrainExecution(exec, now);
        // GQL keeps returning a train for a while after it ended.
        if (train && train->endReason.isEmpty())
        {
            snapshot.hypeTrain = train;
        }
    }

    for (const auto *key : {"activePredictionEvents", "lockedPredictionEvents"})
    {
        const auto events = channel.value(QLatin1String(key)).toArray();
        if (!events.isEmpty())
        {
            snapshot.prediction = parsePrediction(events.first().toObject(), now);
            break;
        }
    }

    return snapshot;
}

QJsonObject buildVoteRequest(const QString &pollId, const QString &choiceId,
                             const QString &userId, const QString &voteId,
                             bool inlineQuery)
{
    // Same input as the website. `tokens` = null is the free vote; extra
    // votes for channel points would go there.
    QJsonObject request{
        {"variables",
         QJsonObject{
             {"input",
              QJsonObject{
                  {"pollID", pollId},
                  {"choiceID", choiceId},
                  {"userID", userId},
                  {"voteID", voteId},
                  {"tokens", QJsonValue::Null},
              }},
         }},
    };
    if (inlineQuery)
    {
        request.insert("operationName", "VoteInPoll");
        request.insert("query", VOTE_IN_POLL_QUERY);
    }
    else
    {
        request.insert("operationName", "ChannelPollContext_VoteInPoll");
        request.insert("extensions",
                       QJsonObject{
                           {"persistedQuery",
                            QJsonObject{
                                {"version", 1},
                                {"sha256Hash", VOTE_IN_POLL_HASH},
                            }},
                       });
    }
    return request;
}

QJsonObject buildPredictionRequest(const QString &eventId,
                                   const QString &outcomeId,
                                   std::int64_t points,
                                   const QString &transactionId,
                                   bool inlineQuery)
{
    QJsonObject request{
        {"operationName", "MakePrediction"},
        {"variables",
         QJsonObject{
             {"input",
              QJsonObject{
                  {"eventID", eventId},
                  {"outcomeID", outcomeId},
                  {"points", static_cast<qint64>(points)},
                  {"transactionID", transactionId},
              }},
         }},
    };
    if (inlineQuery)
    {
        request.insert("query", MAKE_PREDICTION_QUERY);
    }
    else
    {
        request.insert("extensions",
                       QJsonObject{
                           {"persistedQuery",
                            QJsonObject{
                                {"version", 1},
                                {"sha256Hash", MAKE_PREDICTION_HASH},
                            }},
                       });
    }
    return request;
}

std::optional<ActionResponse> parseVoteResponse(const QJsonObject &root,
                                                QString &error)
{
    return parseActionResponse(root, QStringLiteral("voteInPoll"), error);
}

std::optional<ActionResponse> parsePredictionResponse(const QJsonObject &root,
                                                      QString &error)
{
    return parseActionResponse(root, QStringLiteral("makePrediction"), error);
}

QString describeVoteError(const QString &code)
{
    const auto upper = code.toUpper();
    if (upper.contains("NOT_ACTIVE") || upper.contains("ENDED") ||
        upper.contains("NOT_FOUND"))
    {
        return QStringLiteral("The poll is over.");
    }
    if (upper.contains("ALREADY_VOTED") || upper.contains("MAX_VOTES") ||
        upper.contains("LIMIT"))
    {
        return QStringLiteral("You already voted in this poll.");
    }
    if (upper.contains("SUBSCRIBER"))
    {
        return QStringLiteral("Only subscribers can vote in this poll.");
    }
    if (upper.contains("INSUFFICIENT") || upper.contains("NOT_ENOUGH"))
    {
        return QStringLiteral("Not enough channel points.");
    }
    return QStringLiteral("Twitch refused the vote (%1).").arg(code);
}

QString describePredictionError(const QString &code)
{
    const auto upper = code.toUpper();
    if (upper.contains("NOT_ENOUGH") || upper.contains("INSUFFICIENT"))
    {
        return QStringLiteral("Not enough channel points.");
    }
    if (upper.contains("NOT_ACTIVE") || upper.contains("LOCKED") ||
        upper.contains("ENDED") || upper.contains("NOT_FOUND"))
    {
        return QStringLiteral("Predictions are closed.");
    }
    if (upper.contains("MAX_POINTS") || upper.contains("LIMIT"))
    {
        return QStringLiteral("That's more than you can predict on this "
                              "event.");
    }
    if (upper.contains("DIFFERENT_OUTCOME") || upper.contains("OTHER_OUTCOME"))
    {
        return QStringLiteral("You already predicted a different outcome.");
    }
    if (upper.contains("DUPLICATE"))
    {
        return QStringLiteral("Twitch already got this prediction.");
    }
    return QStringLiteral("Twitch refused the prediction (%1).").arg(code);
}

// ---------------------------------------------------------------------------
// Chat messages

QString formatHypeTrainEnd(const HypeTrainState &train)
{
    // EXPIRED = level 1 was never completed.
    if (train.endReason == "EXPIRED" && train.level <= 1)
    {
        return QStringLiteral("%1 ended without completing level 1, %2% of "
                              "the way there.")
            .arg(train.kindName())
            .arg(train.percent());
    }

    auto text = QStringLiteral("%1 ended at level %2, %3% of the way to "
                               "level %4.")
                    .arg(train.kindName())
                    .arg(train.level)
                    .arg(train.percent())
                    .arg(train.level + 1);

    if (train.isTreasureTrain && train.treasureDiscountPercent > 0)
    {
        if (train.treasureDiscountReached)
        {
            text += QStringLiteral(" The %1% sub discount was unlocked.")
                        .arg(train.treasureDiscountPercent);
        }
        else
        {
            text += QStringLiteral(" The %1% sub discount (level %2) wasn't "
                                   "reached.")
                        .arg(train.treasureDiscountPercent)
                        .arg(train.treasureDiscountLevel);
        }
    }

    if (train.allTimeHighLevel > 0 && train.level > train.allTimeHighLevel)
    {
        text += QStringLiteral(" New record, the previous best was level %1.")
                    .arg(train.allTimeHighLevel);
    }
    return text;
}

QString formatPollEnd(const PollState &poll, const QSet<QString> &ownChoices)
{
    auto text = poll.status == PollStatus::Terminated
                    ? QStringLiteral("Poll ended early: ")
                    : QStringLiteral("Poll ended: ");
    text += quoted(poll.title) + QStringLiteral(".");

    const auto leaders = poll.leaders();
    if (leaders.empty())
    {
        text += QStringLiteral(" Nobody voted.");
    }
    else
    {
        QStringList names;
        for (const auto *choice : leaders)
        {
            names.append(choice->title);
        }
        text += leaders.size() == 1
                    ? QStringLiteral(" Winner: %1.").arg(names.first())
                    : QStringLiteral(" Tie between %1.").arg(joinNames(names));

        auto sorted = poll.choices;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto &a, const auto &b) {
                             return a.votes > b.votes;
                         });
        QStringList parts;
        for (const auto &choice : sorted)
        {
            parts.append(QStringLiteral("%1 %2% (%3)")
                             .arg(choice.title)
                             .arg(poll.percent(choice))
                             .arg(plural(choice.votes, QStringLiteral("vote"),
                                         QStringLiteral("votes"))));
        }
        text += QStringLiteral(" ") + parts.join(QStringLiteral(" · ")) +
                QStringLiteral(".");
    }

    QStringList mine;
    for (const auto &choice : poll.choices)
    {
        if (ownChoices.contains(choice.id))
        {
            mine.append(choice.title);
        }
    }
    if (!mine.isEmpty())
    {
        text += QStringLiteral(" You voted for %1.").arg(joinNames(mine));
    }
    return text;
}

QString formatPredictionEnd(const PredictionState &prediction,
                            const std::optional<OwnPrediction> &own)
{
    if (prediction.status == PredictionStatus::Canceled)
    {
        auto text = QStringLiteral("Prediction cancelled: %1. Points were "
                                   "refunded.")
                        .arg(quoted(prediction.title));
        if (own)
        {
            text += QStringLiteral(" You got your %1 back.")
                        .arg(plural(own->points, QStringLiteral("point"),
                                    QStringLiteral("points")));
        }
        return text;
    }

    auto text =
        QStringLiteral("Prediction resolved: %1.").arg(quoted(prediction.title));

    const auto *winner = prediction.findOutcome(prediction.winningOutcomeId);
    if (winner != nullptr)
    {
        text += QStringLiteral(" Winner: %1.").arg(winner->title);
    }

    QStringList parts;
    for (const auto &outcome : prediction.outcomes)
    {
        parts.append(QStringLiteral("%1 %2% (%3, %4)")
                         .arg(outcome.title)
                         .arg(prediction.percent(outcome))
                         .arg(plural(outcome.points, QStringLiteral("point"),
                                     QStringLiteral("points")),
                              plural(outcome.users, QStringLiteral("user"),
                                     QStringLiteral("users"))));
    }
    if (!parts.isEmpty())
    {
        text += QStringLiteral(" ") + parts.join(QStringLiteral(" · ")) +
                QStringLiteral(".");
    }

    if (own)
    {
        const auto *mine = prediction.findOutcome(own->outcomeId);
        const auto name = mine != nullptr ? mine->title : QStringLiteral("?");
        const auto points = plural(own->points, QStringLiteral("point"),
                                   QStringLiteral("points"));
        if (winner == nullptr)
        {
            text += QStringLiteral(" You predicted %1 with %2.").arg(name, points);
        }
        else if (winner->id == own->outcomeId)
        {
            const auto payout = static_cast<std::int64_t>(
                static_cast<double>(own->points) *
                prediction.payoutRatio(*winner));
            text += QStringLiteral(" You predicted %1 with %2 and won about "
                                   "%3.")
                        .arg(name, points, formatNumber(payout));
        }
        else
        {
            text += QStringLiteral(" You predicted %1 and lost %2.")
                        .arg(name, points);
        }
    }
    return text;
}

QString shortNumber(std::int64_t value)
{
    const auto scaled = [](double v, const char *suffix) {
        auto number = QString::number(v, 'f', v < 100 ? 1 : 0);
        if (number.endsWith(".0"))
        {
            number.chop(2);
        }
        return number + QLatin1String(suffix);
    };

    const auto abs = value < 0 ? -value : value;
    if (abs < 1000)
    {
        return QString::number(value);
    }
    if (abs < 1000000)
    {
        return scaled(static_cast<double>(value) / 1e3, "K");
    }
    if (abs < 1000000000)
    {
        return scaled(static_cast<double>(value) / 1e6, "M");
    }
    return scaled(static_cast<double>(value) / 1e9, "B");
}

}  // namespace channelevents

// ---------------------------------------------------------------------------
// Tracker

ChannelEventsTracker::Result ChannelEventsTracker::applyHypeTrainMessage(
    const QString &type, const QJsonObject &data, const QDateTime &now)
{
    using namespace channelevents;

    Result result;

    if (type == "hype-train-start")
    {
        auto train = parseHypeTrainExecution(data, now);
        if (!train || this->endedHypeTrains_.contains(train->id))
        {
            return result;
        }
        this->hypeTrain_ = std::move(train);
        result.changed = true;
        return result;
    }

    if (type == "hype-train-level-up")
    {
        auto train =
            parseHypeTrainExecution(data.value("hype_train").toObject(), now);
        if (train)
        {
            if (this->endedHypeTrains_.contains(train->id))
            {
                return result;
            }
            this->hypeTrain_ = std::move(train);
        }
        if (!this->hypeTrain_)
        {
            result.needsSnapshot = true;
            return result;
        }
        applyHypeTrainProgress(*this->hypeTrain_,
                               data.value("progress").toObject(), now);
        result.changed = true;
        return result;
    }

    if (type == "hype-train-progression")
    {
        const auto id = data.value("id").toString();
        if (this->endedHypeTrains_.contains(id))
        {
            return result;
        }
        if (!this->hypeTrain_ ||
            (!id.isEmpty() && this->hypeTrain_->id != id))
        {
            result.needsSnapshot = true;
            return result;
        }
        applyHypeTrainProgress(*this->hypeTrain_,
                               data.value("progress").toObject(), now);
        applyTreasureDetails(*this->hypeTrain_,
                             data.value("treasureTrainDetails").toObject());
        result.changed = true;
        return result;
    }

    if (type == "hype-train-end")
    {
        // {id, ended_at, ending_reason, participation_totals, rewards}. No
        // final numbers, so the last progress we saw is what gets announced.
        const auto id = data.value("id").toString();
        if (this->hypeTrain_ && !id.isEmpty() && this->hypeTrain_->id != id)
        {
            // Not the train we know of. Ours is out of date.
            this->endedHypeTrains_.insert(id);
            result.needsSnapshot = true;
            return result;
        }
        if (this->hypeTrain_)
        {
            auto latest = parseHypeTrainExecution(
                data.value("hype_train").toObject(), now);
            if (latest && latest->id == this->hypeTrain_->id)
            {
                *this->hypeTrain_ = *latest;
            }
            applyHypeTrainProgress(*this->hypeTrain_,
                                   data.value("progress").toObject(), now);
        }
        this->endHypeTrain(
            result, pick(data, {"ending_reason", "endingReason", "end_reason",
                                "endReason"})
                        .toString());
        return result;
    }

    // hype-train-approaching, -conductor-update, -cooldown-expiration, ...
    return result;
}

ChannelEventsTracker::Result ChannelEventsTracker::applyPollMessage(
    const QString &type, const QJsonObject &data, const QDateTime &now)
{
    Result result;
    auto poll = channelevents::parsePoll(data.value("poll").toObject(), now);
    if (!poll)
    {
        return result;
    }

    const bool sameAsCurrent = this->poll_ && this->poll_->id == poll->id;

    if (type == "POLL_ARCHIVE" || poll->status == PollStatus::Archived ||
        poll->status == PollStatus::Moderated)
    {
        if (sameAsCurrent)
        {
            this->poll_.reset();
            result.changed = true;
        }
        return result;
    }

    if (type == "POLL_COMPLETE" && poll->status == PollStatus::Active)
    {
        poll->status = PollStatus::Completed;
    }
    else if (type == "POLL_TERMINATE" && poll->status == PollStatus::Active)
    {
        poll->status = PollStatus::Terminated;
    }

    // Already announced and gone, Twitch is repeating itself.
    if (poll->isEnded() && !sameAsCurrent &&
        this->announced_.contains("poll:" + poll->id))
    {
        return result;
    }

    if (poll->isEnded())
    {
        poll->endedAt = sameAsCurrent && this->poll_->endedAt.isValid()
                            ? this->poll_->endedAt
                            : now;
        if (this->markAnnounced("poll:" + poll->id))
        {
            result.endedPoll = *poll;
        }
    }

    this->poll_ = std::move(poll);
    result.changed = true;
    return result;
}

ChannelEventsTracker::Result ChannelEventsTracker::applyPredictionMessage(
    const QString & /*type*/, const QJsonObject &data, const QDateTime &now)
{
    Result result;
    const auto serverNow =
        channelpoints::parseTwitchTime(data.value("timestamp").toString());
    auto prediction = channelevents::parsePrediction(
        data.value("event").toObject(), now, serverNow);
    if (!prediction)
    {
        return result;
    }

    const bool sameAsCurrent =
        this->prediction_ && this->prediction_->id == prediction->id;

    // Updates arrive out of order, e.g. an ACTIVE one for a last-second
    // prediction after LOCKED, or CANCEL_PENDING after CANCELED. They are
    // older than what we have, so don't let them move the status back.
    if (sameAsCurrent && predictionStage(prediction->status) <
                             predictionStage(this->prediction_->status))
    {
        return result;
    }

    if (prediction->isEnded() && !sameAsCurrent &&
        this->announced_.contains("prediction:" + prediction->id))
    {
        return result;
    }

    if (prediction->isEnded())
    {
        prediction->endedAt =
            sameAsCurrent && this->prediction_->endedAt.isValid()
                ? this->prediction_->endedAt
                : now;
        if (this->markAnnounced("prediction:" + prediction->id))
        {
            result.endedPrediction = *prediction;
        }
    }

    this->prediction_ = std::move(prediction);
    result.changed = true;
    return result;
}

ChannelEventsTracker::Result ChannelEventsTracker::applySnapshot(
    const channelevents::Snapshot &snapshot, const QDateTime & /*now*/)
{
    Result result;
    result.changed = true;

    if (snapshot.hypeTrain &&
        !this->endedHypeTrains_.contains(snapshot.hypeTrain->id))
    {
        this->hypeTrain_ = snapshot.hypeTrain;
    }
    else if (!snapshot.hypeTrain)
    {
        this->hypeTrain_.reset();
    }

    if (snapshot.poll && snapshot.poll->isActive())
    {
        this->poll_ = snapshot.poll;
    }
    else if (this->poll_ && this->poll_->isActive())
    {
        this->poll_.reset();
    }

    if (snapshot.prediction && !snapshot.prediction->isEnded() &&
        !this->announced_.contains("prediction:" + snapshot.prediction->id))
    {
        this->prediction_ = snapshot.prediction;
    }
    else if (this->prediction_ && !this->prediction_->isEnded())
    {
        this->prediction_.reset();
    }

    return result;
}

bool ChannelEventsTracker::expire(const QDateTime &now)
{
    using channelevents::RESULT_DISPLAY_SECONDS;

    bool changed = false;

    if (this->hypeTrain_ && this->hypeTrain_->expiresAt.isValid() &&
        this->hypeTrain_->expiresAt.addSecs(HYPE_TRAIN_EXPIRY_GRACE_SECONDS) <
            now)
    {
        this->hypeTrain_.reset();
        changed = true;
    }

    if (this->poll_)
    {
        const bool resultsShown =
            this->poll_->isEnded() &&
            this->poll_->endedAt.addSecs(RESULT_DISPLAY_SECONDS) < now;
        const bool overdue =
            this->poll_->isActive() && this->poll_->endsAt.isValid() &&
            this->poll_->endsAt.addSecs(POLL_EXPIRY_GRACE_SECONDS) < now;
        if (resultsShown || overdue)
        {
            this->poll_.reset();
            changed = true;
        }
    }

    if (this->prediction_ && this->prediction_->isOpen() &&
        this->prediction_->locksAt.isValid() &&
        this->prediction_->locksAt.addSecs(PREDICTION_LOCK_GRACE_SECONDS) < now)
    {
        // The window is over. Twitch's LOCKED update was lost or is late.
        this->prediction_->status = PredictionStatus::Locked;
        this->prediction_->locksAt = {};
        changed = true;
    }

    if (this->prediction_ && this->prediction_->isEnded() &&
        this->prediction_->endedAt.addSecs(RESULT_DISPLAY_SECONDS) < now)
    {
        this->prediction_.reset();
        changed = true;
    }

    return changed;
}

const std::optional<HypeTrainState> &ChannelEventsTracker::hypeTrain() const
{
    return this->hypeTrain_;
}

const std::optional<PollState> &ChannelEventsTracker::poll() const
{
    return this->poll_;
}

const std::optional<PredictionState> &ChannelEventsTracker::prediction() const
{
    return this->prediction_;
}

void ChannelEventsTracker::recordPollVote(const QString &pollId,
                                          const QString &choiceId)
{
    this->pollVotes_[pollId].insert(choiceId);
}

QSet<QString> ChannelEventsTracker::pollVotes(const QString &pollId) const
{
    return this->pollVotes_.value(pollId);
}

void ChannelEventsTracker::recordPrediction(const QString &eventId,
                                            const QString &outcomeId,
                                            std::int64_t points)
{
    auto &own = this->predictions_[eventId];
    if (own.outcomeId != outcomeId)
    {
        own.outcomeId = outcomeId;
        own.points = 0;
    }
    own.points += points;
}

std::optional<OwnPrediction> ChannelEventsTracker::ownPrediction(
    const QString &eventId) const
{
    auto it = this->predictions_.find(eventId);
    if (it == this->predictions_.end())
    {
        return std::nullopt;
    }
    return it.value();
}

bool ChannelEventsTracker::markAnnounced(const QString &key)
{
    if (this->announced_.contains(key))
    {
        return false;
    }
    this->announced_.insert(key);
    return true;
}

void ChannelEventsTracker::endHypeTrain(Result &result, const QString &reason)
{
    if (!this->hypeTrain_)
    {
        return;
    }

    auto train = std::move(*this->hypeTrain_);
    this->hypeTrain_.reset();
    train.endReason = reason;
    this->endedHypeTrains_.insert(train.id);
    if (this->markAnnounced("hype:" + train.id))
    {
        result.endedHypeTrain = std::move(train);
    }
    result.changed = true;
}

}  // namespace chatterino
