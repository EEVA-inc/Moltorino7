#pragma once

#include "messages/Emote.hpp"

namespace chatterino {
std::shared_ptr<const EmoteMap> tikTokBuiltinEmotes();
EmotePtr tikTokEmote(QStringView name, QStringView url, int height = 28);
}
