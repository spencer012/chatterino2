// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"
#include "util/StreamQualitySort.hpp"

using namespace chatterino;

namespace {

QString joined(const QStringList &labels)
{
    return labels.join(u'|');
}

}  // namespace

TEST(StreamLinkQualities, TwitchPipeScreenshotOrder)
{
    const QStringList input{
        QStringLiteral("720p60"),     QStringLiteral("480p"),
        QStringLiteral("360p"),       QStringLiteral("160p"),
        QStringLiteral("audio_only"), QStringLiteral("1080p60"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60|720p60|480p|360p|160p|audio_only"));
}

TEST(StreamLinkQualities, HeightThenFrameRate)
{
    const QStringList input{
        QStringLiteral("720p"),    QStringLiteral("720p30"),
        QStringLiteral("1080p"),   QStringLiteral("720p60"),
        QStringLiteral("1080p60"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60|1080p|720p60|720p30|720p"));
}

TEST(StreamLinkQualities, SourceSuffixStaysSelectable)
{
    const QStringList input{
        QStringLiteral("1080p"),
        QStringLiteral("1080p60 (source)"),
        QStringLiteral("1080p60"),
        QStringLiteral("480p"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60 (source)|1080p60|1080p|480p"));
}

TEST(StreamLinkQualities, NamedAndUnknownLabels)
{
    const QStringList input{
        QStringLiteral("worst"),         QStringLiteral("mystery"),
        QStringLiteral("audio_only"),    QStringLiteral("best"),
        QStringLiteral("720p"),          QStringLiteral("chunked"),
        QStringLiteral("best (source)"), QStringLiteral("160p (worst)"),
        QStringLiteral("audio"),         QStringLiteral("audio-only"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("best|best (source)|720p|160p (worst)|mystery|"
                             "chunked|worst|audio_only|audio|audio-only"));
}

TEST(StreamLinkQualities, CaseDuplicatesAndEmpty)
{
    const QStringList mixed{
        QStringLiteral("AUDIO_ONLY"), QStringLiteral("720p"),
        QStringLiteral("1080P60"),    QStringLiteral("Best"),
        QStringLiteral("720p"),       QStringLiteral("Worst"),
        QStringLiteral("zeta"),       QStringLiteral("alpha"),
    };

    EXPECT_EQ(
        joined(sortStreamQualities(mixed)),
        QStringLiteral("Best|1080P60|720p|720p|zeta|alpha|Worst|AUDIO_ONLY"));
    EXPECT_EQ(sortStreamQualities({}), QStringList());
}
