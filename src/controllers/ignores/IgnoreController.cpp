// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/ignores/IgnoreController.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchIrc.hpp"
#include "singletons/Settings.hpp"

namespace {

using namespace chatterino::literals;

QString makeRegexReplacement(QStringView source,
                             const QRegularExpression &regex,
                             const QRegularExpressionMatch &match,
                             const QString &replacement)
{
    using SizeType = QString::size_type;
    struct QStringCapture {
        SizeType pos;
        SizeType len;
        int captureNumber;
    };

    qsizetype numCaptures = regex.captureCount();

    QVarLengthArray<QStringCapture> backReferences;

    SizeType replacementLength = replacement.size();
    for (SizeType i = 0; i + 1 < replacementLength; i++)
    {
        if (replacement[i] != u'\\')
        {
            continue;
        }

        int no = replacement[i + 1].digitValue();
        if (no <= 0 || no > numCaptures)
        {
            continue;
        }

        QStringCapture backReference{.pos = i, .len = 2};

        if (i < replacementLength - 2)
        {
            int secondDigit = replacement[i + 2].digitValue();
            if (secondDigit != -1 && ((no * 10) + secondDigit) <= numCaptures)
            {
                no = (no * 10) + secondDigit;
                ++backReference.len;
            }
        }

        backReference.captureNumber = no;
        backReferences.append(backReference);
    }

    SizeType newLength = 0;
    QVarLengthArray<QStringView> chunks;
    QStringView replacementView{replacement};

    SizeType len = 0;
    SizeType lastEnd = 0;
    for (const QStringCapture &backReference : std::as_const(backReferences))
    {

        len = backReference.pos - lastEnd;
        if (len > 0)
        {
            chunks << replacementView.mid(lastEnd, len);
            newLength += len;
        }

        len = match.capturedLength(backReference.captureNumber);
        if (len > 0)
        {
            chunks << source.mid(
                match.capturedStart(backReference.captureNumber), len);
            newLength += len;
        }

        lastEnd = backReference.pos + backReference.len;
    }

    len = replacementView.size() - lastEnd;
    if (len > 0)
    {
        chunks << replacementView.mid(lastEnd, len);
        newLength += len;
    }

    QString dst;
    dst.reserve(newLength);
    for (const QStringView &chunk : std::as_const(chunks))
    {
        dst += chunk;
    }
    return dst;
}

}

