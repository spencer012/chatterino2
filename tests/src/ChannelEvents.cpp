// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/ChannelEvents.hpp"

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "providers/twitch/api/Helix.hpp"

#include "Test.hpp"

#include <QJsonDocument>
#include <QLocale>

using namespace chatterino;
using namespace chatterino::channelevents;

namespace {

QJsonObject parseJson(const char *json)
{
    QJsonParseError error{};
    auto doc = QJsonDocument::fromJson(QByteArray(json), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << error.errorString();
    return doc.object();
}

/// `data` of a PubSub message (`{"type": ..., "data": {...}}`).
QJsonObject dataOf(const char *message)
{
    return parseJson(message).value("data").toObject();
}

QString num(std::int64_t value)
{
    return QLocale().toString(qlonglong(value));
}

const QDateTime NOW =
    QDateTime::fromString("2026-10-25T03:40:42Z", Qt::ISODate);

// The PubSub messages below are trimmed from real hype-train-events-v2 and
// predictions-channel-v1 messages.

const char *TREASURE_LEVEL_UP = R"json({"type": "hype-train-level-up", "data": {
  "time_to_expire": 1792899941000,
  "progress": {"level": {"value": 9, "goal": 32300}, "value": 2400,
               "goal": 7200, "total": 27500, "remaining_seconds": 299},
  "hype_train": {
    "__typename": "HypeTrainExecution",
    "id": "fe834e9b-e3ef-4752-b504-8edb0d13f56d",
    "startedAt": "2026-10-25T03:39:30.179084193Z",
    "expiresAt": "2026-10-25T03:45:41.964166624Z",
    "endReason": "IN_PROGRESS",
    "isGoldenKappaTrain": false,
    "progress": {"goal": 7200, "progression": 2400, "total": 27500,
                 "level": {"value": 9, "goal": 32300}},
    "config": {"willUseCreatorColor": true, "primaryHexColor": "BF00C2"},
    "allTimeHigh": {"level": {"value": 12}},
    "sharedHypeTrainDetails": null,
    "isTreasureTrain": true,
    "treasureTrainDetails": {"__typename": "TreasureTrainDetails",
      "discountPercentage": 35, "discountLevelThreshold": 2,
      "status": "REACHED"}
  },
  "is_boost_train": false}})json";

const char *TREASURE_PROGRESSION = R"json({"type": "hype-train-progression", "data": {
  "id": "fe834e9b-e3ef-4752-b504-8edb0d13f56d",
  "user_id": "10000001", "user_login": "testviewer",
  "action": "TIER_1_GIFTED_SUB", "source": "SUBS", "quantity": 51,
  "progress": {"level": {"value": 9, "goal": 32300}, "value": 4900,
               "goal": 7200, "total": 30000, "remaining_seconds": 250},
  "expires_at": "2026-10-25T03:45:41.964166624Z",
  "is_boost_train": false,
  "treasureTrainDetails": {"__typename": "TreasureTrainDetails",
    "discountPercentage": 35, "discountLevelThreshold": 2, "status": "REACHED",
    "discount_percentage": 35, "discount_level_threshold": 2,
    "discount_status": "REACHED"}}})json";

const char *TREASURE_END = R"json({"type": "hype-train-end", "data": {
  "id": "fe834e9b-e3ef-4752-b504-8edb0d13f56d", "ended_at": 1792900156000,
  "ending_reason": "COMPLETED",
  "participation_totals": [{"source": "SUBS", "action": "TIER_1_GIFTED_SUB",
                            "quantity": 77}],
  "rewards": [], "shared_train_details": null, "is_boost_train": false}})json";

const char *GOLDEN_KAPPA_START = R"json({"type": "hype-train-start", "data": {
  "__typename": "HypeTrainExecution",
  "id": "f96191a3-f7ac-41e6-9232-85f20e62766a",
  "startedAt": "2026-10-25T03:46:33.589665662Z",
  "expiresAt": "2026-10-25T03:51:33.589665662Z",
  "endReason": "IN_PROGRESS", "isGoldenKappaTrain": true,
  "progress": {"goal": 9900, "progression": 3700, "total": 28100,
               "level": {"value": 7, "goal": 34300}},
  "config": {"willUseCreatorColor": true, "primaryHexColor": "25FF8D"},
  "allTimeHigh": {"level": {"value": 24}},
  "sharedHypeTrainDetails": null, "isTreasureTrain": false,
  "variantTrainDetails": {"__typename": "GoldenKappaTrainDetails",
                          "id": "f96191a3-f7ac-41e6-9232-85f20e62766a"}}})json";

const char *LEVEL_ONE_START = R"json({"type": "hype-train-start", "data": {
  "id": "9d108278-67ec-4e11-a857-4b34c76ce23d",
  "expiresAt": "2026-10-25T03:55:00.219316024Z",
  "endReason": "IN_PROGRESS", "isGoldenKappaTrain": false,
  "progress": {"goal": 1600, "progression": 1000, "total": 1000,
               "level": {"value": 1, "goal": 1600}},
  "config": {"willUseCreatorColor": true, "primaryHexColor": null},
  "allTimeHigh": {"level": {"value": 41}},
  "sharedHypeTrainDetails": null, "isTreasureTrain": false,
  "variantTrainDetails": null}})json";

const char *LEVEL_ONE_PROGRESSION = R"json({"type": "hype-train-progression", "data": {
  "id": "9d108278-67ec-4e11-a857-4b34c76ce23d",
  "progress": {"level": {"value": 1, "goal": 1600}, "value": 1503,
               "goal": 1600, "total": 1503, "remaining_seconds": 207},
  "treasureTrainDetails": null}})json";

const char *LEVEL_ONE_END = R"json({"type": "hype-train-end", "data": {
  "id": "9d108278-67ec-4e11-a857-4b34c76ce23d", "ended_at": 1792900500000,
  "ending_reason": "EXPIRED", "rewards": [], "shared_train_details": null,
  "is_boost_train": false}})json";

