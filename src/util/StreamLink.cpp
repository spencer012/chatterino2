// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamLink.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/LinkParser.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/StreamQualityCache.hpp"
#include "widgets/dialogs/QualityPopup.hpp"
#include "widgets/dialogs/StreamLinkErrorPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QErrorMessage>
#include <QGuiApplication>
#include <QHash>
#include <QProcess>
#include <QStringBuilder>

#include <chrono>
#include <functional>
#include <memory>

namespace {

using namespace chatterino;

StreamPlayerBackend currentBackend()
{
    return getSettings()->streamPlayerBackend.getEnum();
}

QString backendName(StreamPlayerBackend backend)
{
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        return QStringLiteral("TwitchPipe");
    }

    return QStringLiteral("Streamlink");
}

// Bare logins stay bare. URLs without a scheme get https:// so TwitchPipe's
// parser accepts them. twitch.tv/<name> is a URL with an empty scheme.
QString twitchPipeTarget(const QString &channelOrUrl)
{
    const auto parsed = linkparser::parse(channelOrUrl);
    if (!parsed.has_value())
    {
        return channelOrUrl;
    }

    if (parsed->protocol.isEmpty())
    {
        return QStringLiteral("https://") + channelOrUrl;
    }

    return channelOrUrl;
}

// Leave Streamlink's existing twitch.tv/<name> and full URLs alone. Prefix a
// bare login so a popup opened for TwitchPipe still works if Streamlink is
// selected when OK is clicked.
QString streamlinkTarget(const QString &channelOrUrl)
{
    if (linkparser::parse(channelOrUrl).has_value() ||
        channelOrUrl.contains(QStringLiteral("://")) ||
        channelOrUrl.startsWith(QStringLiteral("twitch.tv/"),
                                Qt::CaseInsensitive))
    {
        return channelOrUrl;
    }

    return QStringLiteral("twitch.tv/") + channelOrUrl;
}

QStringList nonEmptyLines(const QString &text)
{
    QStringList lines;
    for (QString line : text.split(u'\n'))
    {
        line = line.trimmed();
        if (!line.isEmpty())
        {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

QString getStreamlinkPath(StreamPlayerBackend backend)
{
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        if (getSettings()->twitchpipeUseCustomPath)
        {
            const QString path = getSettings()->twitchpipePath;
            return path.trimmed() % "/" % TWITCHPIPE_BINARY_NAME;
        }

        return TWITCHPIPE_BINARY_NAME.toString();
    }

    if (getSettings()->streamlinkUseCustomPath)
    {
        const QString path = getSettings()->streamlinkPath;
        return path.trimmed() % "/" % STREAMLINK_BINARY_NAME;
    }

    return STREAMLINK_BINARY_NAME.toString();
}

void showStreamlinkNotFoundError(StreamPlayerBackend backend)
{
    static auto *msg = new QErrorMessage;

    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        msg->setWindowTitle("Chatterino - twitchpipe not found");

        if (getSettings()->twitchpipeUseCustomPath)
        {
            msg->showMessage("Unable to find TwitchPipe executable\nMake sure "
                             "your custom path is pointing to the DIRECTORY "
                             "where the twitchpipe executable is located");
        }
        else
        {
            msg->showMessage(
                "Unable to find TwitchPipe executable.\nIf you have TwitchPipe "
                "installed, you might need to enable the custom path option");
        }
        return;
    }

    msg->setWindowTitle("Chatterino - streamlink not found");

    if (getSettings()->streamlinkUseCustomPath)
    {
        msg->showMessage("Unable to find Streamlink executable\nMake sure "
                         "your custom path is pointing to the DIRECTORY "
                         "where the streamlink executable is located");
    }
    else
    {
        msg->showMessage(
            "Unable to find Streamlink executable.\nIf you have Streamlink "
            "installed, you might need to enable the custom path option");
    }
}

QProcess *createStreamlinkProcess(StreamPlayerBackend backend)
{
    auto *process = new QProcess;

    const auto path = getStreamlinkPath(backend);

    if (Version::instance().isFlatpak())
    {
        process->setProgram("flatpak-spawn");
        process->setArguments({"--host", path});
    }
    else
    {
        process->setProgram(path);
    }

    return process;
}

ChannelPtr selectedSplitChannel()
{
    SplitContainer *currentPage = getApp()
                                      ->getWindows()
                                      ->getLastSelectedWindow()
                                      ->getNotebook()
                                      .getSelectedPage();
    if (currentPage == nullptr)
    {
        return nullptr;
    }

    auto *currentSplit = currentPage->getSelectedSplit();
    if (currentSplit == nullptr)
    {
        return nullptr;
    }

    return currentSplit->getChannel();
}

// Runs a quality listing process and calls done exactly once. parse turns the
// exit code and output into the result. Errors are only shown when the
// result's reportError is called, so a prefetch can fail quietly.
void runQualityListing(
    QProcess *process, StreamPlayerBackend backend,
    std::function<StreamQualityListing(int exitCode, QProcess *process)> parse,
    StreamQualityCache::DoneCallback done)
{
    auto pending =
        std::make_shared<StreamQualityCache::DoneCallback>(std::move(done));
    auto finish = [pending](StreamQualityListing listing) {
        if (!*pending)
        {
            return;
        }
        auto callback = std::move(*pending);
        *pending = nullptr;
        callback(std::move(listing));
    };

    // FailedToStart does not emit finished. Other errors can emit both
    // signals; deleteLater twice is safe.
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [process, backend, finish](QProcess::ProcessError err) {
                         if (err == QProcess::FailedToStart)
                         {
                             process->deleteLater();
                             finish({
                                 .qualities = {},
                                 .reportError =
                                     [backend] {
                                         showStreamlinkNotFoundError(backend);
                                     },
                             });
                             return;
                         }

                         qCWarning(chatterinoStreamlink)
                             << "Error occurred" << err;
                     });

    QObject::connect(
        process,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        process,
        [process, parse = std::move(parse), finish](
            int exitCode, QProcess::ExitStatus /*exitStatus*/) {
            // Read the output before calling back: showing UI can process
            // events.
            auto listing = parse(exitCode, process);
            process->deleteLater();
            finish(std::move(listing));
        });

    process->start();
}

