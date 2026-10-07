#pragma once

#include "controllers/highlights/HighlightResult.hpp"
#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/serialize.hpp>
#include <QColor>
#include <QSet>
#include <QString>
#include <QUrl>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace chatterino {

enum class MessagePlatform : std::uint8_t;

inline constexpr std::size_t HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULES = 10000;
inline constexpr std::size_t HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULE_LENGTH = 256;
inline constexpr std::size_t HIGHLIGHT_WORD_LIST_MAX_RULE_CHARACTERS = 500000;
inline constexpr std::size_t HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES = 250;
inline constexpr std::size_t HIGHLIGHT_WORD_LIST_MAX_REGEX_RULES = 2000;
inline constexpr std::size_t HIGHLIGHT_WORD_LIST_MAX_REGEX_RULE_LENGTH = 512;

enum class HighlightWordListMatchMode : std::uint8_t {
    MatchOnly,
    WithRanges,
};

enum class HighlightWordListPlatform : std::uint8_t {
    All,
    Twitch,
    YouTube,
    Kick,
    TikTok,
};

QString highlightWordListPlatformKey(HighlightWordListPlatform platform);
QString highlightWordListPlatformName(HighlightWordListPlatform platform);
HighlightWordListPlatform highlightWordListPlatformFromKey(
    const QString &key);

struct HighlightWordListMatchResult {
    bool matched{};
    std::vector<HighlightMatch> matches;
};

class HighlightWordList
{
public:
    HighlightWordList(QString name, bool enabled, std::vector<QString> terms,
                      std::vector<QString> channels, bool caseSensitive,
                      bool showInMentions, bool hasAlert, bool hasSound,
                      QString soundUrl, QColor color, bool isRegex = false,
                      QColor matchColor = {},
                      HighlightMatchStyle matchStyle =
                          HighlightMatchStyle::Outline,
                      QString matchPaintID = {},
                      HighlightWordListPlatform platform =
                          HighlightWordListPlatform::All);

    bool operator==(const HighlightWordList &other) const;

    const QString &name() const;
    bool enabled() const;
    const std::vector<QString> &terms() const;
    const std::vector<QString> &channels() const;
    bool caseSensitive() const;
    bool showInMentions() const;
    bool hasAlert() const;
    bool hasSound() const;
    bool hasCustomSound() const;
    const QUrl &soundUrl() const;
    const std::shared_ptr<QColor> &color() const;
    bool isRegex() const;
    const std::shared_ptr<QColor> &matchColor() const;
    HighlightMatchStyle matchStyle() const;
    const QString &matchPaintID() const;
    HighlightWordListPlatform platform() const;
    void setEnabled(bool enabled);

    bool appliesToChannel(const QString &channelName) const;
    bool appliesToPlatform(MessagePlatform platform) const;
    HighlightWordListMatchResult match(
        const QString &subject, const QString &channelName,
        MessagePlatform platform,
        HighlightWordListMatchMode mode =
            HighlightWordListMatchMode::WithRanges) const;

    HighlightWordListMatchResult matchSubject(
        const QString &subject,
        HighlightWordListMatchMode mode =
            HighlightWordListMatchMode::WithRanges) const;
    HighlightWordListMatchResult matchForNormalizedChannel(
        const QString &subject, const QString &normalizedChannelName,
        MessagePlatform platform,
        HighlightWordListMatchMode mode =
            HighlightWordListMatchMode::WithRanges) const;
    std::vector<HighlightMatch> findMatches(const QString &subject,
                                            const QString &channelName,
                                            MessagePlatform platform) const;

private:
    struct CompiledData;
    static std::shared_ptr<const CompiledData> compile(
        const std::vector<QString> &terms, bool caseSensitive, bool isRegex);
    bool appliesToNormalizedChannel(
        const QString &normalizedChannelName) const;

    QString name_;
    bool enabled_{};
    std::vector<QString> terms_;
    std::vector<QString> channels_;
    QSet<QString> normalizedChannels_;
    bool caseSensitive_{};
    bool showInMentions_{};
    bool hasAlert_{};
    bool hasSound_{};
    QUrl soundUrl_;
    std::shared_ptr<QColor> color_;
    bool isRegex_{};
    std::shared_ptr<QColor> matchColor_;
    HighlightMatchStyle matchStyle_{HighlightMatchStyle::Outline};
    QString matchPaintID_;
    HighlightWordListPlatform platform_{HighlightWordListPlatform::All};
    std::shared_ptr<const CompiledData> compiled_;
};

}

