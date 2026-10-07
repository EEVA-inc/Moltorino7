// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/highlights/HighlightChannelScope.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/serialize.hpp>
#include <QColor>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <memory>
#include <utility>
#include <vector>

namespace chatterino {

class HighlightPhrase
{
public:
    bool operator==(const HighlightPhrase &other) const;

    HighlightPhrase(const QString &pattern, bool showInMentions, bool hasAlert,
                    bool hasSound, bool isRegex, bool isCaseSensitive,
                    const QString &soundUrl, QColor color,
                    QColor matchColor = {},
                    HighlightMatchStyle matchStyle =
                        HighlightMatchStyle::Outline,
                    QString matchPaintID = {},
                    HighlightChannelScope channelScope = {});

    HighlightPhrase(const QString &pattern, bool showInMentions, bool hasAlert,
                    bool hasSound, bool isRegex, bool isCaseSensitive,
                    const QString &soundUrl, std::shared_ptr<QColor> color,
                    std::shared_ptr<QColor> matchColor = nullptr,
                    HighlightMatchStyle matchStyle =
                        HighlightMatchStyle::Outline,
                    QString matchPaintID = {},
                    HighlightChannelScope channelScope = {});

    const QString &getPattern() const;
    bool showInMentions() const;
    bool hasAlert() const;

    bool hasSound() const;

    bool hasCustomSound() const;

    bool isRegex() const;
    bool isValid() const;
    bool isMatch(const QString &subject) const;
    std::vector<QRegularExpressionMatch> findMatches(
        const QString &subject) const;
    bool isCaseSensitive() const;
    const QUrl &getSoundUrl() const;
    const std::shared_ptr<QColor> getColor() const;
    const std::shared_ptr<QColor> getMatchColor() const;
    HighlightMatchStyle getMatchStyle() const;
    const QString &getMatchPaintID() const;
    const HighlightChannelScope &getChannelScope() const;

    static QColor FALLBACK_HIGHLIGHT_COLOR;

    static QColor FALLBACK_SELF_MESSAGE_HIGHLIGHT_COLOR;
    static QColor FALLBACK_REDEEMED_HIGHLIGHT_COLOR;
    static QColor FALLBACK_SUB_COLOR;
    static QColor FALLBACK_WATCH_STREAK_COLOR;
    static QColor FALLBACK_FIRST_MESSAGE_HIGHLIGHT_COLOR;
    static QColor FALLBACK_ELEVATED_MESSAGE_HIGHLIGHT_COLOR;
    static QColor FALLBACK_THREAD_HIGHLIGHT_COLOR;
    static QColor FALLBACK_AUTOMOD_HIGHLIGHT_COLOR;
    static QColor FALLBACK_ANNOUNCEMENT_HIGHLIGHT_COLOR;
    static QColor ANNOUNCEMENT_BLUE_HIGHLIGHT_COLOR;
    static QColor ANNOUNCEMENT_GREEN_HIGHLIGHT_COLOR;
    static QColor ANNOUNCEMENT_ORANGE_HIGHLIGHT_COLOR;
    static QColor ANNOUNCEMENT_PURPLE_HIGHLIGHT_COLOR;

private:
    QString pattern_;
    bool showInMentions_;
    bool hasAlert_;
    bool hasSound_;
    bool isRegex_;
    bool isCaseSensitive_;
    QUrl soundUrl_;
    std::shared_ptr<QColor> color_;
    std::shared_ptr<QColor> matchColor_;
    HighlightMatchStyle matchStyle_{HighlightMatchStyle::Outline};
    QString matchPaintID_;
    HighlightChannelScope channelScope_;
    QRegularExpression regex_;
};

}

namespace pajlada {

namespace {
chatterino::HighlightPhrase constructError()
{
    return chatterino::HighlightPhrase(QString(), false, false, false, false,
                                       false, QString(), QColor());
}
}

template <>
struct Serialize<chatterino::HighlightPhrase> {
    static rapidjson::Value get(const chatterino::HighlightPhrase &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value ret(rapidjson::kObjectType);

        chatterino::rj::set(ret, "pattern", value.getPattern(), a);
        chatterino::rj::set(ret, "showInMentions", value.showInMentions(), a);
        chatterino::rj::set(ret, "alert", value.hasAlert(), a);
        chatterino::rj::set(ret, "sound", value.hasSound(), a);
        chatterino::rj::set(ret, "regex", value.isRegex(), a);
        chatterino::rj::set(ret, "case", value.isCaseSensitive(), a);
        chatterino::rj::set(ret, "soundUrl", value.getSoundUrl().toString(), a);
        chatterino::rj::set(ret, "color",
                            value.getColor()->name(QColor::HexArgb), a);
        chatterino::rj::set(ret, "matchColor",
                            value.getMatchColor()->name(QColor::HexArgb), a);
        chatterino::rj::set(ret, "matchStyle",
                            chatterino::highlightMatchStyleName(
                                value.getMatchStyle()),
                            a);
        chatterino::rj::set(ret, "matchPaintId", value.getMatchPaintID(), a);
        chatterino::rj::set(
            ret, "channelScope",
            chatterino::highlightChannelScopeModeKey(
                value.getChannelScope().mode()),
            a);
        std::vector<QString> channelTargets;
        channelTargets.reserve(value.getChannelScope().targets().size());
        for (const auto &target : value.getChannelScope().targets())
        {
            channelTargets.emplace_back(
                chatterino::encodeHighlightChannelTarget(target));
        }
        chatterino::rj::set(ret, "channelTargets", channelTargets, a);

        return ret;
    }
};

template <>
struct Deserialize<chatterino::HighlightPhrase> {
    static chatterino::HighlightPhrase get(const rapidjson::Value &value,
                                           bool *error = nullptr)
    {
        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error)

