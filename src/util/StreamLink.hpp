// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <stdexcept>
#include <string>

namespace chatterino {

class Exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#ifdef Q_OS_WIN
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink.exe";
constexpr inline QStringView TWITCHPIPE_BINARY_NAME = u"twitchpipe.exe";
#else
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink";
constexpr inline QStringView TWITCHPIPE_BINARY_NAME = u"twitchpipe";
#endif

// Open the selected stream player for the given url, quality, and extra
// arguments. Additional options from settings are appended.
// The player binary is chosen from settings at call time.
void openStreamlink(const QString &url, const QString &quality,
                    QStringList extraArguments = QStringList());

// Start opening the selected stream player for the given channel or url.
// Reads that player's quality setting and opens a quality dialog when the
// quality is "Choose".
void openStreamlinkForChannelOrUrl(const QString &channelOrUrl);

}  // namespace chatterino
