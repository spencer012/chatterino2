// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/ExternalToolsPage.hpp"

#include "controllers/spellcheck/SpellChecker.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"
#include "util/Helpers.hpp"
#include "util/ImageUploader.hpp"
#include "util/StreamLink.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

inline const QStringList STREAMLINK_QUALITY = {
    "Choose", "Source", "High", "Medium", "Low", "Audio only",
};

QLabel *makeWrappedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                   Qt::LinksAccessibleByKeyboard);
    label->setOpenExternalLinks(true);
    return label;
}

void exportImageUploaderSettings(QWidget *parent)
{
    const auto &s = *getSettings();

    QJsonObject settingsObj = imageuploader::detail::exportSettings(s);
    QJsonDocument doc(settingsObj);
    crossPlatformCopy(doc.toJson(QJsonDocument::Indented));

    QMessageBox::information(
        parent, "Settings Exported",
        "Image uploader settings have been copied to clipboard as JSON.");
}

void importImageUploaderSettings(QWidget *parent)
{
    QString clipboardText = getClipboardText().trimmed();

    auto res = imageuploader::detail::validateImportJson(clipboardText);
    if (!res)
    {
        QMessageBox::warning(
            parent, "Import Failed",
            QString("Error validating image uploader import: %1.")
                .arg(res.error()));
        return;
    }
    const auto &settingsObj = *res;

    int ret = QMessageBox::question(
        parent, "Import Settings",
        "This will overwrite your current image uploader settings. Continue?",
        QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes)
    {
        return;
    }

    auto &s = *getSettings();
    if (imageuploader::detail::importSettings(settingsObj, s))
    {
        QMessageBox::information(
            parent, "Import Successful",
            "Image uploader settings have been imported successfully!");
    }
    else
    {
        QMessageBox::warning(
            parent, "Import Failed",
            "No valid image uploader settings found in the JSON.");
    }
}

}  // namespace

ExternalToolsPage::ExternalToolsPage()
    : view(GeneralPageView::withoutNavigation(this))
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    x->addWidget(this->view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    this->initLayout(*this->view);
}

bool ExternalToolsPage::filterElements(const QString &query)
{
    if (this->view)
    {
        return this->view->filterElements(query) || query.isEmpty();
    }

    return false;
}

void ExternalToolsPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    {
        layout.addTitle("Stream player");

        SettingWidget::dropdown("Stream player", s.streamPlayerBackend)
            ->addTo(layout);

        const QString streamlinkDescription =
            QStringLiteral("Streamlink is a command-line utility that pipes "
                           "video streams from various services into a video "
                           "player, such as VLC. Make sure to edit the "
                           "configuration file before you use it!");
        const QString streamlinkLinks =
            formatRichNamedLink("https://streamlink.github.io/", "Website") +
            " " +
            formatRichNamedLink(
                "https://github.com/streamlink/streamlink/releases/latest",
                "Download") +
            " " +
            formatRichNamedLink("https://streamlink.github.io/cli.html#twitch",
                                "Documentation");
        const QString streamlinkBinaryNote =
            QStringLiteral(
                "Chatterino expects the executable to be called \"%1\".")
                .arg(STREAMLINK_BINARY_NAME);
        const QString streamlinkPathLabel =
            QStringLiteral("Use custom path (Enable if using non-standard "
                           "streamlink installation path)");

        auto *streamlinkBox = new QGroupBox(QStringLiteral("Streamlink"));
        auto *streamlinkOuter = new QVBoxLayout(streamlinkBox);
        auto *streamlinkForm = new QFormLayout;
        streamlinkForm->setContentsMargins(0, 0, 0, 0);
        streamlinkOuter->addWidget(makeWrappedLabel(streamlinkDescription));
        streamlinkOuter->addWidget(makeWrappedLabel(streamlinkLinks));
        // addTo() registers the checkbox for search and inserts it in the page
        // layout. Reparent it into this group afterwards.
        auto *streamlinkPathCheck = SettingWidget::checkbox(
            streamlinkPathLabel, s.streamlinkUseCustomPath);
        streamlinkPathCheck->addTo(layout);
        streamlinkOuter->addWidget(streamlinkPathCheck);
        streamlinkOuter->addWidget(makeWrappedLabel(streamlinkBinaryNote));
        streamlinkOuter->addLayout(streamlinkForm);

        SettingWidget::lineEdit(
            "Custom streamlink path", s.streamlinkPath,
            "Path to folder where Streamlink executable can be found")
            ->conditionallyEnabledBy(s.streamlinkUseCustomPath)
            ->addTo(layout, streamlinkForm);

        SettingWidget::dropdown("Preferred quality", s.preferredQuality)
            ->addTo(layout, streamlinkForm);

        SettingWidget::lineEdit("Additional options", s.streamlinkOpts, "")
            ->addTo(layout, streamlinkForm);

        layout.addWidget(
            streamlinkBox,
            {QStringLiteral("Streamlink"), streamlinkDescription,
             streamlinkLinks, streamlinkBinaryNote, streamlinkPathLabel,
             QStringLiteral("Custom streamlink path"),
             QStringLiteral("Preferred quality"),
             QStringLiteral("Additional options")});

        const QString twitchpipeDescription = QStringLiteral(
            "TwitchPipe plays a Twitch stream in your video player. Choose "
            "runs twitchpipe --log-level error --streams and lists one quality "
            "name per line. Those names are passed through when you launch.");
        const QString twitchpipeBinaryNote =
            QStringLiteral(
                "Chatterino expects the executable to be called \"%1\".")
                .arg(TWITCHPIPE_BINARY_NAME);
        const QString twitchpipePathLabel = QStringLiteral("Use custom path");

        auto *twitchpipeBox = new QGroupBox(QStringLiteral("TwitchPipe"));
        auto *twitchpipeOuter = new QVBoxLayout(twitchpipeBox);
        auto *twitchpipeForm = new QFormLayout;
        twitchpipeForm->setContentsMargins(0, 0, 0, 0);
        twitchpipeOuter->addWidget(makeWrappedLabel(twitchpipeDescription));
        twitchpipeOuter->addWidget(makeWrappedLabel(twitchpipeBinaryNote));
        auto *twitchpipePathCheck = SettingWidget::checkbox(
            twitchpipePathLabel, s.twitchpipeUseCustomPath);
        twitchpipePathCheck->addTo(layout);
        twitchpipeOuter->addWidget(twitchpipePathCheck);
        twitchpipeOuter->addLayout(twitchpipeForm);

        SettingWidget::lineEdit(
            "Custom twitchpipe path", s.twitchpipePath,
            "Path to folder where the TwitchPipe executable can be found")
            ->conditionallyEnabledBy(s.twitchpipeUseCustomPath)
            ->addTo(layout, twitchpipeForm);

        SettingWidget::dropdown("Preferred quality", s.twitchpipeQuality)
            ->addTo(layout, twitchpipeForm);

        SettingWidget::lineEdit("Quality priority list",
                                s.twitchpipeQualityPriority,
                                "1080p60,720p60,best")
            ->addKeywords({QStringLiteral("1080p60,720p60,best")})
            ->conditionallyEnabledBy(s.twitchpipeQuality,
                                     TwitchPipeQuality::Custom)
            ->addTo(layout, twitchpipeForm);

        SettingWidget::lineEdit(
            "Config file", s.twitchpipeConfigPath,
            "Optional: path to config.toml (default: next to the executable)")
            ->addKeywords({QStringLiteral("config.toml")})
            ->addTo(layout, twitchpipeForm);

        SettingWidget::lineEdit("Additional options", s.twitchpipeOpts, "")
            ->addTo(layout, twitchpipeForm);

        layout.addWidget(
            twitchpipeBox,
            {QStringLiteral("TwitchPipe"), twitchpipeDescription,
             twitchpipeBinaryNote, twitchpipePathLabel,
             QStringLiteral("Custom twitchpipe path"),
             QStringLiteral("Preferred quality"),
             QStringLiteral("Quality priority list"),
             QStringLiteral("1080p60,720p60,best"),
             QStringLiteral("Config file"), QStringLiteral("config.toml"),
             QStringLiteral("--streams"),
             QStringLiteral("Additional options")});

        s.streamPlayerBackend.connect(
            [streamlinkBox, twitchpipeBox](const QString &) {
                const auto backend =
                    getSettings()->streamPlayerBackend.getEnum();
                streamlinkBox->setEnabled(backend ==
                                          StreamPlayerBackend::Streamlink);
                twitchpipeBox->setEnabled(backend ==
                                          StreamPlayerBackend::TwitchPipe);
            },
            this->managedConnections_);
    }

    {
        layout.addTitle("Custom stream player");
        layout.addDescription(
            "You can open Twitch streams directly in any video player that has "
            "built-in Twitch support and its own URI Scheme.\nE.g.: IINA for "
            "macOS and PotPlayer (with extension) for Windows.\n\nWith this "
            "value set, you will get the option to \"Open in custom player\" "
            "when right-clicking a channel header.");

        SettingWidget::lineEdit("Custom stream player URI Scheme",
                                s.customURIScheme, "custom-player-scheme://")
            ->addTo(layout);
    }

    {
        auto *form = new QFormLayout;
        layout.addTitle("Image Uploader");

        layout.addDescription(
            "You can set custom host for uploading images, like imgur.com or "
            "s-ul.eu.<br>Check " +
            formatRichNamedLink("https://chatterino.com/help/image-uploader",
                                "this guide") +
            " for help.");

        SettingWidget::checkbox("Enable image uploader", s.imageUploaderEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Ask for confirmation when uploading an image",
                                s.askOnImageUpload)
            ->addTo(layout);

        layout.addLayout(form);

        SettingWidget::lineEdit("Request URL", s.imageUploaderUrl)
            ->addTo(layout, form);

        SettingWidget::lineEdit("Form field", s.imageUploaderFormField)
            ->addTo(layout, form);

        SettingWidget::lineEdit("Extra Headers", s.imageUploaderHeaders)
            ->addTo(layout, form);

        SettingWidget::lineEdit("Image link", s.imageUploaderLink)
            ->addTo(layout, form);

        SettingWidget::lineEdit("Deletion link", s.imageUploaderDeletionLink)
            ->addTo(layout, form);

        layout.addDescription(
            "Export your current image uploader settings as JSON to share with "
            "others, or import settings from clipboard (compatible with ShareX "
            ".sxcu format).");

        auto *buttonLayout = new QHBoxLayout;

        auto *importButton = new QPushButton("Import Settings from Clipboard");
        importButton->setToolTip(
            "Import image uploader settings from clipboard JSON");
        QObject::connect(importButton, &QPushButton::clicked, [this]() {
            importImageUploaderSettings(this);
        });
        buttonLayout->addWidget(importButton);

        auto *exportButton = new QPushButton("Export Settings to Clipboard");
        exportButton->setToolTip(
            "Copy current image uploader settings to clipboard as JSON");
        QObject::connect(exportButton, &QPushButton::clicked, [this]() {
            exportImageUploaderSettings(this);
        });
        buttonLayout->addWidget(exportButton);

        buttonLayout->addStretch();
        layout.addLayout(buttonLayout);
    }

