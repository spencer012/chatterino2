// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsAuth.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/channelpoints/ChannelPointsLog.hpp"
#include "singletons/Settings.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/settings/settingmanager.hpp>
#include <QUrlQuery>

namespace chatterino {

using namespace channelpoints;

namespace {

constexpr int REVALIDATE_INTERVAL_MS = 60 * 60 * 1000;
constexpr int VALIDATE_RETRY_MS = 5 * 60 * 1000;
constexpr int REQUEST_TIMEOUT_MS = 20 * 1000;

const std::string DEVICE_ID_PATH = "/channelPoints/deviceId";

NetworkRequest postForm(const QString &url, const QUrlQuery &form)
{
    const auto body = form.toString(QUrl::FullyEncoded).toUtf8();
    return NetworkRequest(url, NetworkRequestType::Post)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .header("Client-Id", TV_CLIENT_ID)
        .header("X-Device-Id", ChannelPointsAuth::deviceId())
        .payload(body)
        .timeout(REQUEST_TIMEOUT_MS);
}

}  // namespace

ChannelPointsAuth::ChannelPointsAuth(QObject *parent)
    : QObject(parent)
{
    this->pollTimer_.setSingleShot(true);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this,
                     &ChannelPointsAuth::pollToken);

    this->revalidateTimer_.setSingleShot(true);
    QObject::connect(&this->revalidateTimer_, &QTimer::timeout, this,
                     &ChannelPointsAuth::validate);
}

ChannelPointsAuthView ChannelPointsAuth::view() const
{
    ChannelPointsAuthView out{
        .state = this->state_,
        .login = this->tokenLogin_,
        .error = this->error_,
    };
    if (this->deviceCode_ &&
        this->state_ == ChannelPointsAuthState::AwaitingActivation)
    {
        out.userCode = this->deviceCode_->userCode;
        out.verificationUri = this->deviceCode_->verificationUri;
        out.codeExpiresAt = this->codeExpiresAt_;
    }
    return out;
}

QString ChannelPointsAuth::token() const
{
    if (this->state_ != ChannelPointsAuthState::SignedIn)
    {
        return {};
    }
    return this->token_;
}

QString ChannelPointsAuth::userId() const
{
    return this->tokenUserId_;
}

QString ChannelPointsAuth::deviceId()
{
    auto id = pajlada::Settings::Setting<QString>::get(DEVICE_ID_PATH);
    if (id.isEmpty())
    {
        id = generateTransactionId();
        pajlada::Settings::Setting<QString>::set(DEVICE_ID_PATH, id);
    }
    return id;
}

void ChannelPointsAuth::setChatterinoUser(const QString &userId,
                                          const QString &login)
{
    if (userId == this->chatterinoUserId_ && login == this->chatterinoLogin_ &&
        this->state_ != ChannelPointsAuthState::NoChatterinoAccount)
    {
        return;
    }

    this->generation_++;
    this->pollTimer_.stop();
    this->revalidateTimer_.stop();
    this->deviceCode_.reset();
    this->refreshing_ = false;

    this->chatterinoUserId_ = userId;
    this->chatterinoLogin_ = login;
    this->token_.clear();
    this->refreshToken_.clear();
    this->tokenLogin_.clear();
    this->tokenUserId_.clear();

    if (userId.isEmpty())
    {
        this->setState(ChannelPointsAuthState::NoChatterinoAccount);
        return;
    }

    this->loadStored();
    if (this->token_.isEmpty())
    {
        this->setState(ChannelPointsAuthState::SignedOut);
        return;
    }

    logInfo(QStringLiteral("Auth: found stored token for @%1, validating")
                .arg(this->tokenLogin_));
    this->setState(ChannelPointsAuthState::Validating);
    this->validate();
}

void ChannelPointsAuth::startLogin()
{
    if (this->chatterinoUserId_.isEmpty())
    {
        return;
    }

    this->generation_++;
    this->pollTimer_.stop();
    this->deviceCode_.reset();
    this->setState(ChannelPointsAuthState::RequestingCode);
    logInfo(QStringLiteral("Auth: requesting device code"));

    QUrlQuery form;
    form.addQueryItem("client_id", TV_CLIENT_ID);
    form.addQueryItem("scopes", DEVICE_SCOPES);

    auto handle = [this, gen = this->generation_](const NetworkResult &result) {
        if (gen != this->generation_)
        {
            return;
        }

        auto code = parseDeviceCodeResponse(result.parseJson());
        if (!code)
        {
            logWarning(QStringLiteral("Auth: device code request failed: %1 %2")
                           .arg(result.formatError(),
                                QString::fromUtf8(result.getData().left(300))));
            this->setState(ChannelPointsAuthState::SignedOut,
                           QStringLiteral("Couldn't start sign-in (%1)")
                               .arg(result.formatError()));
            return;
        }

        this->deviceCode_ = *code;
        this->pollIntervalSec_ = code->interval;
        this->codeExpiresAt_ =
            QDateTime::currentDateTimeUtc().addSecs(code->expiresIn);
        this->setState(ChannelPointsAuthState::AwaitingActivation);
        this->pollTimer_.start(this->pollIntervalSec_ * 1000);
    };

    postForm(DEVICE_CODE_URL, form)
        .caller(this)
        .onSuccess(handle)
        .onError(handle)
        .execute();
}

