#pragma once

#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/tiktok/TikTokTypes.hpp"

namespace chatterino {
class TikTokChannel;

class TikTokUsernameElement final : public TextElement
{
public:
    TikTokUsernameElement(const QString &text, MessageElementFlags flags,
                          MessageColor color, FontStyle font,
                          QString avatarUrl);
    std::unique_ptr<MessageElement> clone() const override;
    const QString &avatarUrl() const;

private:
    QString avatarUrl_;
};

std::pair<MessagePtrMut, HighlightAlert> makeTikTokMessage(
    TikTokChannel *channel, const TikTokEvent &event);
}
