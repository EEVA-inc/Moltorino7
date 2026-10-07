// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/InputHighlighter.hpp"

#include "Application.hpp"
#include "common/Aliases.hpp"
#include "common/LinkParser.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Emote.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Settings.hpp"

#include <QTextCharFormat>
#include <QTextDocument>

namespace {

using namespace chatterino;

bool isEmote(TwitchChannel *twitch, KickChannel *kick, YouTubeChannel *youtube,
             const QString &word)
{
    EmoteName name{word};
    if (twitch)
    {
        if (twitch->bttvEmote(name) || twitch->ffzEmote(name) ||
            twitch->seventvEmote(name))
        {
            return true;
        }
        auto locals = twitch->localTwitchEmotes();
        if (locals->contains(name))
        {
            return true;
        }
    }
    if (kick)
    {
        if (kick->seventvEmote(name))
        {
            return true;
        }

        auto globals = getApp()->getKickChatServer()->globalEmotes();
        if (globals->contains(name))
        {
            return true;
        }
    }
    if (youtube)
    {
        return youtube->youtubeEmote(QStringView{word}) != nullptr;
    }

    if (getApp()->getBttvEmotes()->emote(name) ||
        getApp()->getFfzEmotes()->emote(name) ||
        getApp()->getSeventvEmotes()->globalEmote(name))
    {
        return true;
    }

    if (getApp()
            ->getAccounts()
            ->twitch.getCurrent()
            ->twitchEmote(name)
            .has_value())
    {
        return true;
    }

    return false;
}

bool isChatter(TwitchChannel *twitch, KickChannel *kick,
               YouTubeChannel *youtube, const QString &word)
{
    ChannelChatters *cc = nullptr;
    Channel *c = nullptr;
    if (twitch)
    {
        cc = twitch;
        c = twitch;
    }
    else if (kick)
    {
        cc = kick;
        c = kick;
    }
    else if (youtube)
    {
        cc = youtube;
        c = youtube;
    }
    if (cc)
    {
        const auto broadcasterName =
            youtube ? c->getDisplayName() : c->getName();
        if (cc->accessChatters()->contains(word) ||
            (getSettings()->alwaysIncludeBroadcasterInUserCompletions &&
             word.compare(broadcasterName, Qt::CaseInsensitive) == 0))
        {
            return true;
        }
    }
    return false;
}

bool isLink(const QString &token)
{

    auto link = linkparser::parse(token);
    return link.has_value();
}

bool isIgnoredWord(TwitchChannel *twitch, KickChannel *kick,
                   YouTubeChannel *youtube, const QString &word)
{
    return isEmote(twitch, kick, youtube, word) ||
           isChatter(twitch, kick, youtube, word);
}

bool isIgnoredToken(TwitchChannel *twitch, KickChannel *kick,
                    YouTubeChannel *youtube, const QString &token)
{
    return isEmote(twitch, kick, youtube, token) || isLink(token);
}

}

namespace chatterino {

namespace inputhighlight::detail {

QRegularExpression wordRegex()
{
    static QRegularExpression regex{
        R"((?<=^|(?!_)\p{P})\p{L}+(?:['-]\p{L}+)*(?=$|(?!_)\p{P}))",
        QRegularExpression::PatternOption::UseUnicodePropertiesOption,
    };
    return regex;
}

}

InputHighlighter::InputHighlighter(SpellChecker &spellChecker, QObject *parent)
    : QSyntaxHighlighter(parent)
    , spellChecker(spellChecker)
    , wordRegex(inputhighlight::detail::wordRegex())
    , tokenRegex(R"(\S+)")
{
    this->spellFmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    this->spellFmt.setUnderlineColor(Qt::red);
}
InputHighlighter::~InputHighlighter() = default;

void InputHighlighter::setChannel(const std::shared_ptr<Channel> &channel)
{
    auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
    this->channel = twitch;
    auto kick = std::dynamic_pointer_cast<KickChannel>(channel);
    this->kickChannel = kick;
    auto youtube = std::dynamic_pointer_cast<YouTubeChannel>(channel);
    this->youtubeChannel = youtube;
    this->rehighlight();
}

std::vector<QString> InputHighlighter::getSpellCheckedWords(const QString &text)
{
    std::vector<QString> words;
    this->visitWords(text, [&](const QString &word, qsizetype ,
                               qsizetype ) {
        words.emplace_back(word);
    });
    return words;
}

QStringView InputHighlighter::getWordAt(QStringView text, qsizetype pos)
{
    auto tokenIt = this->tokenRegex.globalMatchView(text);
    QString token;
    qsizetype posInWord = 0;
    qsizetype tokenStart = 0;
    while (tokenIt.hasNext())
    {
        auto match = tokenIt.next();

        if (match.capturedStart() <= pos && pos <= match.capturedEnd())
        {
            token = match.captured();
            tokenStart = match.capturedStart();
            posInWord = pos - tokenStart;
            break;
        }
    }
    if (token.isEmpty())
    {
        return {};
    }

    QStringView word;
    this->visitWords(token, [&](const QString & , qsizetype start,
                                qsizetype count) {
        if (start <= posInWord && posInWord <= start + count)
        {
            assert(word.isEmpty());
            word = text.sliced(tokenStart + start, count);
        }
    });
    return word;
}

void InputHighlighter::highlightBlock(const QString &text)
{
    if (!this->spellChecker.isLoaded())
    {
        return;
    }
    this->visitWords(
        text, [&](const QString &word, qsizetype start, qsizetype count) {
            if (!this->spellChecker.check(word))
            {
                this->setFormat(static_cast<int>(start),
                                static_cast<int>(count), this->spellFmt);
            }
        });
}

void InputHighlighter::visitWords(
    const QString &text,
    std::invocable<const QString &, qsizetype, qsizetype> auto &&cb)
{
    auto *channel = this->channel.lock().get();
    auto *kick = this->kickChannel.lock().get();
    auto *youtube = this->youtubeChannel.lock().get();

    QStringView textView = text;

    auto cmdTriggerLen = getApp()->getCommands()->commandTriggerLen(textView);
    textView = textView.sliced(cmdTriggerLen);

    auto tokenIt = this->tokenRegex.globalMatchView(textView);

    while (tokenIt.hasNext())
    {
        auto tokenMatch = tokenIt.next();
        auto token = tokenMatch.captured();
        if (isIgnoredToken(channel, kick, youtube, token))
        {
            continue;
        }

        auto wordIt = this->wordRegex.globalMatchView(token);

        while (wordIt.hasNext())
        {
            auto wordMatch = wordIt.next();
            auto word = wordMatch.captured();

            if (!isIgnoredWord(channel, kick, youtube, word))
            {
                cb(word,
                   static_cast<int>(cmdTriggerLen + tokenMatch.capturedStart() +
                                    wordMatch.capturedStart()),
                   static_cast<int>(word.size()));
            }
        }
    }
}

}