namespace pajlada {

template <>
struct Serialize<chatterino::HighlightWordList> {
    static rapidjson::Value get(const chatterino::HighlightWordList &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value ret(rapidjson::kObjectType);
        chatterino::rj::set(ret, "name", value.name(), a);
        chatterino::rj::set(ret, "enabled", value.enabled(), a);
        chatterino::rj::set(ret, "terms", value.terms(), a);
        chatterino::rj::set(ret, "channels", value.channels(), a);
        chatterino::rj::set(ret, "case", value.caseSensitive(), a);
        chatterino::rj::set(ret, "showInMentions", value.showInMentions(), a);
        chatterino::rj::set(ret, "alert", value.hasAlert(), a);
        chatterino::rj::set(ret, "sound", value.hasSound(), a);
        chatterino::rj::set(ret, "soundUrl", value.soundUrl().toString(), a);
        chatterino::rj::set(ret, "color", value.color()->name(QColor::HexArgb),
                            a);
        chatterino::rj::set(ret, "regex", value.isRegex(), a);
        chatterino::rj::set(ret, "matchColor",
                            value.matchColor()->name(QColor::HexArgb), a);
        chatterino::rj::set(ret, "matchStyle",
                            chatterino::highlightMatchStyleName(
                                value.matchStyle()),
                            a);
        chatterino::rj::set(ret, "matchPaintId", value.matchPaintID(), a);
        chatterino::rj::set(
            ret, "platform",
            chatterino::highlightWordListPlatformKey(value.platform()), a);
        return ret;
    }
};

template <>
struct Deserialize<chatterino::HighlightWordList> {
    static chatterino::HighlightWordList get(const rapidjson::Value &value,
                                             bool *error = nullptr)
    {
        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error)
            return {QString(), false, {},    {},        false,
                    false,     false, false, QString(), QColor()};
        }

        QString name;
        bool enabled = true;
        std::vector<QString> terms;
        std::vector<QString> channels;
        bool caseSensitive = false;
        bool showInMentions = false;
        bool alert = false;
        bool sound = false;
        QString soundUrl;
        QString encodedColor;
        bool isRegex = false;
        QString encodedMatchColor;
        QString matchStyle;
        QString matchPaintID;
        QString platform;
        chatterino::rj::getSafe(value, "name", name);
        chatterino::rj::getSafe(value, "enabled", enabled);
        chatterino::rj::getSafe(value, "terms", terms);
        chatterino::rj::getSafe(value, "channels", channels);
        chatterino::rj::getSafe(value, "case", caseSensitive);
        chatterino::rj::getSafe(value, "showInMentions", showInMentions);
        chatterino::rj::getSafe(value, "alert", alert);
        chatterino::rj::getSafe(value, "sound", sound);
        chatterino::rj::getSafe(value, "soundUrl", soundUrl);
        chatterino::rj::getSafe(value, "color", encodedColor);
        chatterino::rj::getSafe(value, "regex", isRegex);
        chatterino::rj::getSafe(value, "matchColor", encodedMatchColor);
        chatterino::rj::getSafe(value, "matchStyle", matchStyle);
        chatterino::rj::getSafe(value, "matchPaintId", matchPaintID);
        chatterino::rj::getSafe(value, "platform", platform);

        auto color = QColor(encodedColor);
        if (!color.isValid())
        {
            color = QColor(127, 63, 73, 127);
        }
        auto matchColor = QColor(encodedMatchColor);
        if (!matchColor.isValid())
        {
            matchColor = chatterino::defaultHighlightMatchColor(color);
        }
        return {std::move(name),
                enabled,
                std::move(terms),
                std::move(channels),
                caseSensitive,
                showInMentions,
                alert,
                sound,
                std::move(soundUrl),
                color,
                isRegex,
                matchColor,
                chatterino::highlightMatchStyleFromName(matchStyle),
                matchPaintID,
                chatterino::highlightWordListPlatformFromKey(platform)};
    }
};

}
