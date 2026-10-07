#include "providers/tiktok/TikTokText.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <stdexcept>

namespace chatterino::tiktok::livetext {
namespace {
using Unit = char16_t;
bool between(Unit u, Unit a, Unit b)
{
    return u >= a && u <= b;
}

bool surrogate(Unit u)
{
    return between(u, 0xd800, 0xdfff);
}

Unit at(const QString &s, qsizetype p)
{
    return p < s.size() ? static_cast<Unit>(s[p].unicode()) : Unit{0};
}

bool pair(const QString &s, qsizetype p)
{
    return p + 1 < s.size() && between(at(s, p), 0xd800, 0xdbff) &&
           between(at(s, p + 1), 0xdc00, 0xdfff);
}

bool combining(Unit u)
{
    return between(u, 0x0300, 0x036f) || between(u, 0xfe20, 0xfe2f) ||
           between(u, 0x20d0, 0x20ff) || between(u, 0x1ab0, 0x1aff) ||
           between(u, 0x1dc0, 0x1dff);
}

bool skin(const QString &s, qsizetype p)
{
    return p + 1 < s.size() && at(s, p) == 0xd83c &&
           between(at(s, p + 1), 0xdffb, 0xdfff);
}

bool regional(const QString &s, qsizetype p)
{
    return p + 1 < s.size() && at(s, p) == 0xd83c &&
           between(at(s, p + 1), 0xdde6, 0xddff);
}

bool teluguConsonant(Unit u)
{
    return between(u, 0x0c15, 0x0c28) || between(u, 0x0c2a, 0x0c39);
}

bool teluguExtendedConsonant(Unit u)
{
    return teluguConsonant(u) || between(u, 0x0c58, 0x0c5a);
}

bool teluguVowelMark(Unit u)
{
    return between(u, 0x0c3e, 0x0c44) || between(u, 0x0c46, 0x0c48) ||
           between(u, 0x0c4a, 0x0c4c) || between(u, 0x0c62, 0x0c63);
}

bool teluguSourceClass(Unit u)
{
    return between(u, 0x0c01, 0x0c03) || u == 0x0c4d || u == 0x0c55 ||
           u == 0x0c56 || between(u, 0x0c05, 0x0c0c) ||
           between(u, 0x0c0e, 0x0c10) || between(u, 0x0c12, 0x0c14) ||
           between(u, 0x0c60, 0x0c61) || teluguExtendedConsonant(u) ||
           between(u, 0x0c66, 0x0c6f) || between(u, 0x0c78, 0x0c7e) ||
           u == u'|' || u == u'[' || u == u'(' || u == u')' || u == u'?' ||
           u == u'!' || u == u':';
}

bool tagFlag(const QString &s, qsizetype p)
{
    if (p + 13 >= s.size())
    {
        return false;
    }
    return at(s, p) == 0xd83c && at(s, p + 1) == 0xdff4 &&
           at(s, p + 2) == 0xdb40 && at(s, p + 3) == 0xdc67 &&
           at(s, p + 4) == 0xdb40 && at(s, p + 5) == 0xdc62 &&
           at(s, p + 6) == 0xdb40 &&
           (at(s, p + 7) == 0xdc65 || at(s, p + 7) == 0xdc73 ||
            at(s, p + 7) == 0xdc77) &&
           at(s, p + 8) == 0xdb40 &&
           (at(s, p + 9) == 0xdc6e || at(s, p + 9) == 0xdc63 ||
            at(s, p + 9) == 0xdc6c) &&
           at(s, p + 10) == 0xdb40 &&
           (at(s, p + 11) == 0xdc67 || at(s, p + 11) == 0xdc74 ||
            at(s, p + 11) == 0xdc73) &&
           at(s, p + 12) == 0xdb40 && at(s, p + 13) == 0xdc7f;
}

qsizetype variationAndModifier(const QString &s, qsizetype p)
{
    if (p < s.size() && (at(s, p) == 0xfe0e || at(s, p) == 0xfe0f))
    {
        ++p;
    }
    if (p < s.size() && combining(at(s, p)))
    {
        ++p;
    }
    else if (skin(s, p))
    {
        p += 2;
    }
    return p;
}

qsizetype tokenEnd(const QString &s, qsizetype p)
{
    if (skin(s, p) && skin(s, p + 2))
    {
        return p + 2;
    }
    if (p + 2 < s.size() && teluguConsonant(at(s, p)) &&
        at(s, p + 1) == 0x0c4d && teluguConsonant(at(s, p + 2)))
    {
        return p + 3;
    }
    if (p + 1 < s.size() && teluguExtendedConsonant(at(s, p)) &&
        (teluguVowelMark(at(s, p + 1)) || teluguSourceClass(at(s, p + 1))))
    {
        return p + 2;
    }
    if (tagFlag(s, p))
    {
        p += 14;
    }
    else if (!surrogate(at(s, p)))
    {
        ++p;
        if (p < s.size() && combining(at(s, p)))
        {
            ++p;
        }
    }
    else if (regional(s, p) && regional(s, p + 2))
    {
        p += 4;
    }
    else if (pair(s, p))
    {
        p += 2;
    }
    else
    {
        ++p;
    }
    p = variationAndModifier(s, p);
    while (p + 1 < s.size() && at(s, p) == 0x200d)
    {
        qsizetype next = p + 1;
        if (!surrogate(at(s, next)))
        {
            ++next;
        }
        else if (regional(s, next) && regional(s, next + 2))
        {
            next += 4;
        }
        else if (pair(s, next))
        {
            next += 2;
        }
        else
        {
            break;
        }
        p = variationAndModifier(s, next);
    }
    return p;
}

struct CountInput {
    QString text;
    QVector<qsizetype> originalOffsets;
};
CountInput countInput(const QString &original, bool countAnsiEscapeCodes)
{
    CountInput result;
    result.text.reserve(original.size());
    result.originalOffsets.reserve(original.size());
    if (countAnsiEscapeCodes)
    {
        result.text = original;
        for (qsizetype p = 0; p < original.size(); ++p)
        {
            result.originalOffsets.push_back(p);
        }
        return result;
    }

    QString proxy = original;
    for (qsizetype p = 0; p < proxy.size(); ++p)
    {
        if (proxy[p].unicode() > 0x7f && proxy[p].unicode() != 0x9b)
        {
            proxy[p] = QChar(0x0100);
        }
    }
    static const QRegularExpression ansi(QString::fromLatin1(
        R"([\x{001B}\x{009B}][[\]()#;?]*(?:(?:(?:(?:;[-a-zA-Z\d\/#&.:=?%@~_]+)*|[a-zA-Z\d]+(?:;[-a-zA-Z\d\/#&.:=?%@~_]*)*)?\x{0007})|(?:(?:\d{1,4}(?:;\d{0,4})*)?[\dA-PR-TZcf-nq-uy=><~])))"));
    if (!ansi.isValid())
    {
        throw std::runtime_error("ANSI expression failed to compile");
    }
    qsizetype p = 0;
    auto appendUntil = [&](qsizetype end) {
        while (p < end)
        {
            result.text.append(original[p]);
            result.originalOffsets.push_back(p++);
        }
    };
    auto matches = ansi.globalMatch(proxy);
    while (matches.hasNext())
    {
        const auto match = matches.next();
        appendUntil(match.capturedStart());
        p = match.capturedEnd();
    }
    appendUntil(original.size());
    return result;
}
}

TextAnalysis analyzeEditorText(const QString &text, bool countAnsiEscapeCodes)
{
    const auto input = countInput(text, countAnsiEscapeCodes);
    TextAnalysis result;
    result.utf16Boundaries.push_back(0);
    qsizetype p = 0;
    while (p < input.text.size())
    {
        p = tokenEnd(input.text, p);
        result.utf16Boundaries.push_back(input.originalOffsets[p - 1] + 1);
        ++result.count;
    }
    return result;
}

std::optional<qsizetype> incomingIndexToUtf16(const QString &content,
                                              qsizetype index,
                                              qsizetype precedingEmotes)
{
    if (precedingEmotes < 0 || index < precedingEmotes ||
        index - precedingEmotes > content.size())
    {
        return std::nullopt;
    }
    return index - precedingEmotes;
}

}
