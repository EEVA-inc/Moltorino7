// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QStringList>

#include <memory>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class TwitchChannel;
class KickChannel;
class YouTubeChannel;

struct CommandContext {
    QStringList words;
    QString rawText;

    ChannelPtr channel;

    TwitchChannel *twitchChannel;

    KickChannel *kickChannel;
    YouTubeChannel *youtubeChannel{};
};

}
