// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringView>

namespace chatterino {

bool trimChannelName(const QString &channelName, QString &outChannelName);
QString makeTwitchClientNonce(qsizetype length = 32);
QString makeInvisibleTwitchClientNonce(const QString &message);
bool isExtendedTwitchClientNonce(QStringView nonce);

}
