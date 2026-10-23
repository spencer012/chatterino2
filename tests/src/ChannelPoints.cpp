// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "controllers/channelpoints/ChannelPointsTypes.hpp"

#include "Test.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

using namespace chatterino;
using namespace chatterino::channelpoints;

namespace {

QJsonObject parseJson(const char *json)
{
    QJsonParseError error{};
    auto doc = QJsonDocument::fromJson(QByteArray(json), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << error.errorString();
    return doc.object();
}

// Trimmed from a real ChannelPointsContext response.
const char *CONTEXT_RESPONSE = R"({
  "data": {
    "community": {
      "id": "11148817",
      "login": "pajlada",
      "displayName": "pajlada",
      "channel": {
        "id": "11148817",
        "self": {
          "communityPoints": {
            "balance": 12345,
            "availableClaim": null,
            "userRedemptions": [
              {"reward": {"id": "reward-hi"}, "userRedemptionsCurrentStream": 1}
            ]
          }
        },
        "communityPointsSettings": {
          "customRewards": [
            {
              "id": "reward-hi",
              "title": "Hi!",
              "prompt": "Say hi",
              "backgroundColor": "#1F69FF",
              "cost": 1,
              "pricingType": "POINTS",
              "cooldownExpiresAt": null,
              "isEnabled": true,
              "isInStock": true,
              "isPaused": false,
              "isSubOnly": false,
              "isUserInputRequired": false,
              "redemptionsRedeemedCurrentStream": 37,
              "maxPerStreamSetting": {"isEnabled": false, "maxPerStream": 0},
              "maxPerUserPerStreamSetting": {"isEnabled": true, "maxPerUserPerStream": 1},
              "globalCooldownSetting": {"isEnabled": false, "globalCooldownSeconds": 0}
            },
            {
              "id": "reward-tts",
              "title": "TTS",
              "prompt": "",
              "backgroundColor": "#00C7AC",
              "cost": 5000,
              "pricingType": "POINTS",
              "cooldownExpiresAt": "2026-10-05T00:29:54.713618729Z",
              "isEnabled": true,
              "isInStock": false,
              "isPaused": false,
              "isSubOnly": false,
              "isUserInputRequired": true,
              "redemptionsRedeemedCurrentStream": null,
              "maxPerStreamSetting": {"isEnabled": true, "maxPerStream": 10},
              "maxPerUserPerStreamSetting": {"isEnabled": false, "maxPerUserPerStream": 0},
              "globalCooldownSetting": {"isEnabled": true, "globalCooldownSeconds": 300}
            },
            {"title": "missing id is skipped", "cost": 1}
          ]
        }
      }
    }
  },
  "extensions": {"durationMilliseconds": 61, "operationName": "ChannelPointsContext"}
})";

PointsReward makeReward()
{
    PointsReward reward;
    reward.id = "r";
    reward.title = "Reward";
    reward.cost = 100;
    return reward;
}

const QDateTime NOW =
    QDateTime::fromString("2026-10-05T00:00:00Z", Qt::ISODate).toUTC();

}  // namespace

TEST(ChannelPoints, ParseTwitchTime)
{
    auto nanos = parseTwitchTime("2026-10-05T00:29:54.713618729Z");
    ASSERT_TRUE(nanos.isValid());
    EXPECT_EQ(nanos.offsetFromUtc(), 0);
    EXPECT_EQ(nanos.time(), QTime(0, 29, 54, 713));
    EXPECT_EQ(nanos.date(), QDate(2026, 10, 5));

    auto noFraction = parseTwitchTime("2026-10-05T00:29:54Z");
    ASSERT_TRUE(noFraction.isValid());
    EXPECT_EQ(noFraction.time(), QTime(0, 29, 54));

    auto offset = parseTwitchTime("2026-10-05T02:29:54.5+02:00");
    ASSERT_TRUE(offset.isValid());
    EXPECT_EQ(offset.time(), QTime(0, 29, 54, 500));

    EXPECT_FALSE(parseTwitchTime("").isValid());
    EXPECT_FALSE(parseTwitchTime("garbage").isValid());
}