void ChannelPointsAuth::cancelLogin()
{
    if (this->state_ != ChannelPointsAuthState::RequestingCode &&
        this->state_ != ChannelPointsAuthState::AwaitingActivation)
    {
        return;
    }

    this->generation_++;
    this->pollTimer_.stop();
    this->deviceCode_.reset();
    this->setState(ChannelPointsAuthState::SignedOut);
}

void ChannelPointsAuth::signOut()
{
    logInfo(QStringLiteral("Auth: signed out @%1").arg(this->tokenLogin_));
    this->generation_++;
    this->pollTimer_.stop();
    this->revalidateTimer_.stop();
    this->deviceCode_.reset();
    this->clearStored();
    this->setState(this->chatterinoUserId_.isEmpty()
                       ? ChannelPointsAuthState::NoChatterinoAccount
                       : ChannelPointsAuthState::SignedOut);
}

void ChannelPointsAuth::reportTokenRejected(const QString &reason)
{
    if (this->state_ != ChannelPointsAuthState::SignedIn || this->refreshing_)
    {
        return;
    }

    logWarning(QStringLiteral("Auth: token rejected (%1), refreshing").arg(reason));
    this->refreshToken();
}

void ChannelPointsAuth::setState(ChannelPointsAuthState state,
                                 const QString &error)
{
    this->state_ = state;
    this->error_ = error;
    Q_EMIT this->changed();
}

QString ChannelPointsAuth::settingsPath(const QString &key) const
{
    return QStringLiteral("/channelPoints/accounts/uid%1/%2")
        .arg(this->chatterinoUserId_, key);
}

void ChannelPointsAuth::loadStored()
{
    auto get = [this](const char *key) {
        return pajlada::Settings::Setting<QString>::get(
            this->settingsPath(key).toStdString());
    };
    this->token_ = get("token");
    this->refreshToken_ = get("refreshToken");
    this->tokenLogin_ = get("login");
    this->tokenUserId_ = get("userID");
}

void ChannelPointsAuth::store()
{
    auto set = [this](const char *key, const QString &value) {
        pajlada::Settings::Setting<QString>::set(
            this->settingsPath(key).toStdString(), value);
    };
    set("token", this->token_);
    set("refreshToken", this->refreshToken_);
    set("login", this->tokenLogin_);
    set("userID", this->tokenUserId_);
    getSettings()->requestSave();
}

void ChannelPointsAuth::clearStored()
{
    this->token_.clear();
    this->refreshToken_.clear();
    this->tokenLogin_.clear();
    this->tokenUserId_.clear();
    if (!this->chatterinoUserId_.isEmpty())
    {
        pajlada::Settings::SettingManager::gRemoveSetting(
            QStringLiteral("/channelPoints/accounts/uid%1")
                .arg(this->chatterinoUserId_)
                .toStdString());
        getSettings()->requestSave();
    }
}

void ChannelPointsAuth::validate()
{
    if (this->token_.isEmpty())
    {
        return;
    }

    auto handle = [this, gen = this->generation_](const NetworkResult &result) {
        if (gen != this->generation_)
        {
            return;
        }

        if (!result.status())
        {
            // Network trouble. Keep the token, Twitch will tell us if it's bad.
            logWarning(QStringLiteral("Auth: couldn't validate token (%1), "
                                      "retrying later")
                           .arg(result.formatError()));
            this->revalidateTimer_.start(VALIDATE_RETRY_MS);
            if (this->state_ == ChannelPointsAuthState::Validating)
            {
                this->setState(ChannelPointsAuthState::SignedIn);
            }
            return;
        }

        auto response = parseValidateResponse(result.status(), result.parseJson());
        if (!response.valid)
        {
            logWarning(QStringLiteral("Auth: token invalid (HTTP %1)")
                           .arg(result.status().value_or(0)));
            if (this->justRefreshed_)
            {
                // A freshly refreshed token is already invalid, give up
                // instead of looping.
                this->justRefreshed_ = false;
                this->clearStored();
                this->setState(ChannelPointsAuthState::SignedOut,
                               QStringLiteral("Your channel points sign-in "
                                              "expired. Please connect again."));
                return;
            }
            this->refreshToken();
            return;
        }

        if (response.userId != this->chatterinoUserId_)
        {
            logWarning(QStringLiteral("Auth: token belongs to @%1, but "
                                      "Chatterino is using @%2")
                           .arg(response.login, this->chatterinoLogin_));
            this->clearStored();
            this->setState(
                ChannelPointsAuthState::SignedOut,
                QStringLiteral("You signed in as @%1, but Chatterino is using "
                               "@%2. Sign in as @%2 on Twitch and try again.")
                    .arg(response.login, this->chatterinoLogin_));
            return;
        }

        const bool changed = this->tokenLogin_ != response.login ||
                             this->tokenUserId_ != response.userId;
        this->tokenLogin_ = response.login;
        this->tokenUserId_ = response.userId;
        if (changed)
        {
            this->store();
        }

        if (this->state_ != ChannelPointsAuthState::SignedIn)
        {
            logInfo(QStringLiteral("Auth: signed in as @%1").arg(response.login));
        }
        this->justRefreshed_ = false;
        this->revalidateTimer_.start(REVALIDATE_INTERVAL_MS);
        this->setState(ChannelPointsAuthState::SignedIn);
    };

    NetworkRequest(VALIDATE_URL)
        .header("Authorization", "OAuth " + this->token_)
        .timeout(REQUEST_TIMEOUT_MS)
        .caller(this)
        .onSuccess(handle)
        .onError(handle)
        .execute();
}

