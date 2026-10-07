#include "controllers/highlights/HighlightWordList.hpp"

#include "controllers/highlights/HighlightChannelScope.hpp"
#include "messages/Message.hpp"

#include <QHash>
#include <QQueue>
#include <QSet>
#include <QStringBuilder>

#include <algorithm>

namespace chatterino {
namespace {

constexpr size_t MAX_MATCHES_PER_LIST = 128;
constexpr qsizetype MAX_REGEX_BATCH_RULES = 64;
constexpr qsizetype MAX_REGEX_BATCH_CHARACTERS = 32768;

QString normalizeText(QStringView text, bool caseSensitive)
{
    if (caseSensitive)
    {
        return text.toString();
    }

    QString normalized;
    normalized.reserve(text.size());
    for (const auto ch : text)
    {
        normalized.append(ch.toCaseFolded());
    }
    return normalized;
}

bool isWordCharacter(QChar ch)
{
    return ch.isLetterOrNumber() || ch.isMark() || ch == u'_';
}

bool requiresIndividualRegex(const QString &pattern,
                             const QRegularExpression &compiled)
{
    return compiled.captureCount() != 0 ||
           pattern.contains(QStringLiteral("(*")) ||
           pattern.contains(QStringLiteral("(?R")) ||
           pattern.contains(QStringLiteral("(?0"));
}

}

QString highlightWordListPlatformKey(HighlightWordListPlatform platform)
{
    switch (platform)
    {
        case HighlightWordListPlatform::Twitch:
            return QStringLiteral("twitch");
        case HighlightWordListPlatform::YouTube:
            return QStringLiteral("youtube");
        case HighlightWordListPlatform::Kick:
            return QStringLiteral("kick");
        case HighlightWordListPlatform::TikTok:
            return QStringLiteral("tiktok");
        case HighlightWordListPlatform::All:
        default:
            return QStringLiteral("all");
    }
}

QString highlightWordListPlatformName(HighlightWordListPlatform platform)
{
    switch (platform)
    {
        case HighlightWordListPlatform::Twitch:
            return QStringLiteral("Twitch");
        case HighlightWordListPlatform::YouTube:
            return QStringLiteral("YouTube");
        case HighlightWordListPlatform::Kick:
            return QStringLiteral("Kick");
        case HighlightWordListPlatform::TikTok:
            return QStringLiteral("TikTok");
        case HighlightWordListPlatform::All:
        default:
            return QStringLiteral("All platforms");
    }
}

HighlightWordListPlatform highlightWordListPlatformFromKey(const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("twitch"))
    {
        return HighlightWordListPlatform::Twitch;
    }
    if (normalized == QStringLiteral("youtube"))
    {
        return HighlightWordListPlatform::YouTube;
    }
    if (normalized == QStringLiteral("kick"))
    {
        return HighlightWordListPlatform::Kick;
    }
    if (normalized == QStringLiteral("tiktok"))
    {
        return HighlightWordListPlatform::TikTok;
    }
    return HighlightWordListPlatform::All;
}

struct HighlightWordList::CompiledData {
    struct Node {
        QHash<QChar, int> next;
        int failure{};
        std::vector<int> outputs;
    };

    struct RegexBatch {
        QRegularExpression gate;
        std::vector<size_t> termIndices;
    };

    std::vector<Node> nodes{1};
    std::vector<QString> originalTerms;
    std::vector<QString> normalizedTerms;
    std::vector<QRegularExpression> regexes;
    std::vector<RegexBatch> regexBatches;
    std::vector<size_t> individualRegexIndices;
};

HighlightWordList::HighlightWordList(QString name, bool enabled,
                                     std::vector<QString> terms,
                                     std::vector<QString> channels,
                                     bool caseSensitive, bool showInMentions,
                                     bool hasAlert, bool hasSound,
                                     QString soundUrl, QColor color,
                                     bool isRegex, QColor matchColor,
                                     HighlightMatchStyle matchStyle,
                                     QString matchPaintID,
                                     HighlightWordListPlatform platform)
    : name_(std::move(name))
    , enabled_(enabled)
    , terms_(std::move(terms))
    , channels_(std::move(channels))
    , caseSensitive_(caseSensitive)
    , showInMentions_(showInMentions)
    , hasAlert_(hasAlert)
    , hasSound_(hasSound)
    , soundUrl_(std::move(soundUrl))
    , color_(std::make_shared<QColor>(color))
    , isRegex_(isRegex)
    , matchColor_(std::make_shared<QColor>(
          matchColor.isValid() ? matchColor
                               : defaultNewHighlightMatchColor()))
    , matchStyle_(matchStyle)
    , matchPaintID_(std::move(matchPaintID))
    , platform_(platform)
    , compiled_(compile(this->terms_, this->caseSensitive_, this->isRegex_))
{
    for (const auto &channel : this->channels_)
    {
        const auto normalized = normalizeHighlightChannelName(channel);
        if (!normalized.isEmpty())
        {
            this->normalizedChannels_.insert(normalized);
        }
    }
}

