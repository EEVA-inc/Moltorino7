#pragma once

#include "providers/tiktok/TikTokTypes.hpp"

#include <QByteArray>

#include <optional>

namespace chatterino::tiktok {

constexpr qsizetype MAX_FRAME_BYTES = 2 * 1024 * 1024;
constexpr qsizetype MAX_INFLATED_BYTES = 4 * 1024 * 1024;

struct Batch {
    QByteArray acknowledgement;
    std::vector<TikTokEvent> events;
    bool roomTraffic = false;
};

std::optional<Batch> decodeFrame(const QByteArray &data);
QByteArray heartbeat(quint64 roomID);
QByteArray enterRoom(quint64 roomID);
QUrl socketUrl(QStringView roomID);
QByteArray userAgent();

}
