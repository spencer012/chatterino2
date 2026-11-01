// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

/// Hype trains, polls and predictions of a channel.
///
/// Live updates come from PubSub (`hype-train-events-v2`, `polls`,
/// `predictions-channel-v1`), the state at the time a channel is joined from an
/// unauthenticated GQL query. Voting and predicting go through
/// ChannelPointsController, since they need its first-party token.
///
/// Everything in here is free of side effects so it can be unit tested.
namespace chatterino {

struct HelixPinnedChatMessage;

struct HypeTrainState {
    QString id;
    int level = 1;
    /// Points towards the next level.
    int progress = 0;
    /// Points needed for the next level.
    int goal = 0;
    int total = 0;
    /// Local clock.
    QDateTime expiresAt;
    bool isTreasureTrain = false;
    bool isGoldenKappaTrain = false;
    bool isSharedTrain = false;
    int treasureDiscountPercent = 0;
    int treasureDiscountLevel = 0;
    bool treasureDiscountReached = false;
    /// The channel's creator color. Invalid if the channel doesn't use one.
    QColor color;
    int allTimeHighLevel = 0;
    QString endReason;

    /// "Treasure Train", "Golden Kappa Train", "Shared Hype Train" or
    /// "Hype Train".
    [[nodiscard]] QString kindName() const;
    /// Progress towards the next level, 0-100.
    [[nodiscard]] int percent() const;
};

enum class PollStatus : std::uint8_t {
    Active,
    Completed,
    /// Ended early by the broadcaster or a moderator.
    Terminated,
    /// Removed from view after it ended.
    Archived,
    Moderated,
    Unknown,
};

struct PollChoice {
    QString id;
    QString title;
    std::int64_t votes = 0;
    int voters = 0;
};

struct PollState {
    QString id;
    QString title;
    PollStatus status = PollStatus::Unknown;
    /// Local clock. Only meaningful while the poll is active.
    QDateTime endsAt;
    int durationSeconds = 0;
    std::int64_t totalVotes = 0;
    int totalVoters = 0;
    bool channelPointsVotingEnabled = false;
    int channelPointsCost = 0;
    bool subscriberOnly = false;
    bool multichoice = false;
    std::vector<PollChoice> choices;
    /// Local clock, set when we saw the poll end.
    QDateTime endedAt;