bool HighlightWordList::operator==(const HighlightWordList &other) const
{
    return std::tie(this->name_, this->enabled_, this->terms_, this->channels_,
                    this->caseSensitive_, this->showInMentions_,
                    this->hasAlert_, this->hasSound_, this->soundUrl_,
                    *this->color_, this->isRegex_, *this->matchColor_,
                    this->matchStyle_, this->matchPaintID_, this->platform_) ==
           std::tie(other.name_, other.enabled_, other.terms_, other.channels_,
                     other.caseSensitive_, other.showInMentions_,
                     other.hasAlert_, other.hasSound_, other.soundUrl_,
                     *other.color_, other.isRegex_, *other.matchColor_,
                     other.matchStyle_, other.matchPaintID_, other.platform_);
}

const QString &HighlightWordList::name() const
{
    return this->name_;
}

bool HighlightWordList::enabled() const
{
    return this->enabled_;
}

const std::vector<QString> &HighlightWordList::terms() const
{
    return this->terms_;
}

const std::vector<QString> &HighlightWordList::channels() const
{
    return this->channels_;
}

bool HighlightWordList::caseSensitive() const
{
    return this->caseSensitive_;
}

bool HighlightWordList::showInMentions() const
{
    return this->showInMentions_;
}

bool HighlightWordList::hasAlert() const
{
    return this->hasAlert_;
}

bool HighlightWordList::hasSound() const
{
    return this->hasSound_;
}

bool HighlightWordList::hasCustomSound() const
{
    return !this->soundUrl_.isEmpty();
}

const QUrl &HighlightWordList::soundUrl() const
{
    return this->soundUrl_;
}

const std::shared_ptr<QColor> &HighlightWordList::color() const
{
    return this->color_;
}

bool HighlightWordList::isRegex() const
{
    return this->isRegex_;
}

const std::shared_ptr<QColor> &HighlightWordList::matchColor() const
{
    return this->matchColor_;
}

HighlightMatchStyle HighlightWordList::matchStyle() const
{
    return this->matchStyle_;
}

const QString &HighlightWordList::matchPaintID() const
{
    return this->matchPaintID_;
}

HighlightWordListPlatform HighlightWordList::platform() const
{
    return this->platform_;
}

void HighlightWordList::setEnabled(bool enabled)
{
    this->enabled_ = enabled;
}