            return constructError();
        }

        QString _pattern;
        bool _showInMentions = true;
        bool _hasAlert = true;
        bool _hasSound = false;
        bool _isRegex = false;
        bool _isCaseSensitive = false;
        QString _soundUrl;
        QString encodedColor;
        QString encodedMatchColor;
        QString matchStyle;
        QString matchPaintID;
        QString channelScope;
        std::vector<QString> encodedChannelTargets;

        chatterino::rj::getSafe(value, "pattern", _pattern);
        chatterino::rj::getSafe(value, "showInMentions", _showInMentions);
        chatterino::rj::getSafe(value, "alert", _hasAlert);
        chatterino::rj::getSafe(value, "sound", _hasSound);
        chatterino::rj::getSafe(value, "regex", _isRegex);
        chatterino::rj::getSafe(value, "case", _isCaseSensitive);
        chatterino::rj::getSafe(value, "soundUrl", _soundUrl);
        chatterino::rj::getSafe(value, "color", encodedColor);
        chatterino::rj::getSafe(value, "matchColor", encodedMatchColor);
        chatterino::rj::getSafe(value, "matchStyle", matchStyle);
        chatterino::rj::getSafe(value, "matchPaintId", matchPaintID);
        const bool hasChannelScope = chatterino::rj::getSafe(
            value, "channelScope", channelScope);
        chatterino::rj::getSafe(value, "channelTargets",
                                encodedChannelTargets);

        auto scopeMode = chatterino::highlightChannelScopeModeFromKey(
            channelScope);
        std::vector<chatterino::HighlightChannelTarget> channelTargets;
        for (const auto &encoded : encodedChannelTargets)
        {
            if (auto target =
                    chatterino::decodeHighlightChannelTarget(encoded))
            {
                channelTargets.emplace_back(std::move(*target));
            }
        }

        if (!hasChannelScope)
        {
            bool legacyGlobal = true;
            std::vector<QString> legacyIncluded;
            std::vector<QString> legacyExcluded;
            const bool hasLegacyGlobal = chatterino::rj::getSafe(
                value, "global", legacyGlobal);
            const bool hasLegacyIncluded = chatterino::rj::getSafe(
                value, "channels", legacyIncluded);
            const bool hasLegacyExcluded = chatterino::rj::getSafe(
                value, "ExcludedChannels", legacyExcluded);
            if (hasLegacyGlobal || hasLegacyIncluded || hasLegacyExcluded)
            {
                if (!legacyGlobal)
                {
                    scopeMode =
                        chatterino::HighlightChannelScopeMode::OnlySelected;
                    QSet<QString> excluded;
                    for (const auto &channel : legacyExcluded)
                    {
                        excluded.insert(
                            chatterino::normalizeHighlightChannelName(channel));
                    }
                    for (const auto &channel : legacyIncluded)
                    {
                        auto target = chatterino::makeHighlightChannelTarget(
                            chatterino::HighlightChannelTargetPlatform::Any,
                            channel);
                        if (target &&
                            !excluded.contains(
                                chatterino::normalizeHighlightChannelName(
                                    target->channel)))
                        {
                            channelTargets.emplace_back(std::move(*target));
                        }
                    }
                }
                else if (!legacyExcluded.empty())
                {
                    scopeMode =
                        chatterino::HighlightChannelScopeMode::ExcludeSelected;
                    for (const auto &channel : legacyExcluded)
                    {
                        if (auto target = chatterino::makeHighlightChannelTarget(
                                chatterino::HighlightChannelTargetPlatform::Any,
                                channel))
                        {
                            channelTargets.emplace_back(std::move(*target));
                        }
                    }
                }
            }
        }

        auto _color = QColor(encodedColor);
        if (!_color.isValid())
        {
            _color = chatterino::HighlightPhrase::FALLBACK_HIGHLIGHT_COLOR;
        }

        auto _matchColor = QColor(encodedMatchColor);
        if (!_matchColor.isValid())
        {
            _matchColor = chatterino::defaultHighlightMatchColor(_color);
        }

        return chatterino::HighlightPhrase(
            _pattern, _showInMentions, _hasAlert, _hasSound, _isRegex,
            _isCaseSensitive, _soundUrl, _color, _matchColor,
            chatterino::highlightMatchStyleFromName(matchStyle), matchPaintID,
            chatterino::HighlightChannelScope{
                scopeMode, std::move(channelTargets)});
    }
};

}