    [[nodiscard]] bool isActive() const;
    [[nodiscard]] bool isEnded() const;
    /// Share of all votes, 0-100.
    [[nodiscard]] int percent(const PollChoice &choice) const;
    /// The choices with the most votes. More than one on a tie, none if
    /// nobody voted.
    [[nodiscard]] std::vector<const PollChoice *> leaders() const;
};

enum class PredictionStatus : std::uint8_t {
    Active,
    Locked,
    ResolvePending,
    Resolved,
    CancelPending,
    Canceled,
    Unknown,
};

struct PredictionOutcome {
    QString id;
    QString title;
    /// "BLUE" or "PINK".
    QString color;
    std::int64_t points = 0;
    int users = 0;
};

struct PredictionState {
    QString id;
    QString title;
    PredictionStatus status = PredictionStatus::Unknown;
    /// Local clock. When predictions close, while the prediction is active.
    QDateTime locksAt;
    QString winningOutcomeId;
    std::vector<PredictionOutcome> outcomes;
    /// Local clock, set when we saw the prediction end.
    QDateTime endedAt;

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool isEnded() const;
    [[nodiscard]] std::int64_t totalPoints() const;
    [[nodiscard]] int totalUsers() const;
    /// Share of all points, 0-100.
    [[nodiscard]] int percent(const PredictionOutcome &outcome) const;
    /// Payout per point if this outcome wins, e.g. 2.5 for "1:2.5".
    /// 0 if nobody predicted this outcome.
    [[nodiscard]] double payoutRatio(const PredictionOutcome &outcome) const;
    [[nodiscard]] const PredictionOutcome *findOutcome(const QString &id) const;
};

/// A prediction made from this Chatterino session.
struct OwnPrediction {
    QString outcomeId;
    std::int64_t points = 0;
};

namespace channelevents {

/// How long a finished poll or prediction keeps showing its results.
constexpr int RESULT_DISPLAY_SECONDS = 30;
/// Twitch's limit for a single prediction.
constexpr int MAX_PREDICTION_POINTS = 250000;

PollStatus parsePollStatus(const QString &value);
PredictionStatus parsePredictionStatus(const QString &value);
QString describePredictionStatus(PredictionStatus status);

/// Parses a `HypeTrainExecution` (GQL, `hype-train-start` data,
/// `hype-train-level-up` `hype_train`).
std::optional<HypeTrainState> parseHypeTrainExecution(const QJsonObject &exec,
                                                      const QDateTime &now);
/// Applies the snake_case `progress` object of `hype-train-progression` and
/// `hype-train-level-up` messages.
void applyHypeTrainProgress(HypeTrainState &state, const QJsonObject &progress,
                            const QDateTime &now);
/// Applies `treasureTrainDetails` (either casing).
void applyTreasureDetails(HypeTrainState &state, const QJsonObject &details);

/// Parses a poll from PubSub (`data.poll`, snake_case) or GQL (camelCase).
std::optional<PollState> parsePoll(const QJsonObject &poll,
                                   const QDateTime &now);

/// Parses a prediction event from PubSub (`data.event`, snake_case) or GQL
/// (camelCase).
///
/// @param serverNow Twitch's time of the message, if known. Used to work out
///        when predictions lock without depending on the local clock.
std::optional<PredictionState> parsePrediction(
    const QJsonObject &event, const QDateTime &now,
    const QDateTime &serverNow = {});

// ---------------------------------------------------------------------------
// GQL

/// Unauthenticated query for the current hype train, poll and prediction.
QJsonObject buildSnapshotRequest(const QString &channelLogin);

struct Snapshot {
    QString channelId;
    std::optional<HypeTrainState> hypeTrain;
    std::optional<PollState> poll;
    std::optional<PredictionState> prediction;
};

std::optional<Snapshot> parseSnapshot(const QJsonObject &root,
                                      const QDateTime &now, QString &error);

/// Unauthenticated query for the channel's pinned message. Helix's chat/pins
/// only answers the broadcaster and mods.
QJsonObject buildPinnedMessageRequest(const QString &channelLogin);

/// The current mod-pinned message. nullopt with an empty `error` if nothing is
/// pinned, with `error` set if the response is unusable.
std::optional<HelixPinnedChatMessage> parsePinnedMessage(
    const QJsonObject &root, const QDateTime &now, QString &error);

/// @param inlineQuery Send the query text instead of the website's persisted
///        query hash (fallback for PersistedQueryNotFound).
QJsonObject buildVoteRequest(const QString &pollId, const QString &choiceId,
                             const QString &userId, const QString &voteId,
                             bool inlineQuery);
QJsonObject buildPredictionRequest(const QString &eventId,
                                   const QString &outcomeId,
                                   std::int64_t points,
                                   const QString &transactionId,
                                   bool inlineQuery);

struct ActionResponse {
    bool ok = false;
    QString errorCode;
};

/// Parses a `VoteInPoll` response.
std::optional<ActionResponse> parseVoteResponse(const QJsonObject &root,
                                                QString &error);
/// Parses a `MakePrediction` response.
std::optional<ActionResponse> parsePredictionResponse(const QJsonObject &root,
                                                      QString &error);

QString describeVoteError(const QString &code);
QString describePredictionError(const QString &code);

// ---------------------------------------------------------------------------
// Chat messages

QString formatHypeTrainEnd(const HypeTrainState &train);
QString formatPollEnd(const PollState &poll, const QSet<QString> &ownChoices);
QString formatPredictionEnd(const PredictionState &prediction,
                            const std::optional<OwnPrediction> &own);

/// Short number for tight spaces: 950, 12.3K, 4.6M.
QString shortNumber(std::int64_t value);

}  // namespace channelevents

/// Keeps track of a channel's hype train, poll and prediction.
class ChannelEventsTracker
{
public:
    struct Result {
        bool changed = false;
        /// Our state is missing or out of date, the caller should fetch a
        /// snapshot.
        bool needsSnapshot = false;
        /// Events that just ended. Each one is only reported once.
        std::optional<HypeTrainState> endedHypeTrain;
        std::optional<PollState> endedPoll;
        std::optional<PredictionState> endedPrediction;
    };

    /// @param type PubSub message type, e.g. `hype-train-progression`.
    /// @param data The message's `data`.
    Result applyHypeTrainMessage(const QString &type, const QJsonObject &data,
                                 const QDateTime &now);
    Result applyPollMessage(const QString &type, const QJsonObject &data,
                            const QDateTime &now);
    Result applyPredictionMessage(const QString &type, const QJsonObject &data,
                                  const QDateTime &now);
    /// Replaces our state with a freshly fetched one. Never reports endings,
    /// since we can't tell an ending from something we just missed.
    Result applySnapshot(const channelevents::Snapshot &snapshot,
                         const QDateTime &now);

    /// Drops hype trains that expired and results that were shown long
    /// enough, and closes predictions whose window ran out. Returns true if
    /// anything changed.
    bool expire(const QDateTime &now);

    [[nodiscard]] const std::optional<HypeTrainState> &hypeTrain() const;
    [[nodiscard]] const std::optional<PollState> &poll() const;
    [[nodiscard]] const std::optional<PredictionState> &prediction() const;

    void recordPollVote(const QString &pollId, const QString &choiceId);
    [[nodiscard]] QSet<QString> pollVotes(const QString &pollId) const;
    void recordPrediction(const QString &eventId, const QString &outcomeId,
                          std::int64_t points);
    [[nodiscard]] std::optional<OwnPrediction> ownPrediction(
        const QString &eventId) const;

private:
    /// Returns true the first time it's called for a key.
    bool markAnnounced(const QString &key);
    void endHypeTrain(Result &result, const QString &reason);

    std::optional<HypeTrainState> hypeTrain_;
    std::optional<PollState> poll_;
    std::optional<PredictionState> prediction_;
    QSet<QString> announced_;
    QSet<QString> endedHypeTrains_;
    QHash<QString, QSet<QString>> pollVotes_;
    QHash<QString, OwnPrediction> predictions_;
};

}  // namespace chatterino