TEST(ChannelPoints, TransactionIds)
{
    static const QRegularExpression hex32("^[0-9a-f]{32}$");
    QSet<QString> seen;
    for (int i = 0; i < 1000; i++)
    {
        const auto id = generateTransactionId();
        EXPECT_TRUE(hex32.match(id).hasMatch()) << id.toStdString();
        seen.insert(id);
    }
    // The old implementation used a counter that restarted with Chatterino,
    // so IDs repeated across days and Twitch rejected them as duplicates.
    EXPECT_EQ(seen.size(), 1000);

    static const QRegularExpression alnum21("^[A-Za-z0-9]{21}$");
    EXPECT_TRUE(alnum21.match(generateHermesId()).hasMatch());
    EXPECT_NE(generateHermesId(), generateHermesId());
}

TEST(ChannelPoints, ParseContext)
{
    QString error;
    auto context = parseContext(parseJson(CONTEXT_RESPONSE), error);
    ASSERT_TRUE(context) << error.toStdString();

    EXPECT_EQ(context->channelId, "11148817");
    EXPECT_EQ(context->login, "pajlada");
    EXPECT_EQ(context->displayName, "pajlada");
    ASSERT_TRUE(context->balance);
    EXPECT_EQ(*context->balance, 12345);
    ASSERT_EQ(context->rewards.size(), 2);

    const auto &hi = context->rewards[0];
    EXPECT_EQ(hi.id, "reward-hi");
    EXPECT_EQ(hi.title, "Hi!");
    EXPECT_EQ(hi.prompt, "Say hi");
    EXPECT_EQ(hi.backgroundColor, "#1F69FF");
    EXPECT_EQ(hi.cost, 1);
    EXPECT_FALSE(hi.cooldownExpiresAt.isValid());
    EXPECT_EQ(hi.maxPerStream, 0);
    EXPECT_EQ(hi.maxPerUserPerStream, 1);
    EXPECT_EQ(hi.redemptionsThisStream, 37);
    EXPECT_EQ(hi.userRedemptionsThisStream, 1);
    EXPECT_FALSE(hi.hasGlobalCooldown);
    EXPECT_FALSE(hi.isUserInputRequired);

    const auto &tts = context->rewards[1];
    EXPECT_EQ(tts.id, "reward-tts");
    EXPECT_EQ(tts.cost, 5000);
    EXPECT_TRUE(tts.cooldownExpiresAt.isValid());
    EXPECT_TRUE(tts.hasGlobalCooldown);
    EXPECT_EQ(tts.globalCooldownSeconds, 300);
    EXPECT_EQ(tts.maxPerStream, 10);
    EXPECT_EQ(tts.maxPerUserPerStream, 0);
    EXPECT_EQ(tts.redemptionsThisStream, -1);
    // Not in userRedemptions: never redeemed this stream.
    EXPECT_EQ(tts.userRedemptionsThisStream, 0);
    EXPECT_FALSE(tts.isInStock);
    EXPECT_TRUE(tts.isUserInputRequired);
}