std::shared_ptr<const HighlightWordList::CompiledData>
    HighlightWordList::compile(const std::vector<QString> &terms,
                               bool caseSensitive, bool isRegex)
{
    auto data = std::make_shared<CompiledData>();
    QSet<QString> seen;

    const auto regexOptions =
        QRegularExpression::UseUnicodePropertiesOption |
        (caseSensitive ? QRegularExpression::NoPatternOption
                       : QRegularExpression::CaseInsensitiveOption);
    QStringList batchPatterns;
    std::vector<size_t> batchIndices;
    qsizetype batchCharacters = 0;
    size_t literalCharacters = 0;
    size_t regexCharacters = 0;

    const auto flushRegexBatch = [&] {
        if (batchPatterns.isEmpty())
        {
            return;
        }

        auto gate = QRegularExpression(batchPatterns.join(u'|'), regexOptions);
        if (gate.isValid())
        {
            data->regexBatches.emplace_back(CompiledData::RegexBatch{
                std::move(gate), std::move(batchIndices)});
        }
        else
        {
            data->individualRegexIndices.insert(
                data->individualRegexIndices.end(), batchIndices.begin(),
                batchIndices.end());
        }
        batchPatterns.clear();
        batchIndices.clear();
        batchCharacters = 0;
    };

    for (auto term : terms)
    {
        term = term.trimmed();
        if (term.isEmpty())
        {
            continue;
        }
        if ((isRegex &&
             (data->originalTerms.size() >=
                  HIGHLIGHT_WORD_LIST_MAX_REGEX_RULES ||
              static_cast<size_t>(term.size()) >
                  HIGHLIGHT_WORD_LIST_MAX_REGEX_RULE_LENGTH ||
              regexCharacters + static_cast<size_t>(term.size()) >
                  HIGHLIGHT_WORD_LIST_MAX_RULE_CHARACTERS)) ||
            (!isRegex &&
             (data->originalTerms.size() >=
                  HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULES ||
              static_cast<size_t>(term.size()) >
                  HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULE_LENGTH)))
        {
            continue;
        }
        const auto key =
            isRegex ? term : normalizeText(term, caseSensitive);
        if (seen.contains(key))
        {
            continue;
        }
        seen.insert(key);

        if (isRegex)
        {
            QRegularExpression regex(term, regexOptions);
            if (!regex.isValid())
            {
                continue;
            }
            regexCharacters += static_cast<size_t>(term.size());
            const auto termIndex = data->originalTerms.size();
            data->originalTerms.emplace_back(term);
            data->regexes.emplace_back(std::move(regex));

            if (requiresIndividualRegex(term, data->regexes.back()))
            {
                data->individualRegexIndices.emplace_back(termIndex);
                continue;
            }

            const auto wrapped = u"(?:" % term % u')';
            if (!batchPatterns.isEmpty() &&
                (batchPatterns.size() >= MAX_REGEX_BATCH_RULES ||
                 batchCharacters + wrapped.size() >
                     MAX_REGEX_BATCH_CHARACTERS))
            {
                flushRegexBatch();
            }
            batchCharacters += wrapped.size();
            batchPatterns.append(wrapped);
            batchIndices.emplace_back(termIndex);
            continue;
        }

        const auto &normalized = key;
        if (literalCharacters + static_cast<size_t>(normalized.size()) >
            HIGHLIGHT_WORD_LIST_MAX_RULE_CHARACTERS)
        {
            continue;
        }
        literalCharacters += static_cast<size_t>(normalized.size());

        const auto termIndex = static_cast<int>(data->originalTerms.size());
        data->originalTerms.emplace_back(std::move(term));
        data->normalizedTerms.emplace_back(normalized);
        int node = 0;
        for (const auto ch : normalized)
        {
            auto it = data->nodes[node].next.find(ch);
            if (it == data->nodes[node].next.end())
            {
                const auto next = static_cast<int>(data->nodes.size());
                data->nodes[node].next.insert(ch, next);
                data->nodes.emplace_back();
                node = next;
            }
            else
            {
                node = it.value();
            }
        }
        data->nodes[node].outputs.emplace_back(termIndex);
    }

    if (isRegex)
    {
        flushRegexBatch();
        return data;
    }

    QQueue<int> queue;
    for (const auto child : data->nodes[0].next)
    {
        queue.enqueue(child);
    }
    while (!queue.isEmpty())
    {
        const auto node = queue.dequeue();
        for (auto it = data->nodes[node].next.cbegin();
             it != data->nodes[node].next.cend(); ++it)
        {
            const auto ch = it.key();
            const auto child = it.value();
            auto failure = data->nodes[node].failure;
            while (failure != 0 && !data->nodes[failure].next.contains(ch))
            {
                failure = data->nodes[failure].failure;
            }
            if (const auto found = data->nodes[failure].next.find(ch);
                found != data->nodes[failure].next.end() &&
                found.value() != child)
            {
                failure = found.value();
            }
            data->nodes[child].failure = failure;
            const auto &inherited = data->nodes[failure].outputs;
            data->nodes[child].outputs.insert(data->nodes[child].outputs.end(),
                                              inherited.begin(),
                                              inherited.end());
            queue.enqueue(child);
        }
    }
    return data;
}

bool HighlightWordList::appliesToChannel(const QString &channelName) const
{
    return this->appliesToNormalizedChannel(
        normalizeHighlightChannelName(channelName));
}

bool HighlightWordList::appliesToNormalizedChannel(
    const QString &normalizedChannelName) const
{
    if (this->normalizedChannels_.empty())
    {
        return true;
    }
    return this->normalizedChannels_.contains(normalizedChannelName);
}

bool HighlightWordList::appliesToPlatform(MessagePlatform platform) const
{
    switch (this->platform_)
    {
        case HighlightWordListPlatform::All:
            return true;
        case HighlightWordListPlatform::Twitch:
            return platform == MessagePlatform::AnyOrTwitch;
        case HighlightWordListPlatform::YouTube:
            return platform == MessagePlatform::YouTube;
        case HighlightWordListPlatform::Kick:
            return platform == MessagePlatform::Kick;
        case HighlightWordListPlatform::TikTok:
            return platform == MessagePlatform::TikTok;
    }
    return true;
}

std::vector<HighlightMatch> HighlightWordList::findMatches(
    const QString &subject, const QString &channelName,
    MessagePlatform platform) const
{
    return this->match(subject, channelName, platform).matches;
}

HighlightWordListMatchResult HighlightWordList::match(
    const QString &subject, const QString &channelName,
    MessagePlatform platform, HighlightWordListMatchMode mode) const
{
    return this->matchForNormalizedChannel(
        subject, normalizeHighlightChannelName(channelName), platform, mode);
}