void getTwitchPipeStreams(const QString &channel,
                          StreamQualityCache::DoneCallback done)
{
    auto *process = createStreamlinkProcess(StreamPlayerBackend::TwitchPipe);

    QStringList args = process->arguments();
    const auto configPath =
        getSettings()->twitchpipeConfigPath.getValue().trimmed();
    if (!configPath.isEmpty())
    {
        args << QStringLiteral("--config") << configPath;
    }
    args << QStringLiteral("--log-level") << QStringLiteral("error")
         << QStringLiteral("--streams") << channel;
    process->setArguments(args);

    runQualityListing(
        process, StreamPlayerBackend::TwitchPipe,
        [](int exitCode, QProcess *process) -> StreamQualityListing {
            const auto stdoutText = QString(process->readAllStandardOutput());
            const auto stderrText = QString(process->readAllStandardError());

            if (exitCode != 0)
            {
                const auto lines = nonEmptyLines(stderrText);
                const auto lastLine =
                    lines.isEmpty() ? QString() : lines.last();
                qCWarning(chatterinoStreamlink)
                    << "Got error code" << exitCode << lastLine;
                return {
                    .qualities = {},
                    .reportError =
                        [lastLine] {
                            auto channel = selectedSplitChannel();
                            if (channel && !lastLine.isEmpty())
                            {
                                channel->addSystemMessage(lastLine);
                            }
                        },
                };
            }

            return {
                .qualities = nonEmptyLines(stdoutText),
                .reportError = {},
            };
        },
        std::move(done));
}

void getStreamQualities(const QString &channelURL,
                        StreamQualityCache::DoneCallback done)
{
    auto *p = createStreamlinkProcess(StreamPlayerBackend::Streamlink);
    // Streamlink prints its errors to stdout, but merge stderr too so the
    // error popup shows everything.
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->setArguments(p->arguments() +
                    QStringList{channelURL, "--default-stream=KKona"});

    runQualityListing(
        p, StreamPlayerBackend::Streamlink,
        [channelURL](int exitCode, QProcess *p) -> StreamQualityListing {
            const auto stdoutText = QString(p->readAllStandardOutput());
            if (exitCode != 0)
            {
                qCWarning(chatterinoStreamlink) << "Got error code" << exitCode;
            }
            QString lastLine = stdoutText;
            lastLine = lastLine.trimmed().split('\n').last().trimmed();
            if (!lastLine.startsWith("Available streams: "))
            {
                return {
                    .qualities = {},
                    .reportError =
                        [channelURL, stdoutText] {
                            StreamLinkErrorPopup::showError(channelURL,
                                                            stdoutText);
                        },
                };
            }

            QStringList options;
            QStringList split =
                lastLine.right(lastLine.length() - 19).split(", ");

            for (auto i = split.length() - 1; i >= 0; i--)
            {
                QString option = split.at(i);
                if (option == "best)")
                {
                    // As it turns out, sometimes, one quality option can
                    // be the best and worst quality at the same time.
                    // Since we start loop from the end, we can check
                    // that and act accordingly
                    option = split.at(--i);
                    // "900p60 (worst"
                    options << option.left(option.length() - 7);
                }
                else if (option.endsWith(" (worst)"))
                {
                    options << option.left(option.length() - 8);
                }
                else if (option.endsWith(" (best)"))
                {
                    options << option.left(option.length() - 7);
                }
                else
                {
                    options << option;
                }
            }

            return {
                .qualities = options,
                .reportError = {},
            };
        },
        std::move(done));
}

