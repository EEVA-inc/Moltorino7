#pragma once

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace chatterino {
class KickChatServer;
class BoostJsonValue;
}

namespace chatterino::kick::ws {

bool stripPrefix(std::string_view &str, std::string_view prefix);

bool stripSuffix(std::string_view &str, std::string_view suffix);

struct IDs {
    uint64_t roomID = 0;
    uint64_t channelID = 0;
};

IDs parseIDs(std::string_view channel);

struct Connection {
    bool centrifugo = false;
    QString url;
};

QString pusherUrl(QStringView appKey = {}, QStringView cluster = {});
std::optional<Connection> parseConnection(const QJsonObject &response);
void dispatch(KickChatServer *server, std::string_view channel,
              std::string_view event, BoostJsonValue data);
void disconnected(KickChatServer *server,
                  const std::unordered_set<QString> &subscriptions);

}