HighlightWordListMatchResult HighlightWordList::matchForNormalizedChannel(
    const QString &subject, const QString &normalizedChannelName,
    MessagePlatform platform, HighlightWordListMatchMode mode) const
{
    if (!this->enabled_ || !this->appliesToPlatform(platform) ||
        !this->appliesToNormalizedChannel(normalizedChannelName) ||
        !this->compiled_)
    {
        return {};
    }

    return this->matchSubject(subject, mode);
}

HighlightWordListMatchResult HighlightWordList::matchSubject(
    const QString &subject, HighlightWordListMatchMode mode) const
{
    HighlightWordListMatchResult result;
    if (!this->compiled_ || this->compiled_->originalTerms.empty())
    {
        return result;
    }

    if (this->isRegex_)
    {
        const bool collectRanges =
            mode == HighlightWordListMatchMode::WithRanges;
        const auto collectRegex = [&](size_t termIndex) {
            if (!collectRanges)
            {
                if (this->compiled_->regexes[termIndex]
                        .match(subject)
                        .hasMatch())
                {
                    result.matched = true;
                }
                return;
            }

            auto it =
                this->compiled_->regexes[termIndex].globalMatch(subject);
            while (it.hasNext())
            {
                const auto match = it.next();
                result.matched = true;
                if (match.capturedLength() <= 0)
                {
                    continue;
                }
                result.matches.emplace_back(HighlightMatch{
                    .start = match.capturedStart(),
                    .length = match.capturedLength(),
                    .color = *this->matchColor_,
                    .ruleName = this->name_,
                    .pattern = this->compiled_->originalTerms[termIndex],
                    .source = HighlightMatchSource::WordList,
                    .style = this->matchStyle_,
                    .paintID = this->matchPaintID_,
                });
                if (result.matches.size() >= MAX_MATCHES_PER_LIST)
                {
                    break;
                }
            }
        };

        if (!collectRanges)
        {
            for (const auto &batch : this->compiled_->regexBatches)
            {
                if (batch.gate.match(subject).hasMatch())
                {
                    result.matched = true;
                    return result;
                }
            }
            for (const auto termIndex :
                 this->compiled_->individualRegexIndices)
            {
                collectRegex(termIndex);
                if (result.matched)
                {
                    return result;
                }
            }
            return result;
        }

        auto candidateRules = this->compiled_->individualRegexIndices;
        for (const auto &batch : this->compiled_->regexBatches)
        {
            if (!batch.gate.match(subject).hasMatch())
            {
                continue;
            }
            candidateRules.insert(candidateRules.end(),
                                  batch.termIndices.begin(),
                                  batch.termIndices.end());
        }
        std::ranges::sort(candidateRules);
        for (const auto termIndex : candidateRules)
        {
            collectRegex(termIndex);
            if (result.matches.size() >= MAX_MATCHES_PER_LIST)
            {
                break;
            }
        }
        std::ranges::sort(result.matches, [](const auto &a, const auto &b) {
            if (a.start != b.start)
            {
                return a.start < b.start;
            }
            return a.length > b.length;
        });
        return result;
    }

    const auto normalized = normalizeText(subject, this->caseSensitive_);
    int node = 0;
    for (qsizetype index = 0; index < normalized.size(); ++index)
    {
        const auto ch = normalized.at(index);
        while (node != 0 && !this->compiled_->nodes[node].next.contains(ch))
        {
            node = this->compiled_->nodes[node].failure;
        }
        if (const auto found = this->compiled_->nodes[node].next.find(ch);
            found != this->compiled_->nodes[node].next.end())
        {
            node = found.value();
        }

        for (const auto termIndex : this->compiled_->nodes[node].outputs)
        {
            const auto length =
                this->compiled_->normalizedTerms[termIndex].size();
            const auto start = index - length + 1;
            const auto end = start + length;
            if ((start > 0 && isWordCharacter(subject.at(start - 1))) ||
                (end < subject.size() && isWordCharacter(subject.at(end))))
            {
                continue;
            }
            result.matched = true;
            if (mode == HighlightWordListMatchMode::MatchOnly)
            {
                return result;
            }
            result.matches.emplace_back(HighlightMatch{
                .start = start,
                .length = length,
                .color = *this->matchColor_,
                .ruleName = this->name_,
                .pattern = this->compiled_->originalTerms[termIndex],
                .source = HighlightMatchSource::WordList,
                .style = this->matchStyle_,
                .paintID = this->matchPaintID_,
            });
            if (result.matches.size() >= MAX_MATCHES_PER_LIST)
            {
                break;
            }
        }
        if (result.matches.size() >= MAX_MATCHES_PER_LIST)
        {
            break;
        }
    }

    std::ranges::sort(result.matches, [](const auto &a, const auto &b) {
        if (a.start != b.start)
        {
            return a.start < b.start;
        }
        return a.length > b.length;
    });
    return result;
}

}