StreamQualityCache &qualityCache()
{
    static StreamQualityCache cache;
    return cache;
}

bool choosesQuality(StreamPlayerBackend backend)
{
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        return getSettings()->twitchpipeQuality.getEnum() ==
               TwitchPipeQuality::Choose;
    }
    return getSettings()->preferredQuality.getEnum() ==
           StreamLinkPreferredQuality::Choose;
}

QString qualityCacheKey(StreamPlayerBackend backend, const QString &login)
{
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        // TwitchPipe's config can change what it lists (e.g. its auth).
        return u"twitchpipe|" %
               getSettings()->twitchpipeConfigPath.getValue().trimmed() %
               u"|" % login;
    }
    return u"streamlink|" % login;
}

// What the listing runs against. The quality popup still gets the target the
// user asked for; the qualities are the same either way.
QString qualityListTarget(StreamPlayerBackend backend, const QString &login)
{
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        return login;
    }
    return QStringLiteral("twitch.tv/") + login;
}

StreamQualityCache::Fetcher qualityFetcher(StreamPlayerBackend backend,
                                           const QString &target)
{
    return [backend, target](StreamQualityCache::DoneCallback done) {
        if (backend == StreamPlayerBackend::TwitchPipe)
        {
            getTwitchPipeStreams(target, std::move(done));
        }
        else
        {
            getStreamQualities(target, std::move(done));
        }
    };
}

// How long a Helix lookup for a channel that isn't open is reused, so a click
// right after opening a menu doesn't wait for another one.
constexpr std::chrono::seconds STREAM_CHECK_REUSE{60};

struct StreamCheck {
    QString streamId;
    std::chrono::steady_clock::time_point checkedAt;
};

QHash<QString, StreamCheck> &recentStreamChecks()
{
    static QHash<QString, StreamCheck> checks;
    return checks;
}

// Calls cb with the ID of login's current broadcast, or an empty string when
// it's offline or that can't be told. Each broadcast has its own ID, so a
// stream that went offline and came back gets a new one.
void currentStreamId(const QString &login,
                     const std::function<void(const QString &)> &cb)
{
    // An open channel already keeps its stream status up to date.
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(
        getApp()->getTwitch()->getChannelOrEmpty(login).get());
    if (twitchChannel != nullptr)
    {
        cb(twitchChannel->getCurrentStreamID());
        return;
    }

    auto &checks = recentStreamChecks();
    auto it = checks.find(login);
    if (it != checks.end() &&
        std::chrono::steady_clock::now() - it->checkedAt < STREAM_CHECK_REUSE)
    {
        cb(it->streamId);
        return;
    }

    getHelix()->getStreamByName(
        login,
        [login, cb](bool isLive, const HelixStream &stream) {
            const QString streamId = isLive ? stream.id : QString();
            recentStreamChecks().insert(
                login, {
                           .streamId = streamId,
                           .checkedAt = std::chrono::steady_clock::now(),
                       });
            cb(streamId);
        },
        [cb] {
            cb({});
        },
        [] {});
}

// Shows the quality popup for target. Uses the cached list when the stream is
// still the broadcast it was listed for. Errors are reported.
void chooseQuality(StreamPlayerBackend backend, const QString &channelOrUrl,
                   const QString &target)
{
    auto showPopup = [target](const QStringList &qualities) {
        QualityPopup::showDialog(target, qualities);
    };

    const auto login = twitchLoginOf(channelOrUrl);
    if (login.isEmpty())
    {
        // Not a Twitch channel, so there's no stream ID to cache by.
        qualityCache().request({}, {}, qualityFetcher(backend, target),
                               showPopup);
        return;
    }

    const auto key = qualityCacheKey(backend, login);
    const auto listTarget = qualityListTarget(backend, login);
    currentStreamId(login, [=](const QString &streamId) {
        if (streamId.isEmpty())
        {
            qualityCache().forget(key);
        }
        qualityCache().request(key, streamId,
                               qualityFetcher(backend, listTarget), showPopup);
    });
}

// Choose reads the live modifier because launch callers do not pass the event.
// The quality popup's OK button calls openStreamlink directly and skips this.
bool shiftHeldForBest()
{
    return QGuiApplication::queryKeyboardModifiers().testFlag(
        Qt::ShiftModifier);
}

