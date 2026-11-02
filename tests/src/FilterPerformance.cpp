// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// Timings for filter evaluation. These are disabled by default, run them with
//   chatterino-test --gtest_also_run_disabled_tests --gtest_filter=*FilterPerformance*
// in a release build.

#include "controllers/filters/lang/Filter.hpp"
#include "lib/LegacyFilter.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "Test.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QtConcurrent>
#include <QThreadPool>

#include <chrono>
#include <cstdio>
#include <functional>
#include <future>

using namespace chatterino;
using namespace chatterino::filters;
using namespace Qt::Literals;

namespace {

using Clock = std::chrono::steady_clock;

/// Parses the IRC lines in benchmarks/resources/recentmessages-nymn.json into
/// the fields filters look at.
std::vector<std::shared_ptr<Message>> loadMessages()
{
    QFile file(
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .filePath(
                u"../../benchmarks/resources/recentmessages-nymn.json"_s));
    if (!file.open(QFile::ReadOnly))
    {
        return {};
    }
    auto lines =
        QJsonDocument::fromJson(file.readAll()).object()["messages"].toArray();

    std::vector<std::shared_ptr<Message>> messages;
    for (const auto &value : lines)
    {
        auto line = value.toString();
        auto privmsg = line.indexOf(u" PRIVMSG #");
        if (!line.startsWith('@') || privmsg < 0)
        {
            continue;
        }
        auto tagsEnd = line.indexOf(u" :");
        auto textStart = line.indexOf(u" :", privmsg);
        if (tagsEnd < 0 || textStart < 0)
        {
            continue;
        }

        auto message = std::make_shared<Message>();
        message->messageText = line.mid(textStart + 2);
        message->channelName = "nymn";
        for (auto tag : QStringView(line).sliced(1, tagsEnd - 1).split(u';'))
        {
            auto eq = tag.indexOf(u'=');
            if (eq < 0)
            {
                continue;
            }
            auto key = tag.first(eq);
            auto val = tag.sliced(eq + 1);
            if (key == u"display-name")
            {
                message->displayName = val.toString();
                message->loginName = val.toString().toLower();
            }
            else if (key == u"user-id")
            {
                message->userID = val.toString();
            }
            else if (key == u"color")
            {
                message->usernameColor = QColor(val);
            }
            else if (key == u"badges")
            {
                for (auto badge : val.split(u',', Qt::SkipEmptyParts))
                {
                    auto slash = badge.indexOf(u'/');
                    message->twitchBadges.emplace_back(
                        badge.first(slash).toString(),
                        badge.sliced(slash + 1).toString());
                }
            }
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

/// Names of the authors the highlights are for. Some messages are rewritten
/// to come from them.
QStringList streamerNames()
{
    QStringList names;
    for (int i = 0; i < 50; i++)
    {
        names.append(u"streamer"_s % QString::number(i));
    }
    return names;
}

std::vector<std::shared_ptr<Message>> workloadMessages()
{
    auto messages = loadMessages();
    auto names = streamerNames();
    const QStringList botLines{
        "!song",
        "Thanks for the follow, someone!",
        "Current song: Darude - Sandstorm",
        "hey chat, what's up",
    };
    // Every 25th message is from one of the authors, a quarter of those are
    // things the filters ignore.
    for (size_t i = 0; i < messages.size(); i += 25)
    {
        auto copy = std::make_shared<Message>();
        copy->displayName =
            names[static_cast<qsizetype>(i / 25) % names.size()];
        copy->loginName = copy->displayName;
        copy->channelName = messages[i]->channelName;
        copy->messageText = botLines[static_cast<qsizetype>(i / 25) % 4];
        messages[i] = std::move(copy);
    }
    return messages;
}

const QStringList IGNORE_PATTERNS{
    R"(^!\w+)",
    R"(Thanks for the follow)",
    R"(^Current song:)",
    R"(has been timed out)",
    R"(https?://\S+)",
    R"(\bsubscribed\b)",
    R"(^\W+$)",
    R"((?i)giveaway)",
    R"(\b(?:bit\.ly|tinyurl)\b)",
    R"(^@\w+)",
};

/// Words for the worst case filters, none of which are common in the sample.
QStringList manyWords(int n)
{
    static const QStringList base{
        "spoiler",  "scam",     "crypto",  "nft",     "followers",
        "primes",   "viewbot",  "promo",   "discord", "telegram",
        "whatsapp", "onlyfans", "casino",  "betting", "steam",
        "giftcard", "freebie",  "airdrop", "wallet",  "seed",
    };
    QStringList out;
    for (int i = 0; i < n; i++)
    {
        out.append(base[i % base.size()] %
                   (i >= base.size() ? QString::number(i) : QString()));
    }
    return out;
}

struct Workload {
    QString name;
    QStringList filters;
};

std::vector<Workload> workloads()
{
    auto names = streamerNames();
    std::vector<Workload> out;

    {
        Workload w{"50x author && !(one alternation regex)"};
        for (const auto &name : names)
        {
            w.filters.append(u"author.name == \""_s % name %
                             u"\" && !(message.content match r\"" %
                             IGNORE_PATTERNS.join('|') % u"\")");
        }
        out.push_back(w);
    }
    {
        Workload w{"50x author && !(10 separate regexes)"};
        for (const auto &name : names)
        {
            QStringList clauses;
            for (const auto &pattern : IGNORE_PATTERNS)
            {
                clauses.append(u"message.content match r\""_s % pattern % u'"');
            }
            w.filters.append(u"author.name == \""_s % name % u"\" && !(" %
                             clauses.join(" || ") % u')');
        }
        out.push_back(w);
    }
    {
        Workload w{"50x !(10 separate regexes) && author (expensive first)"};
        for (const auto &name : names)
        {
            QStringList clauses;
            for (const auto &pattern : IGNORE_PATTERNS)
            {
                clauses.append(u"message.content match r\""_s % pattern % u'"');
            }
            w.filters.append(u"!("_s % clauses.join(" || ") %
                             u") && author.name == \"" % name % u'"');
        }
        out.push_back(w);
    }
    {
        Workload w{"1x 40 separate regexes, no author check"};
        QStringList clauses;
        for (const auto &word : manyWords(40))
        {
            clauses.append(u"message.content match r\"\\b"_s % word % u"\\b\"");
        }
        w.filters.append(clauses.join(" || "));
        out.push_back(w);
    }
    {
        Workload w{"1x 40 separate ri regexes, no author check"};
        QStringList clauses;
        for (const auto &word : manyWords(40))
        {
            clauses.append(u"message.content match ri\"\\b"_s % word %
                           u"\\b\"");
        }
        w.filters.append(clauses.join(" || "));
        out.push_back(w);
    }
    {
        Workload w{"1x 40 contains, no author check"};
        QStringList clauses;
        for (const auto &word : manyWords(40))
        {
            clauses.append(u"message.content contains \""_s % word % u'"');
        }
        w.filters.append(clauses.join(" || "));
        out.push_back(w);
    }
    {
        Workload w{"existing benchmarks/src/Filters.cpp filters (9)"};
        w.filters = {
            R"(channel.name == "nymn" && author.badges contains "moderator")",
            R"((author.badges contains "moderator") && (message.content contains "forsenParty"))",
            R"(message.length < 40 || author.subbed)",
            R"(!flags.sub_message)",
            R"(!author.no_color)",
            R"((message.content match r"^(?!.*(?:my|complex|(re.*x))).*$"))",
            R"((author.subbed && author.sub_length >= 6) || flags.system_message || flags.first_message || flags.automod || flags.sub_message)",
            R"(!author.no_color && message.content contains "EDM")",
            R"(message.content contains "EDM")",
        };
        out.push_back(w);
    }
    return out;
}

/// Something that evaluates a list of filters on a message.
using Evaluator = std::function<int(const Message &)>;

Evaluator legacyEvaluator(const QStringList &filters)
{
    auto expressions = std::make_shared<std::vector<ExpressionPtr>>();
    for (const auto &text : filters)
    {
        legacy::FilterParser parser(text);
        EXPECT_TRUE(parser.valid()) << text;
        expressions->push_back(parser.release());
    }
    return [expressions](const Message &message) {
        RunContext ctx{.message = message, .channel = nullptr};
        int matches = 0;
        for (const auto &expression : *expressions)
        {
            matches += expression->execute(ctx).toBool() ? 1 : 0;
        }
        return matches;
    };
}

Evaluator currentEvaluator(const QStringList &filters,
                           const OptimizeOptions &options)
{
    auto parsed = std::make_shared<std::vector<Filter>>();
    for (const auto &text : filters)
    {
        auto result = Filter::fromString(text, options);
        EXPECT_TRUE(std::holds_alternative<Filter>(result)) << text;
        parsed->push_back(std::move(std::get<Filter>(result)));
    }
    return [parsed](const Message &message) {
        RunContext ctx{.message = message, .channel = nullptr};
        int matches = 0;
        for (const auto &filter : *parsed)
        {
            matches += filter.execute(ctx).toBool() ? 1 : 0;
        }
        return matches;
    };
}

/// Average nanoseconds per message, best of `rounds` passes over all
/// messages.
double nsPerMessage(const Evaluator &evaluate,
                    const std::vector<std::shared_ptr<Message>> &messages,
                    int &matches, int rounds = 7)
{
    double best = std::numeric_limits<double>::max();
    for (int round = 0; round < rounds; round++)
    {
        int total = 0;
        auto start = Clock::now();
        // At least ~20ms per pass
        int passes = 0;
        do
        {
            for (const auto &message : messages)
            {
                total += evaluate(*message);
            }
            passes++;
        } while (Clock::now() - start < std::chrono::milliseconds(20));
        auto elapsed =
            std::chrono::duration<double, std::nano>(Clock::now() - start)
                .count();
        best = std::min(best, elapsed / static_cast<double>(passes) /
                                  static_cast<double>(messages.size()));
        matches = total / passes;
    }
    return best;
}

double nsPer(const std::function<void()> &fn, int iterations)
{
    double best = std::numeric_limits<double>::max();
    for (int round = 0; round < 5; round++)
    {
        auto start = Clock::now();
        for (int i = 0; i < iterations; i++)
        {
            fn();
        }
        auto elapsed =
            std::chrono::duration<double, std::nano>(Clock::now() - start)
                .count();
        best = std::min(best, elapsed / iterations);
    }
    return best;
}

}  // namespace

TEST(FilterPerformance, DISABLED_Workloads)
{
    auto messages = workloadMessages();
    ASSERT_GT(messages.size(), 400U);

    qsizetype totalLength = 0;
    for (const auto &message : messages)
    {
        totalLength += message->messageText.size();
    }
    std::printf(
        "%zu messages, average length %.1f, regex JIT %s\n", messages.size(),
        static_cast<double>(totalLength) / static_cast<double>(messages.size()),
        qEnvironmentVariableIsSet("QT_ENABLE_REGEXP_JIT")
            ? qPrintable(qEnvironmentVariable("QT_ENABLE_REGEXP_JIT"))
            : "default");

    struct Variant {
        const char *name;
        std::function<Evaluator(const QStringList &)> make;
    };
    std::vector<Variant> variants{
        {"legacy (no short-circuit)", legacyEvaluator},
        {"short-circuit",
         [](const auto &f) {
             return currentEvaluator(f, {.reorder = false,
                                         .mergeRegexes = false,
                                         .foldContains = false});
         }},
        {"short-circuit + reorder",
         [](const auto &f) {
             return currentEvaluator(f, {.reorder = true,
                                         .mergeRegexes = false,
                                         .foldContains = false});
         }},
        {"short-circuit + merge regexes",
         [](const auto &f) {
             return currentEvaluator(f, {.reorder = false,
                                         .mergeRegexes = true,
                                         .foldContains = false});
         }},
        {"short-circuit + fold contains",
         [](const auto &f) {
             return currentEvaluator(f, {.reorder = false,
                                         .mergeRegexes = false,
                                         .foldContains = true});
         }},
        {"all (default)",
         [](const auto &f) {
             return currentEvaluator(f, {});
         }},
    };

    for (const auto &workload : workloads())
    {
        std::printf("\n%s\n", qPrintable(workload.name));
        int expectedMatches = -1;
        for (const auto &variant : variants)
        {
            auto evaluate = variant.make(workload.filters);
            int matches = 0;
            auto ns = nsPerMessage(evaluate, messages, matches);
            std::printf("  %-34s %10.1f ns/message  (%d matches/pass)\n",
                        variant.name, ns, matches);
            if (expectedMatches < 0)
            {
                expectedMatches = matches;
            }
            EXPECT_EQ(matches, expectedMatches) << variant.name;
        }
    }
}

TEST(FilterPerformance, DISABLED_SingleContains)
{
    auto messages = workloadMessages();
    for (const auto *needle : {"EDM", "forsenParty", "giveaway", "a"})
    {
        auto text = u"message.content contains \""_s % needle % u'"';
        std::printf("%s\n", qPrintable(text));
        for (bool fold : {false, true})
        {
            auto evaluate = currentEvaluator({text}, {.foldContains = fold});
            int matches = 0;
            auto ns = nsPerMessage(evaluate, messages, matches);
            std::printf("  fold contains %-5s %8.1f ns/message  (%d matches)\n",
                        fold ? "on" : "off", ns, matches);
        }
    }
}

TEST(FilterPerformance, DISABLED_Operations)
{
    Message message;
    message.displayName = "someviewer";
    message.messageText =
        "forsenParty EDM that drop was insane, can we get a replay pls";
    RunContext ctx{.message = message, .channel = nullptr};

    auto timeFilter = [&](const QString &text) {
        auto result = Filter::fromString(text);
        auto filter = std::move(std::get<Filter>(result));
        auto ns = nsPer(
            [&] {
                auto v = filter.execute(ctx);
                ASSERT_TRUE(v.isValid());
            },
            200000);
        std::printf("  %-60s %8.1f ns\n", qPrintable(text), ns);
    };

    std::printf("Single operations on a %lld character message\n",
                static_cast<long long>(message.messageText.size()));
    timeFilter(R"(flags.action)");
    timeFilter(R"(author.name == "streamer1")");
    timeFilter(R"(author.badges contains "moderator")");
    timeFilter(R"(message.content contains "giveaway")");
    timeFilter(
        R"(message.content contains "giveaway" || message.content contains "scam")");
    timeFilter(R"(message.content startswith "!")");
    timeFilter(R"(message.content match r"^!\w+")");
    timeFilter(R"(message.content match r"\bgiveaway\b")");
    timeFilter(R"(message.content match ri"\bgiveaway\b")");
    timeFilter(u"message.content match r\""_s % IGNORE_PATTERNS.join('|') %
               u'"');

    QRegularExpression direct(R"(\bgiveaway\b)");
    auto text = message.messageText;
    auto ns = nsPer(
        [&] {
            ASSERT_FALSE(direct.match(text).hasMatch());
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "QRegularExpression::match directly", ns);
    ns = nsPer(
        [&] {
            ASSERT_FALSE(direct.matchView(text).hasMatch());
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "QRegularExpression::matchView directly",
                ns);
    ns = nsPer(
        [&] {
            ASSERT_FALSE(text.contains(u"giveaway", Qt::CaseInsensitive));
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "QString::contains(CaseInsensitive)", ns);
    ns = nsPer(
        [&] {
            ASSERT_FALSE(text.contains(u"giveaway", Qt::CaseSensitive));
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "QString::contains(CaseSensitive)", ns);

    QString name = message.displayName;
    QString other = "streamer1";
    ns = nsPer(
        [&] {
            ASSERT_NE(name.compare(other, Qt::CaseInsensitive), 0);
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "QString::compare(CaseInsensitive)", ns);
    QVariant nameVariant(name);
    QVariant otherVariant(other);
    ns = nsPer(
        [&] {
            ASSERT_NE(nameVariant.toString().compare(otherVariant.toString(),
                                                     Qt::CaseInsensitive),
                      0);
        },
        200000);
    std::printf("  %-60s %8.1f ns\n", "same, through QVariant::toString", ns);
}

TEST(FilterPerformance, DISABLED_Threads)
{
    auto *pool = QThreadPool::globalInstance();
    std::printf("Thread pool max threads: %d\n", pool->maxThreadCount());

    // Wake up the pool
    QtConcurrent::run(pool, [] {
        return 1;
    }).waitForFinished();

    auto ns = nsPer(
        [&] {
            auto f = QtConcurrent::run(pool, [] {
                return 1;
            });
            ASSERT_EQ(f.result(), 1);
        },
        20000);
    std::printf("  QtConcurrent::run + result() round trip     %8.1f ns\n", ns);

    ns = nsPer(
        [&] {
            auto f = std::async(std::launch::async, [] {
                return 1;
            });
            ASSERT_EQ(f.get(), 1);
        },
        5000);
    std::printf("  std::async(launch::async) + get() round trip %8.1f ns\n",
                ns);

    // Per-message: 50 highlights split into chunks evaluated in parallel
    auto messages = workloadMessages();
    auto all = workloads();
    for (const auto *name : {"50x author && !(10 separate regexes)",
                             "1x 40 separate regexes, no author check"})
    {
        auto it = std::ranges::find_if(all, [&](const auto &w) {
            return w.name == name;
        });
        ASSERT_NE(it, all.end());
        std::printf("\n%s\n", name);

        for (bool optimized : {false, true})
        {
            auto evaluate = optimized ? currentEvaluator(it->filters, {})
                                      : legacyEvaluator(it->filters);
            int matches = 0;
            auto serial = nsPerMessage(evaluate, messages, matches, 3);

            // Split the filters into 4 parts and run 3 of them on the pool
            std::vector<Evaluator> parts;
            const qsizetype chunks = std::min<qsizetype>(4, it->filters.size());
            for (qsizetype c = 0; c < chunks; c++)
            {
                QStringList part;
                for (qsizetype i = c; i < it->filters.size(); i += chunks)
                {
                    part.append(it->filters[i]);
                }
                parts.push_back(optimized ? currentEvaluator(part, {})
                                          : legacyEvaluator(part));
            }
            Evaluator parallel = [&](const Message &message) {
                std::vector<QFuture<int>> futures;
                for (size_t c = 1; c < parts.size(); c++)
                {
                    futures.push_back(QtConcurrent::run(pool, [&, c] {
                        return parts[c](message);
                    }));
                }
                int total = parts[0](message);
                for (auto &f : futures)
                {
                    total += f.result();
                }
                return total;
            };
            int parallelMatches = 0;
            auto par = nsPerMessage(parallel, messages, parallelMatches, 3);
            EXPECT_EQ(matches, parallelMatches);
            std::printf("  %-10s per message: serial %9.1f ns, %lld-way "
                        "parallel %9.1f ns\n",
                        optimized ? "optimized" : "legacy", serial,
                        static_cast<long long>(parts.size()), par);
        }
    }

    // Bulk: re-filtering a split's buffer when its filter changes
    std::vector<std::shared_ptr<Message>> buffer;
    while (buffer.size() < 10000)
    {
        for (const auto &m : messages)
        {
            buffer.push_back(m);
        }
    }
    buffer.resize(10000);

    for (const auto *name : {"50x author && !(10 separate regexes)",
                             "1x 40 separate regexes, no author check"})
    {
        auto it = std::ranges::find_if(all, [&](const auto &w) {
            return w.name == name;
        });
        std::printf("\nRe-filter %zu messages: %s\n", buffer.size(), name);
        for (bool optimized : {false, true})
        {
            auto evaluate = optimized ? currentEvaluator(it->filters, {})
                                      : legacyEvaluator(it->filters);
            std::vector<char> keep(buffer.size());

            auto serialNs = nsPer(
                [&] {
                    for (size_t i = 0; i < buffer.size(); i++)
                    {
                        keep[i] = evaluate(*buffer[i]) == 0;
                    }
                },
                3);
            auto serialKept = std::ranges::count(keep, 1);

            std::vector<size_t> starts;
            const size_t chunk = 256;
            for (size_t i = 0; i < buffer.size(); i += chunk)
            {
                starts.push_back(i);
            }
            auto parallelNs = nsPer(
                [&] {
                    QtConcurrent::blockingMap(pool, starts, [&](size_t start) {
                        auto end = std::min(start + chunk, buffer.size());
                        for (size_t i = start; i < end; i++)
                        {
                            keep[i] = evaluate(*buffer[i]) == 0;
                        }
                    });
                },
                3);
            EXPECT_EQ(std::ranges::count(keep, 1), serialKept);
            std::printf("  %-10s serial %8.2f ms, parallel %8.2f ms\n",
                        optimized ? "optimized" : "legacy", serialNs / 1e6,
                        parallelNs / 1e6);
        }
    }
}
