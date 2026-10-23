// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino::channelpoints {

/// Logs to the `chatterino.channelpoints` category and appends to a
/// persistent log file (see logFilePath()), so failed redeems can be
/// diagnosed after the fact. Never pass tokens to these functions.
void logInfo(const QString &message);
void logWarning(const QString &message);

/// <AppData>/Misc/channel-points.log. Rotated to channel-points.1.log at 5 MB.
QString logFilePath();

}  // namespace chatterino::channelpoints