#ifdef CHATTERINO_WITH_SPELLCHECK
    {
        // auto *form = new QFormLayout;
        layout.addTitle("Spell checker (experimental)");

        layout.addDescription(
            u"Check the spelling of words in the input box of splits."
            " Chatterino does not include dictionaries - they have to "
            "be downloaded or created manually. Chatterino expects "
            "Hunspell "
            "dictionaries in " %
            formatRichNamedLink(getApp()->getPaths().dictionariesDirectory,
                                getApp()->getPaths().dictionariesDirectory) %
            u". Dictionaries are pairs of .aff (affixes) and .dic (dictionary) "
            u"files.");

        SettingWidget::checkbox("Check spelling by default",
                                s.enableSpellChecking)
            ->setTooltip("Check the spelling of words in the input box of all "
                         "splits by default.")
            ->addTo(layout);
        SettingWidget::intInput("Number of suggestions in context menu",
                                s.nSpellCheckingSuggestions,
                                {
                                    .min = -1,
                                    .max = std::numeric_limits<int>::max(),
                                })
            ->setTooltip(
                "When right-clicking any word, show this many suggestions. If "
                "this is 0, no suggestions will be shown and if it's -1, no "
                "limit is set.")
            ->addTo(layout);

        auto toItem =
            [](const DictionaryInfo &dict) -> std::pair<QString, QVariant> {
            return {
                dict.name,
                dict.path,
            };
        };
        std::vector<std::pair<QString, QVariant>> dictList{{"None", ""}};

        std::ranges::transform(
            getApp()->getSpellChecker()->getAvailableDictionaries(),
            std::back_inserter(dictList), toItem);

        if (dictList.size() > 1)
        {
            SettingWidget::dropdown("Default dictionary (requires restart)",
                                    s.spellCheckingDefaultDictionary, dictList)
                ->addTo(layout);
        }
    }
#endif

    layout.addStretch();
}

}  // namespace chatterino
