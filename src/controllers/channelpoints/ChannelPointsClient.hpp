#pragma once

#include "controllers/channelpoints/ChannelPointsModels.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace chatterino {

class ChannelPointsClient
{
public:
    enum class MessageKind {
        Connected,
        Error,
        Event,
        ChannelPointsSnapshot,
        RewardsSnapshot,
        RedeemResult,
        Subscribed,
        Unsubscribed,
        SubscriptionsReplaced,
        Unknown,
    };

    struct ParsedMessage {
        MessageKind kind = MessageKind::Unknown;
        QString type;
        QString requestId;
        QString channelLogin;
        QString channelId;
        QString event;
        QString errorCode;
        QString errorMessage;
        QJsonObject root;
    };

    static QByteArray buildReplaceSubscriptionsRequest(
        const QString &requestId, const QStringList &channels);
    static QByteArray buildGetRewardsRequest(const QString &requestId,
                                             const QString &channelLogin);
    static QByteArray buildGetChannelPointsRequest(const QString &requestId,
                                                   const QString &channelLogin);
    static QByteArray buildRedeemRequest(const QString &requestId,
                                         const QString &channelLogin,
                                         const ChannelPointRewardData &reward,
                                         const QString &transactionId);

    static std::optional<ParsedMessage> parseMessage(const QByteArray &payload,
                                                     QString &error);
    static QVector<ChannelPointRewardData> parseRewards(const QJsonArray &items,
                                                        bool customRewards);
};

}  // namespace chatterino
