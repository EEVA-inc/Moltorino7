// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"

#include <QJsonArray>
#include <QString>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::recentmessages::detail {

bool hasParseableRecentMessages(const QJsonArray &messages);

void buildRecentMessagesBatched(
    QJsonArray messages, std::weak_ptr<Channel> channel,
    std::function<void(std::vector<MessagePtr>)> onBuilt);

QJsonArray mergeRecentMessageBatches(const std::vector<QJsonArray> &batches,
                                   int limit);

QUrl constructRecentMessagesUrl(
    const QString &urlTemplate, const QString &name, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before);

}