const char *PREDICTION_CREATED = R"json({"type": "event-created", "data": {
  "timestamp": "2026-10-25T03:41:30.428125358Z",
  "event": {"id": "a00d46db-dbba-4dc1-b771-d73c2bec18ae",
    "channel_id": "10000003",
    "created_at": "2026-10-25T03:41:30.416491223Z",
    "ended_at": null, "locked_at": null,
    "outcomes": [
      {"id": "30d9d77a", "color": "BLUE", "title": "YES (COPE)",
       "total_points": 0, "total_users": 0},
      {"id": "33a90ec4", "color": "PINK", "title": "NOPE",
       "total_points": 0, "total_users": 0}],
    "prediction_window_seconds": 120, "status": "ACTIVE",
    "title": "Does Frankie win?", "winning_outcome_id": null}}})json";

const char *PREDICTION_LOCKED = R"json({"type": "event-updated", "data": {
  "timestamp": "2026-10-25T03:43:30.522659101Z",
  "event": {"id": "a00d46db-dbba-4dc1-b771-d73c2bec18ae",
    "created_at": "2026-10-25T03:41:30.416491223Z",
    "locked_at": "2026-10-25T03:43:30.513804254Z",
    "outcomes": [
      {"id": "30d9d77a", "color": "BLUE", "title": "YES (COPE)",
       "total_points": 15783, "total_users": 13},
      {"id": "33a90ec4", "color": "PINK", "title": "NOPE",
       "total_points": 26506, "total_users": 7}],
    "prediction_window_seconds": 120, "status": "LOCKED",
    "title": "Does Frankie win?", "winning_outcome_id": null}}})json";

const char *PREDICTION_RESOLVED = R"json({"type": "event-updated", "data": {
  "timestamp": "2026-10-25T03:58:30.315334087Z",
  "event": {"id": "a00d46db-dbba-4dc1-b771-d73c2bec18ae",
    "created_at": "2026-10-25T03:41:30.416491223Z",
    "ended_at": "2026-10-25T03:58:24.595144975Z",
    "locked_at": "2026-10-25T03:43:30.513804254Z",
    "outcomes": [
      {"id": "30d9d77a", "color": "BLUE", "title": "YES (COPE)",
       "total_points": 15783, "total_users": 13},
      {"id": "33a90ec4", "color": "PINK", "title": "NOPE",
       "total_points": 26506, "total_users": 7}],
    "prediction_window_seconds": 120, "status": "RESOLVED",
    "title": "Does Frankie win?", "winning_outcome_id": "33a90ec4"}}})json";

// Seen in this order: CANCELED, then a late CANCEL_PENDING.
const char *PREDICTION_CANCELED = R"json({"type": "event-updated", "data": {
  "timestamp": "2026-10-25T03:44:09.96261244Z",
  "event": {"id": "6f05cdb3-8fa3-4431-bd0a-a1915a18ebd8",
    "created_at": "2026-10-25T03:44:01.985539952Z",
    "ended_at": "2026-10-25T03:44:09.151959281Z",
    "outcomes": [
      {"id": "f755d96b", "color": "BLUE", "title": "Clix & Ron",
       "total_points": 9913, "total_users": 4},
      {"id": "3aaf6a61", "color": "PINK", "title": "Laiys & Bucke",
       "total_points": 0, "total_users": 0}],
    "prediction_window_seconds": 120, "status": "CANCELED",
    "title": "WHO WINS? (DOUBLE DOUBLE FULL)", "winning_outcome_id": null}}})json";

const char *PREDICTION_LATE_CANCEL_PENDING = R"json({"type": "event-updated", "data": {
  "timestamp": "2026-10-25T03:44:10.1Z",
  "event": {"id": "6f05cdb3-8fa3-4431-bd0a-a1915a18ebd8",
    "created_at": "2026-10-25T03:44:01.985539952Z",
    "outcomes": [
      {"id": "f755d96b", "color": "BLUE", "title": "Clix & Ron",
       "total_points": 9913, "total_users": 4},
      {"id": "3aaf6a61", "color": "PINK", "title": "Laiys & Bucke",
       "total_points": 0, "total_users": 0}],
    "prediction_window_seconds": 120, "status": "CANCEL_PENDING",
    "title": "WHO WINS? (DOUBLE DOUBLE FULL)", "winning_outcome_id": null}}})json";

// Real `polls` messages (trimmed, title changed).
const char *REAL_POLL_CREATE = R"json({"type": "POLL_CREATE", "data": {"poll": {
  "poll_id": "ba40b7f3-a067-4000-8ec7-618ab0867d1f", "owned_by": "10000004",
  "created_by": "10000005", "title": "Add the update",
  "started_at": "2026-10-25T04:57:13.940584942Z", "ended_at": null,
  "ended_by": null, "duration_seconds": 60,
  "settings": {"multi_choice": {"is_enabled": true},
               "bits_votes": {"is_enabled": false, "cost": 0},
               "channel_points_votes": {"is_enabled": false, "cost": 0}},
  "status": "ACTIVE",
  "choices": [
    {"choice_id": "cf5ce540-d8d5-40d3-be45-522477b243af", "title": "Yes",
     "votes": {"total": 0, "bits": 0, "channel_points": 0, "base": 0,
               "granted": 0},
     "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 0},
    {"choice_id": "03f4a088-14d2-46a5-b540-6fc74e437cf4", "title": "No",
     "votes": {"total": 0, "bits": 0, "channel_points": 0, "base": 0,
               "granted": 0},
     "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 0}],
  "votes": {"total": 0, "bits": 0, "channel_points": 0, "base": 0,
            "granted": 0},
  "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 0,
  "remaining_duration_milliseconds": 59981, "top_contributor": null}}})json";

