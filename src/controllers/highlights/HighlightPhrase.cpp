// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightPhrase.hpp"

#include <QStringBuilder>

namespace chatterino {

namespace {

constexpr QStringView REGEX_START_BOUNDARY(u"(?:\\b|(?<=\\s)|^)");
constexpr QStringView REGEX_END_BOUNDARY(u"(?:\\b|(?=\\s)|$)");
constexpr std::size_t MAX_RETAINED_MATCHES = 128;

}

QColor HighlightPhrase::FALLBACK_HIGHLIGHT_COLOR = QColor(127, 63, 73, 127);
QColor HighlightPhrase::FALLBACK_SELF_MESSAGE_HIGHLIGHT_COLOR =
    QColor(0, 118, 221, 115);
QColor HighlightPhrase::FALLBACK_REDEEMED_HIGHLIGHT_COLOR =
    QColor(28, 126, 141, 60);
QColor HighlightPhrase::FALLBACK_FIRST_MESSAGE_HIGHLIGHT_COLOR =
    QColor(72, 127, 63, 60);
QColor HighlightPhrase::FALLBACK_ELEVATED_MESSAGE_HIGHLIGHT_COLOR =
    QColor(255, 174, 66, 60);
QColor HighlightPhrase::FALLBACK_THREAD_HIGHLIGHT_COLOR =
    QColor(143, 48, 24, 60);
QColor HighlightPhrase::FALLBACK_SUB_COLOR = QColor(196, 102, 255, 100);
QColor HighlightPhrase::FALLBACK_AUTOMOD_HIGHLIGHT_COLOR = QColor(64, 64, 64, 140);
QColor HighlightPhrase::FALLBACK_WATCH_STREAK_COLOR = QColor(0, 130, 255, 70);

QColor HighlightPhrase::FALLBACK_ANNOUNCEMENT_HIGHLIGHT_COLOR =
    QColor(255, 102, 237, 100);

QColor HighlightPhrase::ANNOUNCEMENT_BLUE_HIGHLIGHT_COLOR =
    QColor(102, 148, 255, 100);
QColor HighlightPhrase::ANNOUNCEMENT_GREEN_HIGHLIGHT_COLOR =
    QColor(96, 255, 96, 100);
QColor HighlightPhrase::ANNOUNCEMENT_ORANGE_HIGHLIGHT_COLOR =
    QColor(233, 210, 0, 100);
QColor HighlightPhrase::ANNOUNCEMENT_PURPLE_HIGHLIGHT_COLOR =
    QColor(255, 102, 237, 100);

bool HighlightPhrase::operator==(const HighlightPhrase &other) const
{
    return std::tie(this->pattern_, this->showInMentions_, this->hasSound_,
                    this->hasAlert_, this->isRegex_, this->isCaseSensitive_,
                    this->soundUrl_, this->color_, this->matchColor_,
                    this->matchStyle_, this->matchPaintID_,
                    this->channelScope_) ==
           std::tie(other.pattern_, other.showInMentions_, other.hasSound_,
                    other.hasAlert_, other.isRegex_, other.isCaseSensitive_,
                    other.soundUrl_, other.color_, other.matchColor_,
                    other.matchStyle_, other.matchPaintID_,
                    other.channelScope_);
}

HighlightPhrase::HighlightPhrase(const QString &pattern, bool showInMentions,
                                 bool hasAlert, bool hasSound, bool isRegex,
                                 bool isCaseSensitive, const QString &soundUrl,
                                 QColor color, QColor matchColor,
                                 HighlightMatchStyle matchStyle,
                                 QString matchPaintID,
                                 HighlightChannelScope channelScope)
    : pattern_(pattern)
    , showInMentions_(showInMentions)
    , hasAlert_(hasAlert)
    , hasSound_(hasSound)
    , isRegex_(isRegex)
    , isCaseSensitive_(isCaseSensitive)
    , soundUrl_(soundUrl)
    , matchStyle_(matchStyle)
    , matchPaintID_(std::move(matchPaintID))
    , channelScope_(std::move(channelScope))
    , regex_(isRegex_
                 ? pattern
                 : REGEX_START_BOUNDARY % QRegularExpression::escape(pattern) %
                       REGEX_END_BOUNDARY,
             QRegularExpression::UseUnicodePropertiesOption |
                 (isCaseSensitive_ ? QRegularExpression::NoPatternOption
                                   : QRegularExpression::CaseInsensitiveOption))
{
    this->color_ = std::make_shared<QColor>(color);
    this->matchColor_ = std::make_shared<QColor>(
        matchColor.isValid() ? matchColor : defaultNewHighlightMatchColor());
}

HighlightPhrase::HighlightPhrase(const QString &pattern, bool showInMentions,
                                 bool hasAlert, bool hasSound, bool isRegex,
                                 bool isCaseSensitive, const QString &soundUrl,
                                 std::shared_ptr<QColor> color,
                                 std::shared_ptr<QColor> matchColor,
                                 HighlightMatchStyle matchStyle,
                                 QString matchPaintID,
                                 HighlightChannelScope channelScope)
    : pattern_(pattern)
    , showInMentions_(showInMentions)
    , hasAlert_(hasAlert)
    , hasSound_(hasSound)
    , isRegex_(isRegex)
    , isCaseSensitive_(isCaseSensitive)
    , soundUrl_(soundUrl)
    , color_(std::move(color))
    , matchColor_(std::move(matchColor))
    , matchStyle_(matchStyle)
    , matchPaintID_(std::move(matchPaintID))
    , channelScope_(std::move(channelScope))
    , regex_(isRegex_
                 ? pattern
                 : REGEX_START_BOUNDARY % QRegularExpression::escape(pattern) %
                       REGEX_END_BOUNDARY,
             QRegularExpression::UseUnicodePropertiesOption |
                 (isCaseSensitive_ ? QRegularExpression::NoPatternOption
                                   : QRegularExpression::CaseInsensitiveOption))
{
    if (!this->color_)
    {
        this->color_ =
            std::make_shared<QColor>(FALLBACK_HIGHLIGHT_COLOR);
    }
    if (!this->matchColor_)
    {
        this->matchColor_ =
            std::make_shared<QColor>(defaultNewHighlightMatchColor());
    }
}

const QString &HighlightPhrase::getPattern() const
{
    return this->pattern_;
}

bool HighlightPhrase::showInMentions() const
{
    return this->showInMentions_;
}

bool HighlightPhrase::hasAlert() const
{
    return this->hasAlert_;
}

bool HighlightPhrase::hasSound() const
{
    return this->hasSound_;
}

bool HighlightPhrase::hasCustomSound() const
{
    return !this->soundUrl_.isEmpty();
}

bool HighlightPhrase::isRegex() const
{
    return this->isRegex_;
}

bool HighlightPhrase::isValid() const
{
    return !this->pattern_.isEmpty() && this->regex_.isValid();
}

bool HighlightPhrase::isMatch(const QString &subject) const
{
    return this->isValid() && this->regex_.match(subject).hasMatch();
}

bool HighlightPhrase::isCaseSensitive() const
{
    return this->isCaseSensitive_;
}

const QUrl &HighlightPhrase::getSoundUrl() const
{
    return this->soundUrl_;
}

const std::shared_ptr<QColor> HighlightPhrase::getColor() const
{
    return this->color_;
}

std::vector<QRegularExpressionMatch> HighlightPhrase::findMatches(
    const QString &subject) const
{
    std::vector<QRegularExpressionMatch> matches;
    if (!this->isValid())
    {
        return matches;
    }

    auto it = this->regex_.globalMatch(subject);
    while (it.hasNext())
    {
        auto match = it.next();
        if (match.capturedLength() > 0)
        {
            matches.emplace_back(std::move(match));
            if (matches.size() >= MAX_RETAINED_MATCHES)
            {
                break;
            }
        }
    }
    return matches;
}

const std::shared_ptr<QColor> HighlightPhrase::getMatchColor() const
{
    return this->matchColor_;
}

HighlightMatchStyle HighlightPhrase::getMatchStyle() const
{
    return this->matchStyle_;
}

const QString &HighlightPhrase::getMatchPaintID() const
{
    return this->matchPaintID_;
}

const HighlightChannelScope &HighlightPhrase::getChannelScope() const
{
    return this->channelScope_;
}

}
