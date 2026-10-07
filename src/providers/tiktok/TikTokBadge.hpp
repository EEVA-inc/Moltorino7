#pragma once

#include "messages/MessageElement.hpp"
#include "providers/tiktok/TikTokTypes.hpp"

namespace chatterino {
class MessageBuilder;

struct TikTokBadgeArtwork {
    TikTokBadge badge;
    ImagePtr background;
    ImagePtr darkBackground;
};

QSizeF tikTokBadgeSize(const TikTokBadgeArtwork &artwork, const ImagePtr &image,
                       qreal height);
enum class TikTokBadgePaint { All, Static, Animated };
void paintTikTokBadge(QPainter &painter, const QRectF &rect,
                      const TikTokBadgeArtwork &artwork, const ImagePtr &image,
                      bool light,
                      TikTokBadgePaint part = TikTokBadgePaint::All);

class TikTokBadgeElement final : public BadgeElement
{
public:
    explicit TikTokBadgeElement(
        const TikTokBadge &badge, EmotePtr emote,
        std::optional<MessageElementFlags> flags = std::nullopt);
    const std::shared_ptr<const TikTokBadgeArtwork> &artwork() const;
    std::unique_ptr<MessageElement> clone() const override;

protected:
    MessageLayoutElement *makeImageLayoutElement(const ImagePtr &image,
                                                 QSizeF size) override;

private:
    std::shared_ptr<const TikTokBadgeArtwork> artwork_;
};

void appendTikTokBadges(MessageBuilder &builder, const TikTokAuthor &author);

}
