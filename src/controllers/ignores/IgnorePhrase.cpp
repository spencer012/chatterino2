// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/ignores/IgnorePhrase.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep

namespace chatterino {

IgnorePhrase::IgnorePhrase(const QString &pattern, bool isRegex, bool isBlock,
                           const QString &replace, bool isCaseSensitive,
                           bool highlightOnly, bool suppressMentions,
                           bool suppressHighlight)
    : pattern_(pattern)
    , isRegex_(isRegex)
    , regex_(pattern)
    , isBlock_(isBlock)
    , highlightOnly_(highlightOnly)
    , suppressMentions_(suppressMentions)
    , suppressHighlight_(suppressHighlight)
    , replace_(replace)
    , isCaseSensitive_(isCaseSensitive)
{
    if (this->isCaseSensitive_)
    {
        this->regex_.setPatternOptions(
            QRegularExpression::UseUnicodePropertiesOption);
    }
    else
    {
        this->regex_.setPatternOptions(
            QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::UseUnicodePropertiesOption);
    }
}

bool IgnorePhrase::operator==(const IgnorePhrase &other) const
{
    return std::tie(this->pattern_, this->isRegex_, this->isBlock_,
                    this->highlightOnly_, this->suppressMentions_,
                    this->suppressHighlight_, this->replace_,
                    this->isCaseSensitive_) ==
           std::tie(other.pattern_, other.isRegex_, other.isBlock_,
                    other.highlightOnly_, other.suppressMentions_,
                    other.suppressHighlight_, other.replace_,
                    other.isCaseSensitive_);
}

const QString &IgnorePhrase::getPattern() const
{
    return this->pattern_;
}

bool IgnorePhrase::isRegex() const
{
    return this->isRegex_;
}

bool IgnorePhrase::isRegexValid() const
{
    return this->regex_.isValid();
}

bool IgnorePhrase::isMatch(const QString &subject) const
{
    return !this->pattern_.isEmpty() &&
           (this->isRegex()
                ? (this->regex_.isValid() &&
                   this->regex_.match(subject).hasMatch())
                : subject.contains(this->pattern_, this->caseSensitivity()));
}

const QRegularExpression &IgnorePhrase::getRegex() const
{
    return this->regex_;
}

bool IgnorePhrase::isBlock() const
{
    return this->isBlock_;
}

bool IgnorePhrase::highlightOnly() const
{
    return this->highlightOnly_;
}

bool IgnorePhrase::suppressMentions() const
{
    return this->suppressMentions_;
}

bool IgnorePhrase::suppressHighlight() const
{
    return this->suppressHighlight_;
}

const QString &IgnorePhrase::getReplace() const
{
    return this->replace_;
}

bool IgnorePhrase::isCaseSensitive() const
{
    return this->isCaseSensitive_;
}

Qt::CaseSensitivity IgnorePhrase::caseSensitivity() const
{
    return this->isCaseSensitive_ ? Qt::CaseSensitive : Qt::CaseInsensitive;
}

const std::unordered_map<EmoteName, EmotePtr> &IgnorePhrase::getEmotes() const
{
    return this->emotes_;
}

bool IgnorePhrase::containsEmote() const
{
    if (!this->emotesChecked_)
    {
        auto accemotes =
            getApp()->getAccounts()->twitch.getCurrent()->accessEmotes();
        if (*accemotes)
        {
            for (const auto &emote : **accemotes)
            {
                if (this->replace_.contains(emote.first.string,
                                            Qt::CaseSensitive))
                {
                    this->emotes_.emplace(emote.first, emote.second);
                }
            }
        }
        this->emotesChecked_ = true;
    }
    return !this->emotes_.empty();
}

IgnorePhrase IgnorePhrase::createEmpty()
{
    return {
        {}, false, false, DEFAULT_IGNORE_PHRASE_REPLACE.toString(), true,
        false, false, false,
    };
}

}  // namespace chatterino