const char *REAL_POLL_COMPLETE = R"json({"type": "POLL_COMPLETE", "data": {"poll": {
  "poll_id": "ba40b7f3-a067-4000-8ec7-618ab0867d1f", "owned_by": "10000004",
  "title": "Add the update",
  "started_at": "2026-10-25T04:57:13.940584942Z",
  "ended_at": "2026-10-25T04:58:13.940584942Z", "ended_by": null,
  "duration_seconds": 60,
  "settings": {"multi_choice": {"is_enabled": true},
               "bits_votes": {"is_enabled": false, "cost": 0},
               "channel_points_votes": {"is_enabled": false, "cost": 0}},
  "status": "COMPLETED",
  "choices": [
    {"choice_id": "cf5ce540-d8d5-40d3-be45-522477b243af", "title": "Yes",
     "votes": {"total": 270, "bits": 0, "channel_points": 0, "base": 270,
               "granted": 0},
     "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 270},
    {"choice_id": "03f4a088-14d2-46a5-b540-6fc74e437cf4", "title": "No",
     "votes": {"total": 42, "bits": 0, "channel_points": 0, "base": 42,
               "granted": 0},
     "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 42}],
  "votes": {"total": 312, "bits": 0, "channel_points": 0, "base": 312,
            "granted": 0},
  "tokens": {"bits": 0, "channel_points": 0}, "total_voters": 312,
  "remaining_duration_milliseconds": 0, "top_contributor": null}}})json";

const char *REAL_POLL_ARCHIVE = R"json({"type": "POLL_ARCHIVE", "data": {"poll": {
  "poll_id": "ba40b7f3-a067-4000-8ec7-618ab0867d1f", "title": "Add the update",
  "status": "ARCHIVED", "choices": [], "votes": {"total": 312},
  "remaining_duration_milliseconds": 0}}})json";

// Hand-written in the same shape, covering settings the real one doesn't use.
const char *POLL_UPDATE = R"json({"type": "POLL_UPDATE", "data": {"poll": {
  "poll_id": "poll-1", "owned_by": "11148817", "title": "Best snack?",
  "status": "ACTIVE", "duration_seconds": 120,
  "remaining_duration_milliseconds": 60000,
  "settings": {
    "multi_choice": {"is_enabled": false},
    "subscriber_only": {"is_enabled": false},
    "channel_points_votes": {"is_enabled": true, "cost": 100}
  },
  "choices": [
    {"choice_id": "c1", "title": "Chips", "votes": {"total": 6},
     "total_voters": 5},
    {"choice_id": "c2", "title": "Fruit", "votes": {"total": 3},
     "total_voters": 3},
    {"choice_id": "c3", "title": "Nothing", "votes": {"total": 1},
     "total_voters": 1}
  ],
  "votes": {"total": 10}, "total_voters": 9}}})json";

const char *POLL_COMPLETE = R"json({"type": "POLL_COMPLETE", "data": {"poll": {
  "poll_id": "poll-1", "title": "Best snack?", "status": "COMPLETED",
  "duration_seconds": 120, "remaining_duration_milliseconds": 0,
  "choices": [
    {"choice_id": "c1", "title": "Chips", "votes": {"total": 7},
     "total_voters": 6},
    {"choice_id": "c2", "title": "Fruit", "votes": {"total": 3},
     "total_voters": 3}
  ],
  "votes": {"total": 10}, "total_voters": 9}}})json";

const char *POLL_ARCHIVE = R"json({"type": "POLL_ARCHIVE", "data": {"poll": {
  "poll_id": "poll-1", "title": "Best snack?", "status": "ARCHIVED",
  "choices": []}}})json";

const char *SNAPSHOT_RESPONSE = R"json({"data": {"user": {
  "id": "11148817",
  "viewablePoll": {
    "id": "poll-2", "title": "Next game?", "status": "ACTIVE",
    "durationSeconds": 300, "remainingDurationMilliseconds": 90000,
    "totalVoters": 4, "votes": {"total": 4},
    "settings": {"multichoice": {"isEnabled": true},
                 "subscriberOnly": {"isEnabled": false},
                 "communityPointsVotes": {"isEnabled": false, "cost": 0}},
    "choices": [
      {"id": "g1", "title": "Minecraft", "totalVoters": 3,
       "votes": {"total": 3}},
      {"id": "g2", "title": "Celeste", "totalVoters": 1,
       "votes": {"total": 1}}]
  },
  "channel": {
    "hypeTrain": {"execution": {
      "id": "train-9", "expiresAt": "2026-10-25T03:45:00Z",
      "endReason": "IN_PROGRESS", "isGoldenKappaTrain": false,
      "isTreasureTrain": false, "treasureTrainDetails": null,
      "sharedHypeTrainDetails": null,
      "progress": {"goal": 2000, "progression": 500, "total": 4500,
                   "remainingSeconds": 120, "level": {"value": 3}},
      "allTimeHigh": {"level": {"value": 5}},
      "config": {"primaryHexColor": null, "willUseCreatorColor": false}
    }},
    "activePredictionEvents": [],
    "lockedPredictionEvents": [{
      "id": "pred-3", "title": "Win?", "status": "LOCKED",
      "createdAt": "2026-10-25T03:30:00Z", "predictionWindowSeconds": 60,
      "winningOutcome": null,
      "outcomes": [
        {"id": "o1", "title": "Yes", "color": "BLUE", "totalPoints": 300,
         "totalUsers": 3},
        {"id": "o2", "title": "No", "color": "PINK", "totalPoints": 100,
         "totalUsers": 1}]
    }]
  }
}}})json";

}  // namespace

TEST(ChannelEvents, ParseTreasureTrain)
{
    const auto data = dataOf(TREASURE_LEVEL_UP);
    auto train = parseHypeTrainExecution(data.value("hype_train").toObject(),
                                         NOW);
    ASSERT_TRUE(train.has_value());
    EXPECT_EQ(train->id, "fe834e9b-e3ef-4752-b504-8edb0d13f56d");
    EXPECT_EQ(train->level, 9);
    EXPECT_EQ(train->progress, 2400);
    EXPECT_EQ(train->goal, 7200);
    EXPECT_EQ(train->percent(), 33);
    EXPECT_TRUE(train->isTreasureTrain);
    EXPECT_EQ(train->kindName(), "Treasure Train");
    EXPECT_EQ(train->treasureDiscountPercent, 35);
    EXPECT_EQ(train->treasureDiscountLevel, 2);
    EXPECT_TRUE(train->treasureDiscountReached);
    EXPECT_EQ(train->color, QColor("#BF00C2"));
    EXPECT_EQ(train->allTimeHighLevel, 12);
    EXPECT_TRUE(train->endReason.isEmpty());
    EXPECT_EQ(train->expiresAt,
              QDateTime::fromString("2026-10-25T03:45:41.964Z", Qt::ISODateWithMs));
}

