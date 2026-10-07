#include "controllers/highlights/ChattyHighlightImport.hpp"

#include "controllers/highlights/HighlightChannelScope.hpp"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace chatterino {
namespace {

constexpr qsizetype MAX_ANALYZED_RULES = 10000;
constexpr qsizetype MAX_RETAINED_IMPORT_ISSUES = 200;

struct PrefixConversion {
    QString prefix;
    enum class Kind {
        Regex,
        RegexInsensitive,
        RegexWords,
        RegexWordsInsensitive,
        RegexMessage,
        RegexMessageInsensitive,
        LiteralWords,
        LiteralWordsSensitive,
        Literal,
        LiteralSensitive,
        LiteralStart,
        LiteralStartWord,
    } kind;
};

const std::vector<PrefixConversion> &patternPrefixes()
{
    static const std::vector<PrefixConversion> prefixes = [] {
        std::vector<PrefixConversion> result;
        const auto add = [&result](QString prefix,
                                   PrefixConversion::Kind kind) {
            result.push_back({prefix, kind});
            result.push_back({QStringLiteral("msg") + prefix, kind});
        };
        add(QStringLiteral("regmi:"),
            PrefixConversion::Kind::RegexMessageInsensitive);
        add(QStringLiteral("regwi:"),
            PrefixConversion::Kind::RegexWordsInsensitive);
        add(QStringLiteral("startw:"),
            PrefixConversion::Kind::LiteralStartWord);
        add(QStringLiteral("regm:"), PrefixConversion::Kind::RegexMessage);
        add(QStringLiteral("regw:"), PrefixConversion::Kind::RegexWords);
        add(QStringLiteral("regi:"), PrefixConversion::Kind::RegexInsensitive);
        add(QStringLiteral("wcs:"),
            PrefixConversion::Kind::LiteralWordsSensitive);
        add(QStringLiteral("start:"), PrefixConversion::Kind::LiteralStart);
        add(QStringLiteral("text:"), PrefixConversion::Kind::Literal);
        add(QStringLiteral("re*:"), PrefixConversion::Kind::Regex);
        add(QStringLiteral("reg:"), PrefixConversion::Kind::Regex);
        add(QStringLiteral("re:"), PrefixConversion::Kind::RegexMessage);
        add(QStringLiteral("cs:"), PrefixConversion::Kind::LiteralSensitive);
        add(QStringLiteral("w:"), PrefixConversion::Kind::LiteralWords);
        std::ranges::sort(result, [](const auto &a, const auto &b) {
            return a.prefix.size() > b.prefix.size();
        });
        return result;
    }();
    return prefixes;
}

std::optional<PrefixConversion> findPatternPrefix(const QString &text)
{
    for (const auto &candidate : patternPrefixes())
    {
        if (text.startsWith(candidate.prefix))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

struct FirstToken {
    QString value;
    QString remainder;
};

FirstToken takeFirstToken(const QString &input)
{
    QString token;
    bool quoted = false;
    bool escaped = false;
    qsizetype index = 0;
    for (; index < input.size(); ++index)
    {
        const auto ch = input.at(index);
        if (escaped)
        {
            token.append(ch);
            escaped = false;
            continue;
        }
        if (ch == u'\\')
        {
            escaped = true;
            continue;
        }
        if (ch == u'"')
        {
            quoted = !quoted;
            continue;
        }
        if (!quoted && ch.isSpace())
        {
            break;
        }
        token.append(ch);
    }
    while (index < input.size() && input.at(index).isSpace())
    {
        ++index;
    }
    return {std::move(token), input.mid(index)};
}

QString convertPattern(const PrefixConversion &conversion,
                       const QString &payload)
{
    using Kind = PrefixConversion::Kind;
    const auto literal = QRegularExpression::escape(payload);
    switch (conversion.kind)
    {
        case Kind::Regex:
            return payload;
        case Kind::RegexInsensitive:
            return QStringLiteral("(?i:%1)").arg(payload);
        case Kind::RegexWords:
            return QStringLiteral("\\b(?:%1)\\b").arg(payload);
        case Kind::RegexWordsInsensitive:
            return QStringLiteral("(?i:\\b(?:%1)\\b)").arg(payload);
        case Kind::RegexMessage:
            return QStringLiteral("^(?:%1)$").arg(payload);
        case Kind::RegexMessageInsensitive:
            return QStringLiteral("(?i:^(?:%1)$)").arg(payload);
        case Kind::LiteralWords:
            return QStringLiteral("(?i:\\b(?:%1)\\b)").arg(literal);
        case Kind::LiteralWordsSensitive:
            return QStringLiteral("\\b(?:%1)\\b").arg(literal);
        case Kind::LiteralSensitive:
            return literal;
        case Kind::LiteralStart:
            return QStringLiteral("(?i:^(?:%1))").arg(literal);
        case Kind::LiteralStartWord:
            return QStringLiteral("(?i:^(?:%1)\\b)").arg(literal);
        case Kind::Literal:
            return QStringLiteral("(?i:%1)").arg(literal);
    }
    return {};
}

QString javaRegexCompatibilityIssue(const QString &pattern)
{
    static const QRegularExpression inlineUnicodeFlag(
        QStringLiteral(R"(\(\?[a-zA-Z-]*[uU][a-zA-Z-]*(?:\)|:))"));
    static const QRegularExpression classIntersection(
        QStringLiteral(R"(\[[^\]]*&&)"));
    static const QRegularExpression javaProperty(
        QStringLiteral(R"(\\p\{(?:java|Is|In))"));
    static const QRegularExpression ambiguousBackreference(
        QStringLiteral(R"(\\[1-9][0-9])"));

    if (inlineUnicodeFlag.match(pattern).hasMatch())
    {
        return QStringLiteral(
            "Java's inline u or U regex flag has different meaning in Qt");
    }
    if (classIntersection.match(pattern).hasMatch())
    {
        return QStringLiteral(
            "Java character class intersections are not portable to Qt");
    }
    if (javaProperty.match(pattern).hasMatch())
    {
        return QStringLiteral(
            "this Java Unicode property name is not portable to Qt");
    }
    if (pattern.contains(QStringLiteral("\\b{g}")))
    {
        return QStringLiteral(
            "Java grapheme boundary syntax is not supported by Qt");
    }
    if (ambiguousBackreference.match(pattern).hasMatch())
    {
        return QStringLiteral(
            "two digit Java backreferences can change meaning in Qt");
    }
    return {};
}

bool isUnsupportedChattyPrefix(const QString &token)
{
    static const QSet<QString> prefixes = {
        QStringLiteral("cat"),         QStringLiteral("user"),
        QStringLiteral("reuser"),      QStringLiteral("status"),
        QStringLiteral("msgs"),        QStringLiteral("mreq"),
        QStringLiteral("mlimit"),      QStringLiteral("mtime"),
        QStringLiteral("mtype"),       QStringLiteral("mystatus"),
        QStringLiteral("chanCat"),     QStringLiteral("chanCat2"),
        QStringLiteral("color"),       QStringLiteral("bgcolor"),
        QStringLiteral("replacement"), QStringLiteral("if"),
        QStringLiteral("blacklist"),   QStringLiteral("preset"),
        QStringLiteral("to"),          QStringLiteral("n"),
        QStringLiteral("ncat"),        QStringLiteral("tag"),
        QStringLiteral("badge"),       QStringLiteral("cc"),
        QStringLiteral("cc2"),
    };
    auto name = token.section(u':', 0, 0);
    while (name.startsWith(u'!') || name.startsWith(u'+'))
    {
        name.remove(0, 1);
    }
    return prefixes.contains(name);
}

QString normalizedGroupKey(std::vector<QString> &channels)
{
    QSet<QString> seen;
    std::vector<QString> normalized;
    for (const auto &channel : channels)
    {
        const auto value = normalizeHighlightChannelName(channel);
        if (!value.isEmpty() && !seen.contains(value))
        {
            seen.insert(value);
            normalized.emplace_back(value);
        }
    }
    std::ranges::sort(normalized);
    channels = std::move(normalized);
    QStringList parts;
    for (const auto &channel : channels)
    {
        parts.append(channel);
    }
    return parts.join(QChar(0x1F));
}

void addIssue(ChattyHighlightImportResult &result, qsizetype line,
              QString reason, const QString &source, bool invalid)
{
    if (invalid)
    {
        ++result.invalidRules;
    }
    else
    {
        ++result.unsupportedRules;
    }
    if (static_cast<qsizetype>(result.issues.size()) <
        MAX_RETAINED_IMPORT_ISSUES)
    {
        result.issues.push_back({line, std::move(reason), source.left(240)});
    }
}

}

qsizetype ChattyHighlightImportResult::importedRuleCount() const
{
    qsizetype count = 0;
    for (const auto &group : this->groups)
    {
        count += static_cast<qsizetype>(group.regexes.size());
    }
    return count;
}

bool ChattyHighlightImportResult::hasSkippedRules() const
{
    return this->invalidRules > 0 || this->unsupportedRules > 0;
}

ChattyHighlightImportResult importChattyHighlightRules(const QString &text)
{
    ChattyHighlightImportResult result;
    if (text.size() > CHATTY_HIGHLIGHT_IMPORT_MAX_SOURCE_CHARACTERS)
    {
        addIssue(result, 0,
                 QStringLiteral("This text is too large to import."),
                 {}, false);
        return result;
    }
    QHash<QString, qsizetype> groupIndices;
    std::vector<QSet<QString>> groupRules;
    const auto options = QRegularExpression::UseUnicodePropertiesOption;

    qsizetype lineNumber = 0;
    qsizetype analyzedRules = 0;
    for (auto line : text.split(u'\n', Qt::KeepEmptyParts))
    {
        ++lineNumber;
        if (line.endsWith(u'\r'))
        {
            line.chop(1);
        }
        line = line.trimmed();
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark)
        {
            line.remove(0, 1);
        }
        if (line.isEmpty())
        {
            continue;
        }

        if (analyzedRules >= MAX_ANALYZED_RULES)
        {
            result.groups.clear();
            result.exactRules = 0;
            result.adjustedRules = 0;
            addIssue(result, lineNumber,
                     QStringLiteral("Import up to %1 rules at a time.")
                         .arg(MAX_ANALYZED_RULES),
                     {}, false);
            return result;
        }
        ++analyzedRules;

        const auto original = line;
        std::vector<QString> channels;
        bool adjusted = false;
        bool unsupported = false;
        QString unsupportedReason;
        std::optional<PrefixConversion> conversion;
        QString payload;

        while (!line.isEmpty())
        {
            if (const auto found = findPatternPrefix(line))
            {
                conversion = found;
                payload = line.mid(found->prefix.size());
                break;
            }
            if (line.startsWith(u'+') || line.startsWith(u'!'))
            {
                unsupported = true;
                unsupportedReason =
                    QStringLiteral("additional or negative match conditions");
                break;
            }

            const auto token = takeFirstToken(line);
            if (token.value.startsWith(QStringLiteral("config:")))
            {
                const auto values =
                    token.value.mid(7).split(u',', Qt::SkipEmptyParts);
                for (const auto &value : values)
                {
                    if (value == QStringLiteral("!s"))
                    {
                        continue;
                    }
                    if (value == QStringLiteral("any"))
                    {
                        adjusted = true;
                        continue;
                    }
                    unsupported = true;
                    unsupportedReason =
                        value == QStringLiteral("s")
                            ? QStringLiteral(
                                  "Chatty substitute processing is enabled")
                            : QStringLiteral("unsupported config option: %1")
                                  .arg(value);
                    break;
                }
                if (unsupported)
                {
                    break;
                }
                line = token.remainder;
                continue;
            }
            if (token.value.startsWith(QStringLiteral("chan:")))
            {
                if (!channels.empty())
                {
                    unsupported = true;
                    unsupportedReason = QStringLiteral(
                        "multiple channel conditions on one rule");
                    break;
                }
                for (auto channel :
                     token.value.mid(5).split(u',', Qt::SkipEmptyParts))
                {
                    channel = channel.trimmed();
                    if (!channel.isEmpty())
                    {
                        channels.emplace_back(std::move(channel));
                    }
                }
                if (channels.empty())
                {
                    unsupported = true;
                    unsupportedReason =
                        QStringLiteral("empty channel condition");
                    break;
                }
                line = token.remainder;
                continue;
            }
            if (token.value.contains(u':') &&
                isUnsupportedChattyPrefix(token.value))
            {
                unsupported = true;
                unsupportedReason =
                    QStringLiteral("unsupported Chatty prefix: %1")
                        .arg(token.value.section(u':', 0, 0));
                break;
            }

            conversion = PrefixConversion{{}, PrefixConversion::Kind::Literal};
            payload = line;
            break;
        }

        if (unsupported || !conversion || payload.isEmpty())
        {
            addIssue(result, lineNumber,
                     unsupportedReason.isEmpty()
                         ? QStringLiteral("missing message match")
                         : std::move(unsupportedReason),
                     original, false);
            continue;
        }

        auto regex = convertPattern(*conversion, payload);
        if (const auto issue = javaRegexCompatibilityIssue(regex);
            !issue.isEmpty())
        {
            addIssue(result, lineNumber, issue, original, false);
            continue;
        }
        const QRegularExpression compiled(regex, options);
        if (!compiled.isValid())
        {
            addIssue(result, lineNumber,
                     QStringLiteral("invalid converted regex: %1")
                         .arg(compiled.errorString()),
                     original, true);
            continue;
        }

        const auto key = normalizedGroupKey(channels);
        qsizetype groupIndex;
        const auto found = groupIndices.constFind(key);
        if (found == groupIndices.cend())
        {
            groupIndex = static_cast<qsizetype>(result.groups.size());
            groupIndices.insert(key, groupIndex);
            result.groups.push_back(
                {std::move(channels), std::vector<QString>{}});
            groupRules.emplace_back();
        }
        else
        {
            groupIndex = found.value();
        }
        if (groupRules[groupIndex].contains(regex))
        {
            ++result.duplicateRules;
            continue;
        }
        groupRules[groupIndex].insert(regex);
        result.groups[groupIndex].regexes.emplace_back(std::move(regex));
        if (adjusted)
        {
            ++result.adjustedRules;
        }
        else
        {
            ++result.exactRules;
        }
    }
    return result;
}

bool looksLikeChattyHighlightRules(const QString &text)
{
    for (auto line : text.split(u'\n', Qt::SkipEmptyParts))
    {
        line = line.trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        if (findPatternPrefix(line) ||
            line.startsWith(QStringLiteral("config:")) ||
            line.startsWith(QStringLiteral("chan:")) ||
            line.startsWith(QStringLiteral("!chan:")) ||
            line.startsWith(QStringLiteral("+reg")))
        {
            return true;
        }
    }
    return false;
}

bool looksLikeRegularExpressionList(const QString &text)
{
    static const QRegularExpression strongSyntax(QStringLiteral(
        R"((?:\\[AbBdDsSwWzZpP]|\\[.^$|?*+(){}\[\]\\]|\(\?|\[[^\]\r\n]+\]|\{\d+(?:,\d*)?\}|\.\*|\.\+|\|))"));
    const auto options = QRegularExpression::UseUnicodePropertiesOption;
    qsizetype rules = 0;
    qsizetype strongRules = 0;
    for (auto line : text.split(u'\n', Qt::SkipEmptyParts))
    {
        line = line.trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        ++rules;
        const QRegularExpression regex(line, options);
        if (!regex.isValid())
        {
            return false;
        }
        const bool anchored = line.startsWith(u'^') || line.endsWith(u'$');
        if (anchored || strongSyntax.match(line).hasMatch())
        {
            ++strongRules;
        }
    }
    return rules > 0 && strongRules > 0 && strongRules * 3 >= rules;
}

}
