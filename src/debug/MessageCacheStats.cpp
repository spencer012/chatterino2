// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "debug/MessageCacheStats.hpp"

#include "Application.hpp"
#include "singletons/Paths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>

#include <atomic>

namespace chatterino {

uint64_t nextMessageCacheViewId()
{
    static std::atomic<uint64_t> sequence{0};
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

const QString &messageCacheSessionId()
{
    static const auto session =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    return session;
}

QString messageCachePerformanceLogPath()
{
    auto *app = tryGetApp();
    if (!app || app->getPaths().miscDirectory.isEmpty())
    {
        return {};
    }
    return QDir(app->getPaths().miscDirectory)
        .filePath("message-cache-performance.log");
}

void logMessageCachePerformance(const QString &report)
{
    qCInfo(messageCacheLog).noquote() << report;

    const auto path = messageCachePerformanceLogPath();
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    constexpr qint64 MAX_BYTES = 16 * 1024 * 1024;
    if (file.size() >= MAX_BYTES)
    {
        const auto previous = path + ".1";
        if ((QFile::exists(previous) && !QFile::remove(previous)) ||
            !file.rename(previous))
        {
            qCWarning(messageCacheLog)
                << "Unable to rotate cache performance log:" << path;
            return;
        }
        file.setFileName(path);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        qCWarning(messageCacheLog)
            << "Unable to open cache performance log:" << path
            << file.errorString();
        return;
    }
    const auto record =
        (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + "\n" +
         report + "\n\n")
            .toUtf8();
    if (file.write(record) != record.size() || !file.flush())
    {
        qCWarning(messageCacheLog)
            << "Unable to write cache performance log:" << path
            << file.errorString();
    }
}

}  // namespace chatterino
