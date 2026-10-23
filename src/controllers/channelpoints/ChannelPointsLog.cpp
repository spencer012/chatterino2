// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsLog.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace chatterino::channelpoints {

namespace {

constexpr qint64 MAX_LOG_SIZE = 5 * 1024 * 1024;

void appendToFile(const char *level, const QString &message)
{
    const auto path = logFilePath();
    if (path.isEmpty())
    {
        return;
    }

    QFile file(path);
    if (file.exists() && file.size() > MAX_LOG_SIZE)
    {
        const auto rotated =
            QDir(QFileInfo(path).absolutePath()).filePath("channel-points.1.log");
        QFile::remove(rotated);
        QFile::rename(path, rotated);
    }

    if (!file.open(QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' '
           << level << ' ' << message << '\n';
}

}  // namespace

void logInfo(const QString &message)
{
    qCInfo(chatterinoChannelPoints).noquote() << message;
    appendToFile("INFO", message);
}

void logWarning(const QString &message)
{
    qCWarning(chatterinoChannelPoints).noquote() << message;
    appendToFile("WARN", message);
}

QString logFilePath()
{
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return {};
    }
    return QDir(app->getPaths().miscDirectory).filePath("channel-points.log");
}

}  // namespace chatterino::channelpoints
