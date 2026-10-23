// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "controllers/channelpoints/ChannelPointsTypes.hpp"

#include <QObject>
#include <QTimer>

namespace chatterino {

/// Manages the token used for channel points.
///
/// Chatterino's own login can't use Twitch's first-party APIs, so this signs
/// in separately through the device code flow of the Twitch TV app
/// ("go to twitch.tv/activate and enter this code"). Tokens are stored per
/// Chatterino account in the settings, like Chatterino's own tokens.
class ChannelPointsAuth : public QObject
{
    Q_OBJECT

public:
    explicit ChannelPointsAuth(QObject *parent = nullptr);

    [[nodiscard]] ChannelPointsAuthView view() const;
    /// The token, if signed in. Empty otherwise.
    [[nodiscard]] QString token() const;
    /// Twitch user ID the token belongs to.
    [[nodiscard]] QString userId() const;
    /// Stable per-install device ID sent as X-Device-Id.
    [[nodiscard]] static QString deviceId();

    /// Switches to the given Chatterino account (empty = anonymous).
    void setChatterinoUser(const QString &userId, const QString &login);

    void startLogin();
    void cancelLogin();
    void signOut();

    /// Called when Twitch rejected the token. Tries to refresh it.
    void reportTokenRejected(const QString &reason);

Q_SIGNALS:
    /// The state or token changed.
    void changed();

private:
    void setState(ChannelPointsAuthState state, const QString &error = {});
    void loadStored();
    void store();
    void clearStored();
    void validate();
    void refreshToken();
    void pollToken();
    void acceptToken(const QString &accessToken, const QString &refreshToken);

    QString settingsPath(const QString &key) const;

    QString chatterinoUserId_;
    QString chatterinoLogin_;

    QString token_;
    QString refreshToken_;
    QString tokenLogin_;
    QString tokenUserId_;

    ChannelPointsAuthState state_ = ChannelPointsAuthState::NoChatterinoAccount;
    QString error_;

    std::optional<channelpoints::DeviceCode> deviceCode_;
    QDateTime codeExpiresAt_;
    int pollIntervalSec_ = 5;
    QTimer pollTimer_;
    QTimer revalidateTimer_;
    bool refreshing_ = false;
    bool justRefreshed_ = false;

    /// Incremented to drop responses of abandoned requests.
    quint64 generation_ = 0;
};

}  // namespace chatterino
