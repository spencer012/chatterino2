// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamQualitySort.hpp"

#include <QStringView>

#include <algorithm>

namespace chatterino {
namespace {

// Rank only. Callers keep the original selectable text.
constexpr int QUALITY_GROUP_BEST = 0;
constexpr int QUALITY_GROUP_RESOLUTION = 1;
constexpr int QUALITY_GROUP_OTHER = 2;
constexpr int QUALITY_GROUP_WORST = 3;
constexpr int QUALITY_GROUP_AUDIO = 4;

struct StreamQualityRank {
    int group = QUALITY_GROUP_OTHER;
    int height = 0;
    int fps = 0;
};

QString streamQualityName(const QString &label)
{
    const auto trimmed = label.trimmed();
    if (!trimmed.endsWith(u')'))
    {
        return trimmed;
    }

    const auto open = trimmed.lastIndexOf(QStringLiteral(" ("));
    if (open <= 0)
    {
        return trimmed;
    }

    return trimmed.left(open);
}

bool sameQualityName(const QString &name, QStringView expected)
{
    return name.compare(expected, Qt::CaseInsensitive) == 0;
}

bool isAudioOnlyQuality(const QString &name)
{
    return sameQualityName(name, u"audio") ||
           sameQualityName(name, u"audio_only") ||
           sameQualityName(name, u"audio-only");
}

// "1080p60" and "1080p". Anything else, including a suffix glued on, is not a
// resolution.
bool parseStreamResolution(const QString &name, int &height, int &fps)
{
    if (name.isEmpty() || !name.front().isDigit())
    {
        return false;
    }

    int index = 0;
    int parsedHeight = 0;
    while (index < name.size() && name.at(index).isDigit())
    {
        parsedHeight = (parsedHeight * 10) + name.at(index).digitValue();
        ++index;
        if (parsedHeight > 20000)
        {
            return false;
        }
    }

    if (index >= name.size() || name.at(index).toLower() != u'p')
    {
        return false;
    }
    ++index;

    int parsedFps = 0;
    if (index < name.size())
    {
        if (!name.at(index).isDigit())
        {
            return false;
        }

        while (index < name.size() && name.at(index).isDigit())
        {
            parsedFps = (parsedFps * 10) + name.at(index).digitValue();
            ++index;
            if (parsedFps > 1000)
            {
                return false;
            }
        }
    }

    if (index != name.size() || parsedHeight <= 0)
    {
        return false;
    }

    height = parsedHeight;
    fps = parsedFps;
    return true;
}

StreamQualityRank rankStreamQuality(const QString &label)
{
    const auto name = streamQualityName(label);
    StreamQualityRank rank;

    if (isAudioOnlyQuality(name))
    {
        rank.group = QUALITY_GROUP_AUDIO;
        return rank;
    }
    if (sameQualityName(name, u"best"))
    {
        rank.group = QUALITY_GROUP_BEST;
        return rank;
    }
    if (sameQualityName(name, u"worst"))
    {
        rank.group = QUALITY_GROUP_WORST;
        return rank;
    }

    int height = 0;
    int fps = 0;
    if (parseStreamResolution(name, height, fps))
    {
        rank.group = QUALITY_GROUP_RESOLUTION;
        rank.height = height;
        rank.fps = fps;
    }

    return rank;
}

bool streamQualityBefore(const QString &left, const QString &right)
{
    const auto leftRank = rankStreamQuality(left);
    const auto rightRank = rankStreamQuality(right);
    if (leftRank.group != rightRank.group)
    {
        return leftRank.group < rightRank.group;
    }

    if (leftRank.group != QUALITY_GROUP_RESOLUTION)
    {
        return false;
    }

    if (leftRank.height != rightRank.height)
    {
        return leftRank.height > rightRank.height;
    }
    if (leftRank.fps != rightRank.fps)
    {
        return leftRank.fps > rightRank.fps;
    }

    return false;
}

}  // namespace

QStringList sortStreamQualities(QStringList options)
{
    std::stable_sort(options.begin(), options.end(), streamQualityBefore);
    return options;
}

}  // namespace chatterino
