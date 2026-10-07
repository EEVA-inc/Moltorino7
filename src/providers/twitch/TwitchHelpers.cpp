// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchHelpers.hpp"

#include "common/QLogging.hpp"
#include "providers/twitch/TwitchCommon.hpp"

#include <QRandomGenerator>

#include <algorithm>

namespace chatterino {

namespace {
constexpr qsizetype INVISIBLE_NONCE_AND_MESSAGE_BYTES = 32'346;
}

bool trimChannelName(const QString &channelName, QString &outChannelName)
{
    if (channelName.length() < 2)
    {
        qCDebug(chatterinoTwitch) << "channel name length below 2";
        return false;
    }

    outChannelName = channelName.mid(1);

    return true;
}

QString makeTwitchClientNonce(qsizetype length)
{
    if (length <= 0)
    {
        return {};
    }

    QString nonce;
    nonce.reserve(length);

    auto *random = QRandomGenerator::global();
    for (int i = 0; i < 4 && nonce.size() < length; ++i)
    {
        nonce += QStringLiteral("%1").arg(random->generate(), 8, 16,
                                          QLatin1Char('0'));
    }

    nonce.truncate(length);
    if (nonce.size() < length)
    {
        nonce.resize(length, QLatin1Char('0'));
    }
    return nonce.toLower();
}

QString makeInvisibleTwitchClientNonce(const QString &message)
{
    const auto messageBytes = message.toUtf8().size();
    const auto nonceLength = std::max<qsizetype>(
        32, INVISIBLE_NONCE_AND_MESSAGE_BYTES - messageBytes);
    return makeTwitchClientNonce(nonceLength);
}

bool isExtendedTwitchClientNonce(QStringView nonce)
{
    return nonce.size() >=
           INVISIBLE_NONCE_AND_MESSAGE_BYTES - TWITCH_MESSAGE_LIMIT * 4;
}

}