TEST(ChannelEvents, ParseGoldenKappaTrain)
{
    auto train = parseHypeTrainExecution(dataOf(GOLDEN_KAPPA_START), NOW);
    ASSERT_TRUE(train.has_value());
    EXPECT_TRUE(train->isGoldenKappaTrain);
    EXPECT_FALSE(train->isTreasureTrain);
    EXPECT_EQ(train->kindName(), "Golden Kappa Train");
    EXPECT_EQ(train->level, 7);
    EXPECT_EQ(train->percent(), 37);

    // No creator color without a hex value.
    auto plain = parseHypeTrainExecution(dataOf(LEVEL_ONE_START), NOW);
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(plain->color.isValid());
    EXPECT_EQ(plain->kindName(), "Hype Train");
}

TEST(ChannelEvents, HypeTrainLifecycle)
{
    ChannelEventsTracker tracker;

    // Progression for a train we don't know yet: ask for a snapshot.
    auto result = tracker.applyHypeTrainMessage(
        "hype-train-progression", dataOf(TREASURE_PROGRESSION), NOW);
    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(result.needsSnapshot);
    EXPECT_FALSE(tracker.hypeTrain().has_value());

    result = tracker.applyHypeTrainMessage("hype-train-level-up",
                                           dataOf(TREASURE_LEVEL_UP), NOW);
    EXPECT_TRUE(result.changed);
    ASSERT_TRUE(tracker.hypeTrain().has_value());
    EXPECT_EQ(tracker.hypeTrain()->level, 9);

    result = tracker.applyHypeTrainMessage(
        "hype-train-progression", dataOf(TREASURE_PROGRESSION), NOW);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.needsSnapshot);
    EXPECT_EQ(tracker.hypeTrain()->progress, 4900);
    EXPECT_EQ(tracker.hypeTrain()->total, 30000);
    EXPECT_EQ(tracker.hypeTrain()->percent(), 68);
    EXPECT_EQ(tracker.hypeTrain()->expiresAt, NOW.addSecs(250));

    // Unrelated messages don't change anything.
    result = tracker.applyHypeTrainMessage(
        "hype-train-conductor-update", QJsonObject{{"id", "x"}}, NOW);
    EXPECT_FALSE(result.changed);

    result = tracker.applyHypeTrainMessage("hype-train-end",
                                           dataOf(TREASURE_END), NOW);
    EXPECT_TRUE(result.changed);
    ASSERT_TRUE(result.endedHypeTrain.has_value());
    EXPECT_FALSE(tracker.hypeTrain().has_value());
    EXPECT_EQ(result.endedHypeTrain->endReason, "COMPLETED");
    EXPECT_EQ(formatHypeTrainEnd(*result.endedHypeTrain),
              "Treasure Train ended at level 9, 68% of the way to level 10. "
              "The 35% sub discount was unlocked.");

    // Announced once, and late messages don't bring it back.
    result = tracker.applyHypeTrainMessage("hype-train-end",
                                           dataOf(TREASURE_END), NOW);
    EXPECT_FALSE(result.endedHypeTrain.has_value());
    result = tracker.applyHypeTrainMessage("hype-train-level-up",
                                           dataOf(TREASURE_LEVEL_UP), NOW);
    EXPECT_FALSE(tracker.hypeTrain().has_value());
    result = tracker.applyHypeTrainMessage(
        "hype-train-progression", dataOf(TREASURE_PROGRESSION), NOW);
    EXPECT_FALSE(result.needsSnapshot);
    EXPECT_FALSE(tracker.hypeTrain().has_value());
}

TEST(ChannelEvents, HypeTrainExpiredAtLevelOne)
{
    ChannelEventsTracker tracker;
    tracker.applyHypeTrainMessage("hype-train-start", dataOf(LEVEL_ONE_START),
                                  NOW);
    tracker.applyHypeTrainMessage("hype-train-progression",
                                  dataOf(LEVEL_ONE_PROGRESSION), NOW);
    ASSERT_TRUE(tracker.hypeTrain().has_value());
    EXPECT_FALSE(tracker.hypeTrain()->isTreasureTrain);

    auto result = tracker.applyHypeTrainMessage("hype-train-end",
                                                dataOf(LEVEL_ONE_END), NOW);
    ASSERT_TRUE(result.endedHypeTrain.has_value());
    EXPECT_EQ(formatHypeTrainEnd(*result.endedHypeTrain),
              "Hype Train ended without completing level 1, 93% of the way "
              "there.");
}

TEST(ChannelEvents, HypeTrainEndForOtherTrain)
{
    ChannelEventsTracker tracker;
    tracker.applyHypeTrainMessage("hype-train-start", dataOf(LEVEL_ONE_START),
                                  NOW);
    auto result = tracker.applyHypeTrainMessage("hype-train-end",
                                                dataOf(TREASURE_END), NOW);
    EXPECT_FALSE(result.endedHypeTrain.has_value());
    EXPECT_TRUE(result.needsSnapshot);
    EXPECT_TRUE(tracker.hypeTrain().has_value());
}

TEST(ChannelEvents, HypeTrainRecord)
{
    HypeTrainState train;
    train.level = 13;
    train.progress = 50;
    train.goal = 200;
    train.allTimeHighLevel = 12;
    EXPECT_EQ(formatHypeTrainEnd(train),
              "Hype Train ended at level 13, 25% of the way to level 14. New "
              "record, the previous best was level 12.");

    train.isTreasureTrain = true;
    train.treasureDiscountPercent = 50;
    train.treasureDiscountLevel = 15;
    train.allTimeHighLevel = 20;
    EXPECT_EQ(formatHypeTrainEnd(train),
              "Treasure Train ended at level 13, 25% of the way to level 14. "
              "The 50% sub discount (level 15) wasn't reached.");
}