namespace chatterino {

bool isIgnoredMessage(IgnoredMessageParameters &&params)
{
    if (!params.message.isEmpty())
    {

        auto phrases = getSettings()->ignoredMessages.readOnly();
        for (const auto &phrase : *phrases)
        {
            if (phrase.isBlock() && phrase.isMatch(params.message))
            {
                qCDebug(chatterinoMessage)
                    << "Blocking message because it contains ignored phrase"
                    << phrase.getPattern();
                return true;
            }
        }
    }

    if (getSettings()->enableTwitchBlockedUsers)
    {
        bool isBlocked = false;

        if (!params.twitchUserID.isEmpty())
        {
            isBlocked = getApp()
                            ->getAccounts()
                            ->twitch.getCurrent()
                            ->blockedUserIds()
                            .contains(params.twitchUserID);
        }
        else if (!params.twitchUserLogin.isEmpty())
        {
            isBlocked = getApp()
                            ->getAccounts()
                            ->twitch.getCurrent()
                            ->blockedUserLogins()
                            .contains(params.twitchUserLogin);
        }

        if (isBlocked)
        {
            switch (static_cast<ShowIgnoredUsersMessages>(
                getSettings()->showBlockedUsersMessages.getValue()))
            {
                case ShowIgnoredUsersMessages::IfModerator:
                    if (params.isMod || params.isBroadcaster)
                    {
                        return false;
                    }
                    break;
                case ShowIgnoredUsersMessages::IfBroadcaster:
                    if (params.isBroadcaster)
                    {
                        return false;
                    }
                    break;
                case ShowIgnoredUsersMessages::Never:
                    break;
            }

            return true;
        }
    }

    return false;
}

void processIgnorePhrases(const std::vector<IgnorePhrase> &phrases,
                          QString &content,
                          std::vector<TwitchEmoteOccurrence> &twitchEmotes,
                          std::vector<TwitchGifOccurrence> *twitchGifs)
{
    using SizeType = QString::size_type;

    auto removeEmotesInRange = [&twitchEmotes](SizeType pos, SizeType len) {
        const auto end = pos + len;

        auto it = std::partition(
            twitchEmotes.begin(), twitchEmotes.end(),
            [pos, end, len](const auto &item) {
                if (len == 0)
                {
                    return !(item.start < static_cast<qint64>(pos) &&
                             item.end >= static_cast<qint64>(pos));
                }
                return !(item.start < end && item.end >= pos);
            });
        std::vector<TwitchEmoteOccurrence> emotesInRange(it,
                                                         twitchEmotes.end());
        twitchEmotes.erase(it, twitchEmotes.end());
        return emotesInRange;
    };

    auto shiftIndicesAfter = [&twitchEmotes](SizeType pos, qint64 by) {
        for (auto &item : twitchEmotes)
        {
            if (item.start >= 0 && static_cast<SizeType>(item.start) >= pos)
            {
                item.start = static_cast<int>(item.start + by);
                item.end = static_cast<int>(item.end + by);
            }
        }
    };

    auto removeGifsInRange = [twitchGifs](SizeType pos, SizeType len) {
        if (twitchGifs == nullptr)
        {
            return;
        }

        if (len == 0)
        {
            std::erase_if(*twitchGifs, [pos](const auto &item) {
                return item.start < static_cast<qint64>(pos) &&
                       item.end >= static_cast<qint64>(pos);
            });
            return;
        }

        const auto end = pos + len;
        std::erase_if(*twitchGifs, [pos, end](const auto &item) {
            return item.start < end && item.end >= pos;
        });
    };

    auto shiftGifIndicesAfter = [twitchGifs](SizeType pos, qint64 by) {
        if (twitchGifs == nullptr || by == 0)
        {
            return;
        }

        for (auto &item : *twitchGifs)
        {
            if (item.start >= 0 && static_cast<SizeType>(item.start) >= pos)
            {
                item.start = static_cast<int>(item.start + by);
                item.end = static_cast<int>(item.end + by);
            }
        }
    };

    auto addReplEmotes = [&twitchEmotes](const IgnorePhrase &phrase,
                                         const auto &midrepl,
                                         SizeType startIndex) {
        if (!phrase.containsEmote())
        {
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto words = midrepl.tokenize(u' ');
#else
        auto words = midrepl.split(' ');
#endif
        SizeType pos = 0;
        for (const auto &word : words)
        {
            for (const auto &emote : phrase.getEmotes())
            {
                if (word == emote.first.string)
                {
                    if (emote.first.string.isEmpty() ||
                        emote.second == nullptr)
                    {
                        qCDebug(chatterinoTwitch)
                            << "emote null" << emote.first.string;
                        continue;
                    }
                    twitchEmotes.push_back(TwitchEmoteOccurrence{
                        static_cast<int>(startIndex + pos),
                        static_cast<int>(startIndex + pos +
                                         emote.first.string.length() - 1),
                        emote.second,
                        emote.first,
                    });
                }
            }
            pos += word.length() + 1;
        }
    };

    auto replaceMessageAt = [&](const IgnorePhrase &phrase, SizeType from,
                                SizeType length, const QString &replacement) {
        auto removedEmotes = removeEmotesInRange(from, length);
        removeGifsInRange(from, length);
        content.replace(from, length, replacement);
        auto wordStart = from;
        while (wordStart > 0)
        {
            if (content[wordStart - 1] == ' ')
            {
                break;
            }
            --wordStart;
        }
        auto wordEnd = from + replacement.length();
        while (wordEnd < content.length())
        {
            if (content[wordEnd] == ' ')
            {
                break;
            }
            ++wordEnd;
        }

        const auto sourceEnd = from + length;
        const auto delta = static_cast<qint64>(replacement.length()) -
                           static_cast<qint64>(length);
        shiftIndicesAfter(sourceEnd, delta);
        shiftGifIndicesAfter(sourceEnd, delta);

        auto midExtendedRef =
            QStringView{content}.mid(wordStart, wordEnd - wordStart);

        for (auto &emote : removedEmotes)
        {
            if (emote.ptr == nullptr)
            {
                qCDebug(chatterinoTwitch)
                    << "Invalid emote occurrence" << emote.name.string;
                continue;
            }
            QRegularExpression emoteregex(
                "\\b" + QRegularExpression::escape(emote.name.string) + "\\b",
                QRegularExpression::UseUnicodePropertiesOption);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            auto match = emoteregex.matchView(midExtendedRef);
#else
            auto match = emoteregex.match(midExtendedRef);
#endif
            if (match.hasMatch())
            {
                emote.start =
                    static_cast<int>(wordStart + match.capturedStart());
                emote.end =
                    static_cast<int>(wordStart + match.capturedEnd() - 1);
                twitchEmotes.push_back(std::move(emote));
            }
        }

        addReplEmotes(phrase, midExtendedRef, wordStart);
    };

    for (const auto &phrase : phrases)
    {
        if (phrase.isBlock())
        {
            continue;
        }
        const auto &pattern = phrase.getPattern();
        if (pattern.isEmpty())
        {
            continue;
        }
        if (phrase.isRegex())
        {
            const auto &regex = phrase.getRegex();
            if (!regex.isValid())
            {
                continue;
            }

            QRegularExpressionMatch match;
            size_t iterations = 0;
            SizeType from = 0;
            while ((from = content.indexOf(regex, from, &match)) != -1)
            {
                auto replacement = phrase.getReplace();
                if (regex.captureCount() > 0)
                {
                    replacement = makeRegexReplacement(content, regex, match,
                                                       replacement);
                }

                replaceMessageAt(phrase, from, match.capturedLength(),
                                 replacement);
                from += replacement.length();
                iterations++;
                if (iterations >= 128)
                {
                    content = u"Too many replacements - check your ignores!"_s;
                    twitchEmotes.clear();
                    if (twitchGifs != nullptr)
                    {
                        twitchGifs->clear();
                    }
                    return;
                }
            }

            continue;
        }

        SizeType from = 0;
        while ((from = content.indexOf(pattern, from,
                                       phrase.caseSensitivity())) != -1)
        {
            replaceMessageAt(phrase, from, pattern.length(),
                             phrase.getReplace());
            from += phrase.getReplace().length();
        }
    }
}

}
