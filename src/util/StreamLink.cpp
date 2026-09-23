// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamLink.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/LinkParser.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/QualityPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QErrorMessage>
#include <QGuiApplication>
#include <QProcess>
#include <QStringBuilder>

#include <functional>

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

// Connect after any finished handler that still needs the process. FailedToStart
// does not emit finished. Other errors can emit both signals; deleteLater twice
// is safe, but deleting from errorOccurred before finished can free the process
// while a finished handler is still using it.
void bindAttachedProcessLifetime(QProcess *process, StreamPlayerBackend backend)
{
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [=](QProcess::ProcessError err) {
                         if (err == QProcess::FailedToStart)
                         {
                             showStreamlinkNotFoundError(backend);
                             process->deleteLater();
                             return;
                         }

                         qCWarning(chatterinoStreamlink)
                             << "Error occurred" << err;
                     });

    QObject::connect(
        process,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        process, [=](int /*exitCode*/, QProcess::ExitStatus /*exitStatus*/) {
            process->deleteLater();
        });
}

void getTwitchPipeStreams(const QString &channel, const ChannelPtr &channelPtr,
                          std::function<void(QStringList)> cb)
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

    QObject::connect(
        process,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        process,
        [process, cb = std::move(cb), channelPtr](
            int exitCode, QProcess::ExitStatus /*exitStatus*/) {
            // Copy before any callback. Showing UI can process events.
            const auto stdoutText = QString(process->readAllStandardOutput());
            const auto stderrText = QString(process->readAllStandardError());

            if (exitCode != 0)
            {
                const auto lines = nonEmptyLines(stderrText);
                const auto lastLine =
                    lines.isEmpty() ? QString() : lines.last();
                qCWarning(chatterinoStreamlink)
                    << "Got error code" << exitCode << lastLine;
                if (channelPtr && !lastLine.isEmpty())
                {
                    channelPtr->addSystemMessage(lastLine);
                }
                return;
            }

            cb(nonEmptyLines(stdoutText));
        });

    bindAttachedProcessLifetime(process, StreamPlayerBackend::TwitchPipe);
    process->start();
}

// Choose reads the live modifier because launch callers do not pass the event.
// The quality popup's OK button calls openStreamlink directly and skips this.
bool shiftHeldForBest()
{
    return QGuiApplication::queryKeyboardModifiers().testFlag(
        Qt::ShiftModifier);
}

void launchTwitchPipe(const QString &channelOrUrl, const ChannelPtr &channel)
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

        getTwitchPipeStreams(target, channel, [target](QStringList options) {
            QualityPopup::showDialog(target, std::move(options));
        });
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

void getStreamQualities(const QString &channelURL,
                        std::function<void(QStringList)> cb)
{
    auto *p = createStreamlinkProcess(StreamPlayerBackend::Streamlink);

    QObject::connect(
        p,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        [=](int exitCode, QProcess::ExitStatus /*exitStatus*/) {
            const auto stdoutText = QString(p->readAllStandardOutput());
            if (exitCode != 0)
            {
                qCWarning(chatterinoStreamlink) << "Got error code" << exitCode;
                // return;
            }
            QString lastLine = stdoutText;
            lastLine = lastLine.trimmed().split('\n').last().trimmed();
            if (lastLine.startsWith("Available streams: "))
            {
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

                cb(options);
            }
        });

    bindAttachedProcessLifetime(p, StreamPlayerBackend::Streamlink);

    p->setArguments(p->arguments() +
                    QStringList{channelURL, "--default-stream=KKona"});

    p->start();
}

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

    ChannelPtr channel;
    SplitContainer *currentPage = getApp()
                                      ->getWindows()
                                      ->getLastSelectedWindow()
                                      ->getNotebook()
                                      .getSelectedPage();
    if (currentPage != nullptr)
    {
        auto *currentSplit = currentPage->getSelectedSplit();
        if (currentSplit != nullptr)
        {
            channel = currentSplit->getChannel();
            channel->addSystemMessage(
                QStringLiteral("Opening %1 in %2 ...")
                    .arg(channelOrUrl, backendName(backend)));
        }
    }

    if (backend == StreamPlayerBackend::TwitchPipe)
    {
        launchTwitchPipe(channelOrUrl, channel);
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

        getStreamQualities(url, [=](QStringList qualityOptions) {
            QualityPopup::showDialog(url, qualityOptions);
        });

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

}  // namespace chatterino