TEST(ChannelEvents, HypeTrainExpiry)
{
    ChannelEventsTracker tracker;
    tracker.applyHypeTrainMessage("hype-train-progression",
                                  dataOf(LEVEL_ONE_PROGRESSION), NOW);
    tracker.applyHypeTrainMessage("hype-train-start", dataOf(LEVEL_ONE_START),
                                  NOW);
    ASSERT_TRUE(tracker.hypeTrain().has_value());
    const auto expiresAt = tracker.hypeTrain()->expiresAt;

    EXPECT_FALSE(tracker.expire(expiresAt.addSecs(10)));
    EXPECT_TRUE(tracker.hypeTrain().has_value());
    EXPECT_TRUE(tracker.expire(expiresAt.addSecs(31)));
    EXPECT_FALSE(tracker.hypeTrain().has_value());
}

TEST(ChannelEvents, ParsePrediction)
{
    const auto data = dataOf(PREDICTION_CREATED);
    // Our clock is 5s behind Twitch's.
    const auto now = QDateTime::fromString("2026-10-25T03:41:25.428Z",
                                           Qt::ISODateWithMs);
    auto prediction = parsePrediction(
        data.value("event").toObject(), now,
        channelpoints::parseTwitchTime(data.value("timestamp").toString()));
    ASSERT_TRUE(prediction.has_value());
    EXPECT_EQ(prediction->title, "Does Frankie win?");
    EXPECT_TRUE(prediction->isOpen());
    ASSERT_EQ(prediction->outcomes.size(), 2);
    EXPECT_EQ(prediction->outcomes[1].color, "PINK");
    // Locks 120s after creation, on our clock.
    EXPECT_NEAR(now.msecsTo(prediction->locksAt), 120000 - 12, 50);

    auto locked = parsePrediction(dataOf(PREDICTION_LOCKED).value("event").toObject(),
                                  NOW);
    ASSERT_TRUE(locked.has_value());
    EXPECT_EQ(locked->status, PredictionStatus::Locked);
    EXPECT_FALSE(locked->isOpen());
    EXPECT_FALSE(locked->isEnded());
    EXPECT_FALSE(locked->locksAt.isValid());
    EXPECT_EQ(locked->totalPoints(), 42289);
    EXPECT_EQ(locked->totalUsers(), 20);
    EXPECT_EQ(locked->percent(locked->outcomes[0]), 37);
    EXPECT_EQ(locked->percent(locked->outcomes[1]), 63);
    EXPECT_NEAR(locked->payoutRatio(locked->outcomes[1]), 1.5955, 0.001);
    EXPECT_EQ(describePredictionStatus(locked->status), "Locked");
}

TEST(ChannelEvents, PredictionLifecycle)
{
    ChannelEventsTracker tracker;
    auto result = tracker.applyPredictionMessage(
        "event-created", dataOf(PREDICTION_CREATED), NOW);
    EXPECT_TRUE(result.changed);
    ASSERT_TRUE(tracker.prediction().has_value());

    tracker.recordPrediction("a00d46db-dbba-4dc1-b771-d73c2bec18ae", "33a90ec4",
                             1000);
    tracker.recordPrediction("a00d46db-dbba-4dc1-b771-d73c2bec18ae", "33a90ec4",
                             500);
    const auto own =
        tracker.ownPrediction("a00d46db-dbba-4dc1-b771-d73c2bec18ae");
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->points, 1500);

    tracker.applyPredictionMessage("event-updated", dataOf(PREDICTION_LOCKED),
                                   NOW);
    EXPECT_EQ(tracker.prediction()->status, PredictionStatus::Locked);

    result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_RESOLVED), NOW);
    ASSERT_TRUE(result.endedPrediction.has_value());
    EXPECT_EQ(formatPredictionEnd(*result.endedPrediction, own),
              QStringLiteral("Prediction resolved: \"Does Frankie win?\". "
                             "Winner: NOPE. YES (COPE) 37% (%1 points, 13 "
                             "users) · NOPE 63% (%2 points, 7 users). You "
                             "predicted NOPE with %3 points and won about %4.")
                  .arg(num(15783), num(26506), num(1500), num(2393)));

    // Results stay for a while, then go.
    EXPECT_FALSE(tracker.expire(NOW.addSecs(10)));
    EXPECT_TRUE(tracker.prediction().has_value());
    EXPECT_TRUE(tracker.expire(NOW.addSecs(RESULT_DISPLAY_SECONDS + 1)));
    EXPECT_FALSE(tracker.prediction().has_value());

    // A repeated message is neither announced nor shown again.
    result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_RESOLVED), NOW);
    EXPECT_FALSE(result.endedPrediction.has_value());
    EXPECT_FALSE(tracker.prediction().has_value());
}

TEST(ChannelEvents, PredictionCanceledOutOfOrder)
{
    ChannelEventsTracker tracker;
    auto result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_CANCELED), NOW);
    ASSERT_TRUE(result.endedPrediction.has_value());
    EXPECT_EQ(formatPredictionEnd(*result.endedPrediction, std::nullopt),
              "Prediction cancelled: \"WHO WINS? (DOUBLE DOUBLE FULL)\". "
              "Points were refunded.");

    result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_LATE_CANCEL_PENDING), NOW);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(tracker.prediction()->status, PredictionStatus::Canceled);
}

TEST(ChannelEvents, PredictionLateActiveAfterLock)
{
    ChannelEventsTracker tracker;
    tracker.applyPredictionMessage("event-created", dataOf(PREDICTION_CREATED),
                                   NOW);
    tracker.applyPredictionMessage("event-updated", dataOf(PREDICTION_LOCKED),
                                   NOW);
    ASSERT_EQ(tracker.prediction()->status, PredictionStatus::Locked);

    // An update from before the lock, e.g. for a last-second prediction.
    const auto result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_CREATED), NOW);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(tracker.prediction()->status, PredictionStatus::Locked);
    EXPECT_EQ(tracker.prediction()->totalPoints(), 42289);
}

