// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "debug/MessageCacheStats.hpp"

#include "Application.hpp"
#include "debug/MessageContentStats.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFuture>
#include <QPointer>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <mutex>

namespace chatterino {

namespace {

using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> lastLogWriteNanoseconds{0};
uint64_t imageLayoutInvalidations = 0;
uint64_t layoutBuilds = 0;

void writeRecords(const QString &path, const QString &records)
{
    // A worker must not access Application or any widget. All inputs are copies.
    static std::mutex writeMutex;
    const auto start = Clock::now();
    std::lock_guard lock(writeMutex);
    qCInfo(messageCacheLog).noquote() << records;
    if (!path.isEmpty())
    {
        QFile file(path);
        constexpr qint64 MAX_BYTES = 16 * 1024 * 1024;
        bool ready = true;
        if (file.size() >= MAX_BYTES)
        {
            const auto previous = path + ".1";
            ready = (!QFile::exists(previous) || QFile::remove(previous)) &&
                    file.rename(previous);
            file.setFileName(path);
        }
        if (ready && file.open(QIODevice::WriteOnly | QIODevice::Append |
                               QIODevice::Text))
        {
            const auto bytes = records.toUtf8();
            if (file.write(bytes) != bytes.size() || !file.flush())
            {
                qCWarning(messageCacheLog)
                    << "Unable to write cache performance log:" << path
                    << file.errorString();
            }
        }
        else
        {
            qCWarning(messageCacheLog)
                << "Unable to open or rotate cache performance log:" << path
                << file.errorString();
        }
    }
    lastLogWriteNanoseconds.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             start)
            .count(),
        std::memory_order_relaxed);
}

class CachePerformanceMonitor : public QObject
{
public:
    explicit CachePerformanceMonitor(QObject *parent)
        : QObject(parent)
    {
        this->watchdog_.setInterval(50);
        this->watchdog_.setTimerType(Qt::PreciseTimer);
        connect(&this->watchdog_, &QTimer::timeout, this, [this] {
            const auto now = Clock::now();
            const bool enabled =
                tryGetApp() && getSettings()->cachePerformanceLogging;
            if (enabled && this->enabled_)
            {
                const auto gap =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - this->lastTick_)
                        .count();
                this->eventLoop_.record(static_cast<uint64_t>(
                    std::max<int64_t>(0, gap - 50000000)));
            }
            this->enabled_ = enabled;
            this->lastTick_ = now;
        });
        this->watchdog_.start();
        this->flush_.setInterval(100);
        connect(&this->flush_, &QTimer::timeout, this, [this] {
            this->flushReports();
        });
    }

    void enqueue(const QString &report)
    {
        // Bound queued diagnostics even if the disk or console stops responding.
        constexpr qsizetype MAX_PENDING_CHARACTERS = 1024 * 1024;
        if (this->pendingCharacters_ + report.size() > MAX_PENDING_CHARACTERS)
        {
            ++this->droppedReports_;
            return;
        }
        this->pending_.push_back(report);
        this->pendingCharacters_ += report.size();
        if (!this->flush_.isActive())
        {
            this->flush_.start();
        }
    }

private:
    void flushReports()
    {
        if (this->writer_.isRunning())
        {
            return;
        }
        if (this->pending_.empty() || !tryGetApp())
        {
            this->flush_.stop();
            return;
        }
        const auto now = Clock::now();
        const auto builds = messageLayoutBuildCount();
        const auto global =
            QString("\n\n%1\nRuntime interval: %2 ms; geometry builds: %3; "
                    "image-load invalidations: %4\n"
                    "Previous log batch write: %5 ms; dropped reports: %6\n%7")
                .arg(this->eventLoop_.describeInterval(
                    "Event-loop delay beyond 50 ms"))
                .arg(QLocale().toString(
                    std::chrono::duration<double, std::milli>(now -
                                                              this->lastReport_)
                        .count(),
                    'f', 3))
                .arg(localizeNumbers(builds - this->lastBuilds_))
                .arg(localizeNumbers(imageLayoutInvalidations -
                                     this->lastInvalidations_))
                .arg(QLocale().toString(
                    lastLogWriteNanoseconds.load(std::memory_order_relaxed) /
                        1000000.0,
                    'f', 3))
                .arg(localizeNumbers(this->droppedReports_))
                .arg(describeMessageContentPool());
        const auto timestamp =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString records;
        for (const auto &report : this->pending_)
        {
            records += timestamp + "\n" + report + "\n\n";
        }
        // Global counters appear once per batch, rather than once per split.
        records += "Runtime diagnostics" + global + "\n\n";
        const auto path = messageCachePerformanceLogPath();
        this->writer_ = QtConcurrent::run([path, records = std::move(records)] {
            writeRecords(path, records);
        });
        this->pending_.clear();
        this->pendingCharacters_ = 0;
        this->lastReport_ = now;
        this->lastBuilds_ = builds;
        this->lastInvalidations_ = imageLayoutInvalidations;
        this->eventLoop_.resetInterval();
        this->flush_.stop();
    }

    QTimer watchdog_;
    QTimer flush_;
    QFuture<void> writer_;
    QStringList pending_;
    qsizetype pendingCharacters_ = 0;
    uint64_t droppedReports_ = 0;
    MessageCacheTiming eventLoop_;
    Clock::time_point lastTick_ = Clock::now();
    Clock::time_point lastReport_ = Clock::now();
    uint64_t lastBuilds_ = messageLayoutBuildCount();
    uint64_t lastInvalidations_ = imageLayoutInvalidations;
    bool enabled_ = false;
};

CachePerformanceMonitor *monitor()
{
    static QPointer<CachePerformanceMonitor> instance;
    if (!instance)
    {
        instance = new CachePerformanceMonitor(QCoreApplication::instance());
    }
    return instance;
}

}  // namespace

void recordMessageLayoutBuild()
{
    ++layoutBuilds;
}

uint64_t messageLayoutBuildCount()
{
    return layoutBuilds;
}

void recordImageLayoutInvalidation()
{
    ++imageLayoutInvalidations;
}

void initializeMessageCachePerformance()
{
    (void)monitor();
}

void queueMessageCachePerformance(const QString &report)
{
    monitor()->enqueue(report);
}

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
    writeRecords(messageCachePerformanceLogPath(),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
                     "\n" + report + "\n\n");
}

}  // namespace chatterino