TEST(ChannelPoints, ParseContextErrors)
{
    QString error;
    EXPECT_FALSE(parseContext(parseJson(R"({"data": {"community": null}})"),
                              error));
    EXPECT_EQ(error, "Channel not found");

    error.clear();
    EXPECT_FALSE(parseContext(
        parseJson(R"({"errors": [{"message": "PersistedQueryNotFound"}]})"),
        error));
    EXPECT_EQ(error, "PersistedQueryNotFound");

    error.clear();
    EXPECT_FALSE(parseContext(
        parseJson(R"({"data": {"community": {"id": "1", "channel": null}}})"),
        error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(ChannelPoints, InspectGqlErrors)
{
    auto none = inspectGqlErrors(parseJson(R"({"data": {}})"));
    EXPECT_FALSE(none.hasErrors);

    auto pqnf = inspectGqlErrors(
        parseJson(R"({"errors": [{"message": "PersistedQueryNotFound"}]})"));
    EXPECT_TRUE(pqnf.hasErrors);
    EXPECT_TRUE(pqnf.persistedQueryNotFound);
    EXPECT_FALSE(pqnf.unauthorized);

    auto unauthorized =
        inspectGqlErrors(parseJson(R"({"errors": [{"message": "unauthorized"}]})"));
    EXPECT_TRUE(unauthorized.unauthorized);

    auto topLevel = inspectGqlErrors(parseJson(
        R"({"error": "Unauthorized", "status": 401, "message": "The \"Authorization\" token is invalid."})"));
    EXPECT_TRUE(topLevel.hasErrors);
    EXPECT_TRUE(topLevel.unauthorized);
}

TEST(ChannelPoints, BuildRedeemRequest)
{
    auto reward = makeReward();
    reward.prompt = "prompt";

    auto request = buildRedeemRequest("123", reward, {}, "tx", false);
    EXPECT_EQ(request["operationName"].toString(), "RedeemCustomReward");
    auto input = request["variables"].toObject()["input"].toObject();
    EXPECT_EQ(input["channelID"].toString(), "123");
    EXPECT_EQ(input["cost"].toInt(), 100);
    EXPECT_EQ(input["pricingType"].toString(), "POINTS");
    EXPECT_EQ(input["prompt"].toString(), "prompt");
    EXPECT_EQ(input["rewardID"].toString(), "r");
    EXPECT_EQ(input["title"].toString(), "Reward");
    EXPECT_EQ(input["transactionID"].toString(), "tx");
    // Twitch rejects textInput on rewards that don't take input.
    EXPECT_FALSE(input.contains("textInput"));
    EXPECT_TRUE(request.contains("extensions"));
    EXPECT_FALSE(request.contains("query"));

    auto withText = buildRedeemRequest("123", reward, "hello", "tx", true);
    EXPECT_EQ(withText["variables"].toObject()["input"].toObject()["textInput"]
                  .toString(),
              "hello");
    EXPECT_TRUE(withText.contains("query"));
    EXPECT_FALSE(withText.contains("extensions"));
}

TEST(ChannelPoints, BuildContextRequest)
{
    auto request = buildContextRequest("pajlada", false);
    EXPECT_EQ(request["operationName"].toString(), "ChannelPointsContext");
    EXPECT_EQ(request["variables"].toObject()["channelLogin"].toString(),
              "pajlada");
    EXPECT_EQ(request["extensions"]
                  .toObject()["persistedQuery"]
                  .toObject()["sha256Hash"]
                  .toString()
                  .size(),
              64);
    EXPECT_TRUE(buildContextRequest("pajlada", true).contains("query"));
}

TEST(ChannelPoints, ParseRedeemResponse)
{
    QString error;
    auto ok = parseRedeemResponse(
        parseJson(
            R"({"data": {"redeemCommunityPointsCustomReward": {"error": null, "redemption": {"id": "x"}}}})"),
        error);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ok->ok);

    auto failed = parseRedeemResponse(
        parseJson(
            R"({"data": {"redeemCommunityPointsCustomReward": {"error": {"code": "MAX_PER_USER_PER_STREAM", "message": ""}}}})"),
        error);
    ASSERT_TRUE(failed);
    EXPECT_FALSE(failed->ok);
    EXPECT_EQ(failed->errorCode, "MAX_PER_USER_PER_STREAM");

    error.clear();
    auto broken = parseRedeemResponse(
        parseJson(R"({"errors": [{"message": "service error"}]})"), error);
    EXPECT_FALSE(broken);
    EXPECT_EQ(error, "service error");
}

TEST(ChannelPoints, ClassifyRedeemError)
{
    // The bug behind the "works half the days" problem: a reused ID came back
    // as DUPLICATE_TRANSACTION on the *first* attempt, which is not a success.
    EXPECT_EQ(classifyRedeemError("DUPLICATE_TRANSACTION", false),
              RedeemOutcome::NewTransactionAndRetry);
    // Retrying an attempt that got no answer: Twitch already processed it.
    EXPECT_EQ(classifyRedeemError("DUPLICATE_TRANSACTION", true),
              RedeemOutcome::Success);
    EXPECT_EQ(classifyRedeemError("TRANSACTION_ALREADY_COMMITTED", false),
              RedeemOutcome::Success);
    EXPECT_EQ(classifyRedeemError("TRANSACTION_IN_PROGRESS", false),
              RedeemOutcome::RetrySameTransaction);
    EXPECT_EQ(classifyRedeemError("PROPERTIES_MISMATCH", false),
              RedeemOutcome::RefreshAndRetry);

    for (const auto *code :
         {"NOT_ENOUGH_POINTS", "GLOBAL_COOLDOWN", "COOLDOWN", "DISABLED",
          "PAUSED", "OUT_OF_STOCK", "MAX_PER_STREAM", "MAX_PER_USER_PER_STREAM",
          "RATE_LIMITED", "STREAM_NOT_LIVE"})
    {
        EXPECT_EQ(classifyRedeemError(code, false), RedeemOutcome::Wait)
            << code;
    }

    EXPECT_EQ(classifyRedeemError("FORBIDDEN", false), RedeemOutcome::Stop);
    EXPECT_EQ(classifyRedeemError("SOMETHING_NEW", false), RedeemOutcome::Stop);
    EXPECT_EQ(classifyRedeemError("", false), RedeemOutcome::Success);

    EXPECT_EQ(describeRedeemError("SOMETHING_NEW"), "SOMETHING_NEW");
    EXPECT_EQ(describeRedeemError("NOT_ENOUGH_POINTS"), "Not enough points");
}

TEST(ChannelPoints, EvaluateReward)
{
    auto reward = makeReward();
    EXPECT_EQ(evaluateReward(reward, 100, NOW), RewardBlock::None);
    // Unknown balance doesn't block.
    EXPECT_EQ(evaluateReward(reward, std::nullopt, NOW), RewardBlock::None);
    EXPECT_EQ(evaluateReward(reward, 99, NOW), RewardBlock::NotEnoughPoints);

    auto disabled = reward;
    disabled.isEnabled = false;
    EXPECT_EQ(evaluateReward(disabled, 100, NOW), RewardBlock::Disabled);

    auto paused = reward;
    paused.isPaused = true;
    EXPECT_EQ(evaluateReward(paused, 100, NOW), RewardBlock::Paused);

    auto cooldown = reward;
    cooldown.hasGlobalCooldown = true;
    cooldown.cooldownExpiresAt = NOW.addSecs(30);
    cooldown.isInStock = false;
    EXPECT_EQ(evaluateReward(cooldown, 100, NOW), RewardBlock::Cooldown);
    // Once the cooldown passed it's available, even if Twitch didn't send an
    // update flipping isInStock back.
    EXPECT_EQ(evaluateReward(cooldown, 100, NOW.addSecs(31)),
              RewardBlock::None);

    auto outOfStock = reward;
    outOfStock.isInStock = false;
    EXPECT_EQ(evaluateReward(outOfStock, 100, NOW), RewardBlock::OutOfStock);

    auto streamLimit = outOfStock;
    streamLimit.maxPerStream = 10;
    streamLimit.redemptionsThisStream = 10;
    EXPECT_EQ(evaluateReward(streamLimit, 100, NOW), RewardBlock::StreamLimit);

    auto userLimit = reward;
    userLimit.maxPerUserPerStream = 1;
    userLimit.userRedemptionsThisStream = 0;
    EXPECT_EQ(evaluateReward(userLimit, 100, NOW), RewardBlock::None);
    userLimit.userRedemptionsThisStream = 1;
    EXPECT_EQ(evaluateReward(userLimit, 100, NOW),
              RewardBlock::UserStreamLimit);
    // Unknown count doesn't block.
    userLimit.userRedemptionsThisStream = -1;
    EXPECT_EQ(evaluateReward(userLimit, 100, NOW), RewardBlock::None);

    // Order: the most permanent reason wins.
    auto many = cooldown;
    many.isPaused = true;
    EXPECT_EQ(evaluateReward(many, 0, NOW), RewardBlock::Paused);
}

TEST(ChannelPoints, ParsePointsEvents)
{
    auto balance = parsePointsEvent(parseJson(R"({
      "type": "points-spent",
      "data": {"balance": {"user_id": "1", "channel_id": "11148817", "balance": 4200}}
    })"));
    EXPECT_EQ(balance.type, PointsEvent::Type::BalanceUpdate);
    EXPECT_EQ(balance.channelId, "11148817");
    EXPECT_EQ(balance.balance, 4200);

    auto updated = parsePointsEvent(parseJson(R"({
      "type": "custom-reward-updated",
      "data": {"updated_reward": {
        "id": "reward-tts", "channel_id": "11148817", "title": "TTS",
        "prompt": "", "cost": 6000, "background_color": "#00C7AC",
        "is_user_input_required": true, "is_enabled": true, "is_paused": false,
        "is_in_stock": true, "is_sub_only": false,
        "max_per_stream": {"is_enabled": true, "max_per_stream": 10},
        "max_per_user_per_stream": {"is_enabled": false, "max_per_user_per_stream": 0},
        "global_cooldown": {"is_enabled": true, "global_cooldown_seconds": 300},
        "redemptions_redeemed_current_stream": 3,
        "cooldown_expires_at": "2026-10-05T00:05:00Z"
      }}
    })"));
    EXPECT_EQ(updated.type, PointsEvent::Type::RewardUpdated);
    EXPECT_EQ(updated.channelId, "11148817");
    EXPECT_EQ(updated.reward.id, "reward-tts");
    EXPECT_EQ(updated.reward.cost, 6000);
    EXPECT_EQ(updated.reward.backgroundColor, "#00C7AC");
    EXPECT_TRUE(updated.reward.isUserInputRequired);
    EXPECT_EQ(updated.reward.maxPerStream, 10);
    EXPECT_EQ(updated.reward.maxPerUserPerStream, 0);
    EXPECT_TRUE(updated.reward.hasGlobalCooldown);
    EXPECT_EQ(updated.reward.globalCooldownSeconds, 300);
    EXPECT_EQ(updated.reward.redemptionsThisStream, 3);
    EXPECT_EQ(updated.reward.cooldownExpiresAt, NOW.addSecs(300));

    auto created = parsePointsEvent(parseJson(
        R"({"type": "custom-reward-created", "data": {"new_reward": {"id": "n", "channel_id": "5"}}})"));
    EXPECT_EQ(created.type, PointsEvent::Type::RewardCreated);
    EXPECT_EQ(created.reward.id, "n");

    auto deleted = parsePointsEvent(parseJson(
        R"({"type": "custom-reward-deleted", "data": {"deleted_reward": {"id": "d", "channel_id": "5"}}})"));
    EXPECT_EQ(deleted.type, PointsEvent::Type::RewardDeleted);
    EXPECT_EQ(deleted.channelId, "5");

    auto redeemed = parsePointsEvent(parseJson(R"({
      "type": "reward-redeemed",
      "data": {"redemption": {
        "channel_id": "11148817",
        "user": {"id": "42", "login": "me"},
        "reward": {"id": "reward-hi", "channel_id": "11148817", "cost": 1}
      }}
    })"));
    EXPECT_EQ(redeemed.type, PointsEvent::Type::RewardRedeemed);
    EXPECT_EQ(redeemed.channelId, "11148817");
    EXPECT_EQ(redeemed.redeemUserId, "42");
    EXPECT_EQ(redeemed.reward.id, "reward-hi");

    auto unknown = parsePointsEvent(parseJson(R"({"type": "claim-available"})"));
    EXPECT_EQ(unknown.type, PointsEvent::Type::Unknown);
}

TEST(ChannelPoints, ApplyLiveRewardUpdate)
{
    auto existing = makeReward();
    existing.userRedemptionsThisStream = 2;
    existing.redemptionsThisStream = 9;

    auto update = makeReward();
    update.cost = 250;
    update.redemptionsThisStream = -1;
    update.userRedemptionsThisStream = -1;

    applyLiveRewardUpdate(existing, update);
    EXPECT_EQ(existing.cost, 250);
    // Live updates don't carry per-user counts.
    EXPECT_EQ(existing.userRedemptionsThisStream, 2);
    // Nor always the stream count.
    EXPECT_EQ(existing.redemptionsThisStream, 9);

    update.redemptionsThisStream = 11;
    applyLiveRewardUpdate(existing, update);
    EXPECT_EQ(existing.redemptionsThisStream, 11);
}

TEST(ChannelPoints, Hermes)
{
    auto welcome = parseHermesMessage(
        R"({"id":"a","type":"welcome","welcome":{"keepaliveSec":10,"recoveryUrl":"wss://x","sessionId":"s"},"timestamp":"t"})");
    ASSERT_TRUE(welcome);
    EXPECT_EQ(welcome->type, HermesMessage::Type::Welcome);
    EXPECT_EQ(welcome->keepaliveSec, 10);

    auto auth = parseHermesMessage(
        R"({"id":"b","parentId":"req","type":"authenticateResponse","authenticateResponse":{"result":"error","error":"bad token","errorCode":"AUTH001"}})");
    ASSERT_TRUE(auth);
    EXPECT_EQ(auth->type, HermesMessage::Type::AuthenticateResponse);
    EXPECT_EQ(auth->parentId, "req");
    EXPECT_EQ(auth->result, "error");
    EXPECT_EQ(auth->error, "bad token");
    EXPECT_EQ(auth->errorCode, "AUTH001");

    auto sub = parseHermesMessage(
        R"({"id":"c","type":"subscribeResponse","subscribeResponse":{"result":"ok","subscription":{"id":"sub1"}}})");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->type, HermesMessage::Type::SubscribeResponse);
    EXPECT_EQ(sub->result, "ok");
    EXPECT_EQ(sub->subscriptionId, "sub1");

    auto notification = parseHermesMessage(
        R"({"id":"d","type":"notification","notification":{"subscription":{"id":"sub1"},"type":"pubsub","pubsub":"{\"type\":\"points-earned\",\"data\":{\"balance\":{\"channel_id\":\"1\",\"balance\":7}}}"}})");
    ASSERT_TRUE(notification);
    EXPECT_EQ(notification->type, HermesMessage::Type::Notification);
    EXPECT_EQ(notification->subscriptionId, "sub1");
    auto event = parsePointsEvent(notification->pubsub);
    EXPECT_EQ(event.type, PointsEvent::Type::BalanceUpdate);
    EXPECT_EQ(event.balance, 7);

    auto reconnect = parseHermesMessage(
        R"({"id":"e","type":"reconnect","reconnect":{"url":"wss://hermes.twitch.tv/v1?x=1"}})");
    ASSERT_TRUE(reconnect);
    EXPECT_EQ(reconnect->type, HermesMessage::Type::Reconnect);
    EXPECT_EQ(reconnect->reconnectUrl, "wss://hermes.twitch.tv/v1?x=1");

    auto keepalive = parseHermesMessage(R"({"id":"f","type":"keepalive"})");
    ASSERT_TRUE(keepalive);
    EXPECT_EQ(keepalive->type, HermesMessage::Type::Keepalive);

    EXPECT_FALSE(parseHermesMessage("not json"));
    EXPECT_FALSE(parseHermesMessage("[1, 2]"));

    auto subscribe = QJsonDocument::fromJson(buildHermesSubscribe(
                                                 "req", "sub", "topic.1"))
                         .object();
    EXPECT_EQ(subscribe["type"].toString(), "subscribe");
    EXPECT_EQ(subscribe["id"].toString(), "req");
    EXPECT_EQ(subscribe["subscribe"].toObject()["id"].toString(), "sub");
    EXPECT_EQ(subscribe["subscribe"].toObject()["type"].toString(), "pubsub");
    EXPECT_EQ(subscribe["subscribe"]
                  .toObject()["pubsub"]
                  .toObject()["topic"]
                  .toString(),
              "topic.1");

    auto authenticate =
        QJsonDocument::fromJson(buildHermesAuthenticate("req", "tok")).object();
    EXPECT_EQ(authenticate["type"].toString(), "authenticate");
    EXPECT_EQ(authenticate["authenticate"].toObject()["token"].toString(),
              "tok");
}

TEST(ChannelPoints, DeviceCodeFlow)
{
    auto code = parseDeviceCodeResponse(parseJson(
        R"({"device_code":"dc","expires_in":1800,"interval":5,"user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate?device-code=ABCDEFGH"})"));
    ASSERT_TRUE(code);
    EXPECT_EQ(code->deviceCode, "dc");
    EXPECT_EQ(code->userCode, "ABCDEFGH");
    EXPECT_EQ(code->expiresIn, 1800);
    EXPECT_EQ(code->interval, 5);
    EXPECT_TRUE(code->verificationUri.startsWith("https://www.twitch.tv/activate"));

    auto noUri = parseDeviceCodeResponse(
        parseJson(R"({"device_code":"dc","user_code":"X"})"));
    ASSERT_TRUE(noUri);
    EXPECT_EQ(noUri->verificationUri, "https://www.twitch.tv/activate");

    EXPECT_FALSE(parseDeviceCodeResponse(parseJson(R"({"status":400})")));

    auto success = parseTokenResponse(
        200, parseJson(
                 R"({"access_token":"at","refresh_token":"rt","expires_in":14000,"token_type":"bearer"})"));
    EXPECT_EQ(success.status, TokenResponse::Status::Success);
    EXPECT_EQ(success.accessToken, "at");
    EXPECT_EQ(success.refreshToken, "rt");

    EXPECT_EQ(parseTokenResponse(
                  400, parseJson(
                           R"({"status":400,"message":"authorization_pending"})"))
                  .status,
              TokenResponse::Status::Pending);
    EXPECT_EQ(
        parseTokenResponse(400, parseJson(R"({"status":400,"message":"slow_down"})"))
            .status,
        TokenResponse::Status::SlowDown);
    EXPECT_EQ(parseTokenResponse(
                  400, parseJson(R"({"status":400,"message":"invalid device code"})"))
                  .status,
              TokenResponse::Status::Expired);

    auto failed = parseTokenResponse(std::nullopt, {});
    EXPECT_EQ(failed.status, TokenResponse::Status::Failed);
    EXPECT_FALSE(failed.message.isEmpty());

    auto valid = parseValidateResponse(
        200, parseJson(
                 R"({"client_id":"ue6666qo983tsx6so1t0vnawi233wa","login":"me","scopes":[],"user_id":"42","expires_in":1000})"));
    EXPECT_TRUE(valid.valid);
    EXPECT_EQ(valid.login, "me");
    EXPECT_EQ(valid.userId, "42");
    EXPECT_EQ(valid.clientId, TV_CLIENT_ID);

    EXPECT_FALSE(parseValidateResponse(
                     401, parseJson(R"({"status":401,"message":"invalid access token"})"))
                     .valid);
}