TEST(ChannelEvents, PredictionLocksWhenWindowRunsOut)
{
    ChannelEventsTracker tracker;
    // Our clock matches Twitch's.
    const auto now = QDateTime::fromString("2026-10-25T03:41:30.428Z",
                                           Qt::ISODateWithMs);
    tracker.applyPredictionMessage("event-created", dataOf(PREDICTION_CREATED),
                                   now);
    ASSERT_TRUE(tracker.prediction()->isOpen());

    // Twitch's LOCKED update gets a moment to arrive.
    EXPECT_FALSE(tracker.expire(now.addSecs(121)));
    EXPECT_TRUE(tracker.prediction()->isOpen());

    // It never came; the window is over, so the prediction is locked.
    EXPECT_TRUE(tracker.expire(now.addSecs(123)));
    EXPECT_EQ(tracker.prediction()->status, PredictionStatus::Locked);
    EXPECT_FALSE(tracker.prediction()->locksAt.isValid());
    EXPECT_FALSE(tracker.expire(now.addSecs(124)));

    // A late ACTIVE update doesn't reopen it, the real LOCKED one applies.
    tracker.applyPredictionMessage("event-updated", dataOf(PREDICTION_CREATED),
                                   now.addSecs(125));
    EXPECT_EQ(tracker.prediction()->status, PredictionStatus::Locked);
    const auto result = tracker.applyPredictionMessage(
        "event-updated", dataOf(PREDICTION_LOCKED), now.addSecs(126));
    EXPECT_TRUE(result.changed);
    EXPECT_EQ(tracker.prediction()->totalPoints(), 42289);
}

TEST(ChannelEvents, PredictionLost)
{
    auto prediction = parsePrediction(
        dataOf(PREDICTION_RESOLVED).value("event").toObject(), NOW);
    ASSERT_TRUE(prediction.has_value());
    const auto text =
        formatPredictionEnd(*prediction, OwnPrediction{"30d9d77a", 200});
    EXPECT_TRUE(text.endsWith(" You predicted YES (COPE) and lost 200 points."))
        << text.toStdString();
}

TEST(ChannelEvents, ParsePoll)
{
    auto poll = parsePoll(dataOf(POLL_UPDATE).value("poll").toObject(), NOW);
    ASSERT_TRUE(poll.has_value());
    EXPECT_EQ(poll->id, "poll-1");
    EXPECT_TRUE(poll->isActive());
    EXPECT_EQ(poll->endsAt, NOW.addSecs(60));
    EXPECT_EQ(poll->totalVotes, 10);
    EXPECT_EQ(poll->totalVoters, 9);
    EXPECT_TRUE(poll->channelPointsVotingEnabled);
    EXPECT_EQ(poll->channelPointsCost, 100);
    EXPECT_FALSE(poll->multichoice);
    ASSERT_EQ(poll->choices.size(), 3);
    EXPECT_EQ(poll->percent(poll->choices[0]), 60);
    ASSERT_EQ(poll->leaders().size(), 1);
    EXPECT_EQ(poll->leaders().front()->id, "c1");

    // Tie
    poll->choices[1].votes = 6;
    EXPECT_EQ(poll->leaders().size(), 2);
}

TEST(ChannelEvents, PollLifecycle)
{
    ChannelEventsTracker tracker;
    auto result =
        tracker.applyPollMessage("POLL_UPDATE", dataOf(POLL_UPDATE), NOW);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.endedPoll.has_value());

    tracker.recordPollVote("poll-1", "c2");
    EXPECT_TRUE(tracker.pollVotes("poll-1").contains("c2"));

    result = tracker.applyPollMessage("POLL_COMPLETE", dataOf(POLL_COMPLETE),
                                      NOW);
    ASSERT_TRUE(result.endedPoll.has_value());
    EXPECT_EQ(formatPollEnd(*result.endedPoll, tracker.pollVotes("poll-1")),
              "Poll ended: \"Best snack?\". Winner: Chips. Chips 70% (7 "
              "votes) · Fruit 30% (3 votes). You voted for Fruit.");
    ASSERT_TRUE(tracker.poll().has_value());
    EXPECT_TRUE(tracker.poll()->isEnded());

    // Twitch repeats the end, only announce it once.
    result = tracker.applyPollMessage("POLL_COMPLETE", dataOf(POLL_COMPLETE),
                                      NOW);
    EXPECT_FALSE(result.endedPoll.has_value());

    result =
        tracker.applyPollMessage("POLL_ARCHIVE", dataOf(POLL_ARCHIVE), NOW);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(tracker.poll().has_value());
}

TEST(ChannelEvents, RealPollLifecycle)
{
    ChannelEventsTracker tracker;
    auto result = tracker.applyPollMessage("POLL_CREATE",
                                           dataOf(REAL_POLL_CREATE), NOW);
    EXPECT_TRUE(result.changed);
    ASSERT_TRUE(tracker.poll().has_value());
    EXPECT_TRUE(tracker.poll()->isActive());
    EXPECT_TRUE(tracker.poll()->multichoice);
    EXPECT_FALSE(tracker.poll()->channelPointsVotingEnabled);
    EXPECT_EQ(tracker.poll()->endsAt, NOW.addMSecs(59981));
    EXPECT_EQ(tracker.poll()->durationSeconds, 60);
    EXPECT_TRUE(tracker.poll()->leaders().empty());

    tracker.recordPollVote("ba40b7f3-a067-4000-8ec7-618ab0867d1f",
                           "cf5ce540-d8d5-40d3-be45-522477b243af");

    result = tracker.applyPollMessage("POLL_COMPLETE",
                                      dataOf(REAL_POLL_COMPLETE), NOW);
    ASSERT_TRUE(result.endedPoll.has_value());
    EXPECT_EQ(formatPollEnd(*result.endedPoll,
                            tracker.pollVotes(
                                "ba40b7f3-a067-4000-8ec7-618ab0867d1f")),
              "Poll ended: \"Add the update\". Winner: Yes. Yes 87% (270 "
              "votes) · No 13% (42 votes). You voted for Yes.");

    result = tracker.applyPollMessage("POLL_ARCHIVE",
                                      dataOf(REAL_POLL_ARCHIVE), NOW);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.endedPoll.has_value());
    EXPECT_FALSE(tracker.poll().has_value());
}

TEST(ChannelEvents, PollTerminatedWithoutVotes)
{
    PollState poll;
    poll.id = "p";
    poll.title = "Anyone?";
    poll.status = PollStatus::Terminated;
    poll.choices = {{"a", "A", 0, 0}, {"b", "B", 0, 0}};
    EXPECT_EQ(formatPollEnd(poll, {}),
              "Poll ended early: \"Anyone?\". Nobody voted.");
}