void launchTwitchPipe(const QString &channelOrUrl)
{
    const QString target = twitchPipeTarget(channelOrUrl);
    const auto preferred = getSettings()->twitchpipeQuality.getEnum();

    if (preferred == TwitchPipeQuality::Choose)
    {
        if (shiftHeldForBest())
        {
            openStreamlink(target, QStringLiteral("best"), {});
            return;
        }

        chooseQuality(StreamPlayerBackend::TwitchPipe, channelOrUrl, target);
        return;
    }

    QString quality;
    if (preferred == TwitchPipeQuality::Best)
    {
        quality = QStringLiteral("best");
    }
    else if (preferred == TwitchPipeQuality::Custom)
    {
        quality = getSettings()->twitchpipeQualityPriority.getValue().trimmed();
        if (quality.isEmpty())
        {
            quality = QStringLiteral("best");
        }
    }

    openStreamlink(target, quality, {});
}

}  // namespace

namespace chatterino {

void openStreamlink(const QString &url, const QString &quality,
                    QStringList extraArguments)
{
    // Read the backend now so the quality popup launches whichever player is
    // selected when OK is clicked.
    const auto backend = currentBackend();
    auto *proc = createStreamlinkProcess(backend);

    QStringList arguments = proc->arguments();
    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        const auto configPath =
            getSettings()->twitchpipeConfigPath.getValue().trimmed();
        if (!configPath.isEmpty())
        {
            arguments << QStringLiteral("--config") << configPath;
        }

        arguments << std::move(extraArguments) << twitchPipeTarget(url)
                  << quality;

        // Remove empty arguments before appending additional options, since
        // the options might purposely contain empty arguments. An empty
        // quality drops the positional so TwitchPipe uses its config list.
        arguments.removeAll(QString());

        arguments << QProcess::splitCommand(
            getSettings()->twitchpipeOpts.getValue());
    }
    else
    {
        arguments << std::move(extraArguments) << streamlinkTarget(url)
                  << quality;

        // Remove empty arguments before appending additional streamlink options
        // as the options might purposely contain empty arguments
        arguments.removeAll(QString());

        arguments << QProcess::splitCommand(
            getSettings()->streamlinkOpts.getValue());
    }

    proc->setArguments(arguments);

    // startDetached does not emit finished, and a failed start also emits
    // errorOccurred. Report failure once from the return value, then delete
    // the QProcess. The detached player keeps running.
    if (!proc->startDetached())
    {
        showStreamlinkNotFoundError(backend);
    }
    proc->deleteLater();
}

void openStreamlinkForChannelOrUrl(const QString &channelOrUrl)
{
    const auto backend = currentBackend();

    if (auto channel = selectedSplitChannel())
    {
        channel->addSystemMessage(QStringLiteral("Opening %1 in %2 ...")
                                      .arg(channelOrUrl, backendName(backend)));
    }

    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        launchTwitchPipe(channelOrUrl);
        return;
    }

    QString url;
    if (linkparser::parse(channelOrUrl).has_value())
    {
        url = channelOrUrl;
    }
    else
    {
        url = "twitch.tv/" + channelOrUrl;
    }

    auto preferredQuality = getSettings()->preferredQuality.getEnum();

    if (preferredQuality == StreamLinkPreferredQuality::Choose)
    {
        if (shiftHeldForBest())
        {
            openStreamlink(url, QStringLiteral("best"), {});
            return;
        }

        chooseQuality(StreamPlayerBackend::Streamlink, channelOrUrl, url);
        return;
    }

    QStringList args;

    // Quality converted from Chatterino format to Streamlink format
    QString quality;
    // Streamlink qualities to exclude
    QString exclude;

    if (preferredQuality == StreamLinkPreferredQuality::High)
    {
        exclude = ">720p30";
        quality = "high,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Medium)
    {
        exclude = ">540p30";
        quality = "medium,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Low)
    {
        exclude = ">360p30";
        quality = "low,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::AudioOnly)
    {
        quality = "audio,audio_only";
    }
    else
    {
        quality = "best";
    }
    if (!exclude.isEmpty())
    {
        args << "--stream-sorting-excludes" << exclude;
    }

    openStreamlink(url, quality, args);
}

void prefetchStreamQualities(const QString &channelOrUrl)
{
    const auto backend = currentBackend();
    if (!choosesQuality(backend))
    {
        return;
    }

    const auto login = twitchLoginOf(channelOrUrl);
    if (login.isEmpty())
    {
        return;
    }

    const auto key = qualityCacheKey(backend, login);
    const auto listTarget = qualityListTarget(backend, login);
    currentStreamId(login, [=](const QString &streamId) {
        if (streamId.isEmpty())
        {
            // Offline, or it can't be told. Either way don't start the player
            // in the background; a click lists qualities as usual.
            qualityCache().forget(key);
            return;
        }
        qualityCache().prefetch(key, streamId,
                                qualityFetcher(backend, listTarget));
    });
}

}  // namespace chatterino