void ChannelPointsAuth::refreshToken()
{
    if (this->refreshToken_.isEmpty())
    {
        this->clearStored();
        this->setState(ChannelPointsAuthState::SignedOut,
                       QStringLiteral("Your channel points sign-in expired. "
                                      "Please connect again."));
        return;
    }

    this->refreshing_ = true;
    this->setState(ChannelPointsAuthState::Validating);

    QUrlQuery form;
    form.addQueryItem("client_id", TV_CLIENT_ID);
    form.addQueryItem("grant_type", "refresh_token");
    form.addQueryItem("refresh_token", this->refreshToken_);

    auto handle = [this, gen = this->generation_](const NetworkResult &result) {
        if (gen != this->generation_)
        {
            return;
        }
        this->refreshing_ = false;

        auto response = parseTokenResponse(result.status(), result.parseJson());
        if (response.status != TokenResponse::Status::Success)
        {
            logWarning(QStringLiteral("Auth: refreshing token failed: %1")
                           .arg(response.message));
            this->clearStored();
            this->setState(ChannelPointsAuthState::SignedOut,
                           QStringLiteral("Your channel points sign-in expired. "
                                          "Please connect again."));
            return;
        }

        logInfo(QStringLiteral("Auth: token refreshed"));
        this->justRefreshed_ = true;
        this->acceptToken(response.accessToken, response.refreshToken);
    };

    postForm(TOKEN_URL, form)
        .caller(this)
        .onSuccess(handle)
        .onError(handle)
        .execute();
}

void ChannelPointsAuth::pollToken()
{
    if (!this->deviceCode_)
    {
        return;
    }

    if (QDateTime::currentDateTimeUtc() > this->codeExpiresAt_)
    {
        logInfo(QStringLiteral("Auth: device code expired"));
        this->deviceCode_.reset();
        this->setState(ChannelPointsAuthState::SignedOut,
                       QStringLiteral("The code expired. Please try again."));
        return;
    }

    QUrlQuery form;
    form.addQueryItem("client_id", TV_CLIENT_ID);
    form.addQueryItem("device_code", this->deviceCode_->deviceCode);
    form.addQueryItem("grant_type",
                      "urn:ietf:params:oauth:grant-type:device_code");

    auto handle = [this, gen = this->generation_](const NetworkResult &result) {
        if (gen != this->generation_)
        {
            return;
        }

        auto response = parseTokenResponse(result.status(), result.parseJson());
        switch (response.status)
        {
            case TokenResponse::Status::Success:
                logInfo(QStringLiteral("Auth: device code activated"));
                this->deviceCode_.reset();
                this->acceptToken(response.accessToken, response.refreshToken);
                return;

            case TokenResponse::Status::SlowDown:
                this->pollIntervalSec_ += 5;
                [[fallthrough]];
            case TokenResponse::Status::Pending:
                this->pollTimer_.start(this->pollIntervalSec_ * 1000);
                return;

            case TokenResponse::Status::Expired:
                this->deviceCode_.reset();
                this->setState(ChannelPointsAuthState::SignedOut,
                               QStringLiteral("The code expired. Please try again."));
                return;

            case TokenResponse::Status::Failed:
                if (!result.status())
                {
                    // Network trouble, keep polling.
                    this->pollTimer_.start(this->pollIntervalSec_ * 1000);
                    return;
                }
                logWarning(QStringLiteral("Auth: device code polling failed: %1")
                               .arg(response.message));
                this->deviceCode_.reset();
                this->setState(ChannelPointsAuthState::SignedOut,
                               QStringLiteral("Sign-in failed: %1")
                                   .arg(response.message));
                return;
        }
    };

    postForm(TOKEN_URL, form)
        .caller(this)
        .onSuccess(handle)
        .onError(handle)
        .execute();
}

void ChannelPointsAuth::acceptToken(const QString &accessToken,
                                    const QString &refreshToken)
{
    this->token_ = accessToken;
    if (!refreshToken.isEmpty())
    {
        this->refreshToken_ = refreshToken;
    }
    // Persist right away so a crash doesn't lose the sign-in. validate() fills
    // in the login and checks the account matches.
    this->store();
    this->setState(ChannelPointsAuthState::Validating);
    this->validate();
}

}  // namespace chatterino