TEST(ChannelEvents, ParseSnapshot)
{
    QString error;
    auto snapshot = parseSnapshot(parseJson(SNAPSHOT_RESPONSE), NOW, error);
    ASSERT_TRUE(snapshot.has_value()) << error.toStdString();
    EXPECT_EQ(snapshot->channelId, "11148817");

    ASSERT_TRUE(snapshot->poll.has_value());
    EXPECT_EQ(snapshot->poll->title, "Next game?");
    EXPECT_TRUE(snapshot->poll->multichoice);
    EXPECT_EQ(snapshot->poll->endsAt, NOW.addSecs(90));
    EXPECT_EQ(snapshot->poll->choices[0].votes, 3);

    ASSERT_TRUE(snapshot->hypeTrain.has_value());
    EXPECT_EQ(snapshot->hypeTrain->level, 3);
    EXPECT_EQ(snapshot->hypeTrain->percent(), 25);
    EXPECT_EQ(snapshot->hypeTrain->expiresAt, NOW.addSecs(120));
    EXPECT_EQ(snapshot->hypeTrain->allTimeHighLevel, 5);

    ASSERT_TRUE(snapshot->prediction.has_value());
    EXPECT_EQ(snapshot->prediction->status, PredictionStatus::Locked);
    EXPECT_EQ(snapshot->prediction->percent(snapshot->prediction->outcomes[0]),
              75);

    ChannelEventsTracker tracker;
    auto result = tracker.applySnapshot(*snapshot, NOW);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.endedHypeTrain || result.endedPoll ||
                 result.endedPrediction);
    EXPECT_TRUE(tracker.hypeTrain() && tracker.poll() && tracker.prediction());

    // An empty snapshot clears what's running.
    tracker.applySnapshot(Snapshot{}, NOW);
    EXPECT_FALSE(tracker.hypeTrain() || tracker.poll() ||
                 tracker.prediction());
}

TEST(ChannelEvents, ParseSnapshotErrors)
{
    QString error;
    EXPECT_FALSE(parseSnapshot(parseJson(R"json({"data": {"user": null}})json"), NOW,
                               error)
                     .has_value());
    EXPECT_EQ(error, "Channel not found");

    EXPECT_FALSE(
        parseSnapshot(
            parseJson(R"json({"errors": [{"message": "service timeout"}]})json"), NOW,
            error)
            .has_value());
    EXPECT_FALSE(error.isEmpty());

    // A train that already ended is ignored.
    auto ended = parseJson(SNAPSHOT_RESPONSE);
    auto text = QJsonDocument(ended).toJson();
    text.replace(R"json("endReason": "IN_PROGRESS")json", R"json("endReason": "COMPLETED")json");
    text.replace(R"json("endReason":"IN_PROGRESS")json", R"json("endReason":"COMPLETED")json");
    auto snapshot = parseSnapshot(QJsonDocument::fromJson(text).object(), NOW,
                                  error);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(snapshot->hypeTrain.has_value());
}

TEST(ChannelEvents, BuildRequests)
{
    // Persisted query, like the website.
    const auto vote =
        buildVoteRequest("poll-1", "c1", "user-1", "vote-1", false);
    EXPECT_EQ(vote.value("operationName").toString(),
              "ChannelPollContext_VoteInPoll");
    EXPECT_FALSE(vote.contains("query"));
    EXPECT_EQ(vote.value("extensions")
                  .toObject()
                  .value("persistedQuery")
                  .toObject()
                  .value("sha256Hash")
                  .toString(),
              "1280e27b0f3c7ae60b5714bd569771ea50635778473182e6e959e2dcfcc16e3c");

    const auto inlineVote =
        buildVoteRequest("poll-1", "c1", "user-1", "vote-1", true);
    EXPECT_EQ(inlineVote.value("operationName").toString(), "VoteInPoll");
    EXPECT_TRUE(inlineVote.value("query").toString().contains("voteInPoll"));
    EXPECT_FALSE(inlineVote.contains("extensions"));

    for (const auto &request : {vote, inlineVote})
    {
        const auto voteInput =
            request.value("variables").toObject().value("input").toObject();
        EXPECT_EQ(voteInput.value("pollID").toString(), "poll-1");
        EXPECT_EQ(voteInput.value("choiceID").toString(), "c1");
        EXPECT_EQ(voteInput.value("userID").toString(), "user-1");
        EXPECT_EQ(voteInput.value("voteID").toString(), "vote-1");
        // Only the free vote (like the website's tokens: null), never paid
        // extra votes.
        EXPECT_TRUE(voteInput.contains("tokens"));
        EXPECT_TRUE(voteInput.value("tokens").isNull());
    }

    const auto persisted =
        buildPredictionRequest("event-1", "o1", 250, "tx-1", false);
    EXPECT_FALSE(persisted.contains("query"));
    EXPECT_EQ(persisted.value("extensions")
                  .toObject()
                  .value("persistedQuery")
                  .toObject()
                  .value("sha256Hash")
                  .toString()
                  .size(),
              64);
    const auto input =
        persisted.value("variables").toObject().value("input").toObject();
    EXPECT_EQ(input.value("eventID").toString(), "event-1");
    EXPECT_EQ(input.value("outcomeID").toString(), "o1");
    EXPECT_EQ(input.value("points").toInteger(), 250);
    EXPECT_EQ(input.value("transactionID").toString(), "tx-1");

    const auto inlined =
        buildPredictionRequest("event-1", "o1", 250, "tx-1", true);
    EXPECT_TRUE(inlined.value("query").toString().contains("makePrediction"));
    EXPECT_FALSE(inlined.contains("extensions"));

    EXPECT_EQ(buildSnapshotRequest("forsen")
                  .value("variables")
                  .toObject()
                  .value("login")
                  .toString(),
              "forsen");
}

