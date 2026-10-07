#pragma once

#include <QString>

#include <optional>

namespace chatterino::MoltorinoAuth::detail {

std::optional<QString> advanceModeratedChannelsCursor(
    const QString &cursor, const QString &currentChannelId,
    const QString &nextChannelId);

}
