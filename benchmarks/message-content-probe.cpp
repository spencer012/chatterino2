// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// Replay leaf payloads from daily logs through the production pool. This models
// plain-text body and username word lists, not native emote parsing or GUI RAM.
#include "messages/MessageContentPool.hpp"

#include <boost/circular_buffer.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <chrono>
#include <iostream>
#include <memory>

#ifdef Q_OS_WIN
#    include <psapi.h>
#    include <Windows.h>
#endif

using namespace chatterino;

namespace {

struct Record {
    QString text;
    QStringList usernameWords;
    QStringList bodyWords;
    MessageContentRef textRef;
    MessageContentRef usernameRef;
    MessageContentRef bodyRef;
};

qint64 privateBytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
            sizeof(memory)))
    {
        return static_cast<qint64>(memory.PrivateUsage);
    }
#endif
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5 || (args[4] != "on" && args[4] != "off"))
    {
        std::cerr << "usage: content-probe CHANNEL_LOG_DIRECTORY CHANNEL "
                     "CAPACITY on|off\n";
        return 2;
    }
    bool valid = false;
    const auto capacity = args[3].toUInt(&valid);
    if (!valid || capacity == 0 || capacity > 1000000)
    {
        return 2;
    }
    const bool enabled = args[4] == "on";
    MessageContentPool pool({.enabled = enabled});
    boost::circular_buffer<std::unique_ptr<Record>> history(capacity);
    const QDir directory(args[1]);
    auto files = directory.entryList({args[2] + "-????-??-??.log"}, QDir::Files,
                                     QDir::Name);
    if (files.size() > 30)
    {
        files = files.last(30);
    }
    if (files.isEmpty())
    {
        return 2;
    }
    const QRegularExpression linePattern(
        R"(^\[\d{2}:\d{2}:\d{2}\] ((?:\S+ )?[A-Za-z0-9_]{1,25}): (.*)$)");
    quint64 parsed = 0;
    const auto started = std::chrono::steady_clock::now();
    for (const auto &name : files)
    {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
        {
            return 3;
        }
        while (!file.atEnd())
        {
            auto bytes = file.readLine();
            if (bytes.endsWith('\n'))
                bytes.chop(1);
            if (bytes.endsWith('\r'))
                bytes.chop(1);
            const auto match = linePattern.match(QString::fromUtf8(bytes));
            if (!match.hasMatch())
                continue;
            auto record = std::make_unique<Record>();
            record->text = match.captured(2);
            record->usernameWords = QStringList{match.captured(1) + ":"};
            // The real parser converts body token views to owned strings before
            // constructing text elements. Keep this independent of messageText.
            record->bodyWords = QStringView(record->text).toString().split(' ');
            if (enabled)
            {
                pool.shareWords(record->usernameWords, record->usernameRef);
                pool.shareWords(record->bodyWords, record->bodyRef);
                pool.shareText(record->text, record->textRef);
            }
            history.push_back(std::move(record));
            ++parsed;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const auto stats = pool.stats();
    QJsonObject output{
        {"channel", args[2]},
        {"enabled", enabled},
        {"daily_files", files.size()},
        {"parsed_messages", static_cast<qint64>(parsed)},
        {"retained_messages", static_cast<qint64>(history.size())},
        {"scope", "leaf payload replay; all bodies treated as plain text; "
                  "excludes GUI and emote elements"},
        {"private_process_bytes", privateBytes()},
        {"elapsed_ms", elapsed},
        {"estimated_current_net_saved_bytes", stats.retainedNetSavedBytes},
        {"estimated_current_gross_saved_bytes",
         static_cast<qint64>(stats.retainedGrossSavedBytes)},
        {"estimated_pool_overhead_bytes",
         static_cast<qint64>(stats.estimatedOverheadBytes)},
        {"tracking_slot_bytes", static_cast<qint64>(stats.trackingSlotBytes)},
        {"live_payloads", static_cast<qint64>(stats.liveEntries)},
        {"indexed_payloads", static_cast<qint64>(stats.indexedEntries)},
        {"unindexed_live_payloads",
         static_cast<qint64>(stats.unindexedLiveEntries)},
        {"hits", static_cast<qint64>(stats.hits)},
        {"cumulative_released_bytes",
         static_cast<qint64>(stats.cumulativeDeduplicatedBytes)},
        {"pool_lookup_ms",
         static_cast<double>(stats.processingTimeNs) / 1000000.0},
    };
    history.clear();
    const auto after = pool.stats();
    output.insert("live_payloads_after_clear",
                  static_cast<qint64>(after.liveEntries));
    output.insert("tracking_slots_after_clear",
                  static_cast<qint64>(after.trackingSlots));
    std::cout
        << QJsonDocument(output).toJson(QJsonDocument::Indented).constData();
}