TEST(ChannelEvents, ParseActionResponses)
{
    QString error;
    // Real responses (trimmed).
    auto ok = parseVoteResponse(
        parseJson(R"json({"data": {"voteInPoll": {
          "voter": {"id": "ba40b7f3-a067-4000-8ec7-618ab0867d1f:10000006",
            "votes": {"total": 1, "communityPoints": 0, "base": 1}},
          "error": null, "__typename": "VoteInPollPayload"}},
          "extensions": {"operationName": "ChannelPollContext_VoteInPoll"}})json"),
        error);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->ok);

    auto predicted = parsePredictionResponse(
        parseJson(R"json({"data": {"makePrediction": {"error": null,
          "__typename": "MakePredictionPayload"}},
          "extensions": {"operationName": "MakePrediction"}})json"),
        error);
    ASSERT_TRUE(predicted.has_value());
    EXPECT_TRUE(predicted->ok);

    auto refused = parsePredictionResponse(
        parseJson(R"json({"data": {"makePrediction": {"prediction": null,
                  "error": {"code": "NOT_ENOUGH_POINTS"}}}})json"),
        error);
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->ok);
    EXPECT_EQ(refused->errorCode, "NOT_ENOUGH_POINTS");
    EXPECT_EQ(describePredictionError(refused->errorCode),
              "Not enough channel points.");

    auto missing = parseVoteResponse(
        parseJson(R"json({"errors": [{"message": "failed integrity check"}],
                  "data": {"voteInPoll": null}})json"),
        error);
    EXPECT_FALSE(missing.has_value());
    EXPECT_FALSE(error.isEmpty());

    EXPECT_EQ(describeVoteError("POLL_NOT_ACTIVE"), "The poll is over.");
    EXPECT_EQ(describeVoteError("WEIRD"), "Twitch refused the vote (WEIRD).");
    EXPECT_EQ(describePredictionError("EVENT_LOCKED"),
              "Predictions are closed.");
}

// Real response (trimmed) for a non-mod, from the website's GQL.
const char *PINNED_RESPONSE = R"json({"data": {"user": {"id": "10000004",
  "channel": {"pinnedChatMessages": {"edges": [
    {"node": {"id": "2f1d", "type": "PAID", "startsAt": "2026-10-31T01:10:00Z",
      "endsAt": "2026-10-31T01:20:00Z",
      "pinnedBy": {"id": "1", "login": "someone", "displayName": "Someone"},
      "pinnedMessage": {"id": "aaaa", "sender": {"id": "1", "login": "someone",
        "displayName": "Someone"}, "content": {"text": "paid"}}}},
    {"node": {"id": "93ed4028-e3f9-4988-8b38-b298e7d1adcf", "type": "MOD",
      "startsAt": "2026-10-31T01:15:24Z", "endsAt": null,
      "pinnedBy": {"id": "10000002", "login": "testmod",
                   "displayName": "TestMod"},
      "pinnedMessage": {"id": "71095e60-b22b-415c-8402-ccbcca66d78b",
        "sender": {"id": "10000002", "login": "testmod",
                   "displayName": "TestMod"},
        "content": {"text": "Welcome to the stream!"}}}}
  ]}}}}})json";

TEST(ChannelEvents, ParsePinnedMessage)
{
    const auto now = QDateTime::fromString("2026-10-31T01:16:00Z", Qt::ISODate);
    QString error;
    auto pin = parsePinnedMessage(parseJson(PINNED_RESPONSE), now, error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_TRUE(pin.has_value());
    // The paid pin is skipped, Helix wouldn't return it either.
    EXPECT_EQ(pin->messageID, "71095e60-b22b-415c-8402-ccbcca66d78b");
    EXPECT_EQ(pin->messageText, "Welcome to the stream!");
    EXPECT_EQ(pin->sender.login, "testmod");
    EXPECT_EQ(pin->pinnedBy.displayName, "TestMod");
    EXPECT_EQ(pin->startsAt,
              QDateTime::fromString("2026-10-31T01:15:24Z", Qt::ISODate));
    EXPECT_FALSE(pin->endsAt.has_value());

    // Nothing pinned.
    pin = parsePinnedMessage(
        parseJson(R"json({"data": {"user": {"id": "11148817", "channel":
            {"pinnedChatMessages": {"edges": []}}}}})json"),
        now, error);
    EXPECT_FALSE(pin.has_value());
    EXPECT_TRUE(error.isEmpty());

    // A timed pin, before and after its time runs out.
    const auto *timed = R"json({"data": {"user": {"id": "1", "channel":
        {"pinnedChatMessages": {"edges": [{"node": {"id": "p", "type": "MOD",
          "startsAt": "2026-10-31T01:15:00Z", "endsAt": "2026-10-31T01:20:00Z",
          "pinnedBy": {"id": "2", "login": "mod", "displayName": "Mod"},
          "pinnedMessage": {"id": "m", "sender": {"id": "3", "login": "a",
            "displayName": "A"}, "content": {"text": "hi"}}}}]}}}}})json";
    pin = parsePinnedMessage(parseJson(timed), now, error);
    ASSERT_TRUE(pin.has_value());
    ASSERT_TRUE(pin->endsAt.has_value());
    EXPECT_EQ(*pin->endsAt,
              QDateTime::fromString("2026-10-31T01:20:00Z", Qt::ISODate));
    pin = parsePinnedMessage(parseJson(timed), now.addSecs(300), error);
    EXPECT_FALSE(pin.has_value());
    EXPECT_TRUE(error.isEmpty());

    pin = parsePinnedMessage(parseJson(R"json({"data": {"user": null}})json"),
                             now, error);
    EXPECT_FALSE(pin.has_value());
    EXPECT_EQ(error, "Channel not found");

    const auto request = buildPinnedMessageRequest("kokonuts");
    EXPECT_EQ(request.value("variables").toObject().value("login").toString(),
              "kokonuts");
    EXPECT_TRUE(request.value("query").toString().contains(
        "pinnedChatMessages"));
}

TEST(ChannelEvents, ShortNumber)
{
    EXPECT_EQ(shortNumber(0), "0");
    EXPECT_EQ(shortNumber(950), "950");
    EXPECT_EQ(shortNumber(1000), "1K");
    EXPECT_EQ(shortNumber(12345), "12.3K");
    EXPECT_EQ(shortNumber(250000), "250K");
    EXPECT_EQ(shortNumber(4600000), "4.6M");
    EXPECT_EQ(shortNumber(2500000000LL), "2.5B");
}
