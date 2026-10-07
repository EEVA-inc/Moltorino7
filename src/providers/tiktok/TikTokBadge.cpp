#include "providers/tiktok/TikTokBadge.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/tiktok/TikTokEmotes.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QCache>
#include <QDataStream>
#include <QFontMetricsF>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace chatterino {
namespace {
constexpr qreal BADGE_HEIGHT = 18;
constexpr qreal TIKTOK_BADGE_HEIGHT = 14;

MessageElementFlag badgeFlag(int scene)
{
    switch (scene)
    {
        case 1:
        case 12:
            return MessageElementFlag::BadgeChannelAuthority;
        case 4:
        case 7:
            return MessageElementFlag::BadgeSubscription;
        default:
            return MessageElementFlag::BadgeVanity;
    }
}

QFont badgeFont(qreal height)
{
    auto font = getApp()->getFonts()->getFont(FontStyle::ChatMediumBold, 1.F);
    font.setPixelSize(std::max(1, qRound(height * 10 / TIKTOK_BADGE_HEIGHT)));
    font.setWeight(QFont::DemiBold);
    return font;
}

ImagePtr backgroundImage(const QString &url)
{
    return isTikTokImageUrl(QUrl(url)) ? Image::fromAutoscaledUrl(Url{url}, 18)
                                       : getEmptyImagePtr();
}

std::shared_ptr<const TikTokBadgeArtwork> badgeArtwork(const TikTokBadge &badge)
{
    static QMutex mutex;
    static QCache<QByteArray, std::shared_ptr<const TikTokBadgeArtwork>> cache(
        512);
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << badge.imageUrl << badge.label << badge.text << badge.textColor
           << badge.backgroundColor << badge.darkBackgroundColor
           << badge.backgroundUrl << badge.darkBackgroundUrl << badge.scene
           << badge.combined;
    QMutexLocker guard(&mutex);
    if (const auto *found = cache.object(key))
    {
        return *found;
    }
    auto result = std::make_shared<const TikTokBadgeArtwork>(
        TikTokBadgeArtwork{badge, backgroundImage(badge.backgroundUrl),
                           backgroundImage(badge.darkBackgroundUrl)});
    cache.insert(key, new std::shared_ptr<const TikTokBadgeArtwork>(result));
    return result;
}

class TikTokBadgeLayoutElement final : public ImageLayoutElement
{
public:
    TikTokBadgeLayoutElement(MessageElement &creator, const ImagePtr &image,
                             QSizeF size,
                             std::shared_ptr<const TikTokBadgeArtwork> artwork)
        : ImageLayoutElement(creator, image, size)
        , artwork_(std::move(artwork))
    {
    }

    void paint(QPainter &painter, const MessageColors &) override
    {
        paintTikTokBadge(painter, this->getRect(), *this->artwork_,
                         this->image_, getApp()->getThemes()->isLightTheme(),
                         TikTokBadgePaint::Static);
    }

    void paint(QPainter &painter, const MessageColors &colors, bool) override
    {
        this->paint(painter, colors);
    }

    void paint(QPainter &painter, const MessageColors &colors, bool,
               bool) override
    {
        this->paint(painter, colors);
    }

    bool hasAnimatedContent() const override
    {
        return this->image_->animated() ||
               this->artwork_->background->animated() ||
               this->artwork_->darkBackground->animated();
    }

    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *) override
    {
        if (!this->hasAnimatedContent())
        {
            return {};
        }
        const auto rect = this->getRect().translated(0, yOffset);
        paintTikTokBadge(painter, rect, *this->artwork_, this->image_,
                         getApp()->getThemes()->isLightTheme(),
                         TikTokBadgePaint::Animated);
        return rect.toAlignedRect();
    }

    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow, bool,
                          bool) override
    {
        return this->paintAnimated(painter, yOffset, shadow);
    }

private:
    std::shared_ptr<const TikTokBadgeArtwork> artwork_;
};
}

QSizeF tikTokBadgeSize(const TikTokBadgeArtwork &artwork, const ImagePtr &image,
                       qreal height)
{
    if (!artwork.badge.combined)
    {
        auto size = image->size();
        if (size.isEmpty())
        {
            return {height, height};
        }
        size.scale(height * 6, height, Qt::KeepAspectRatio);
        return size;
    }
    const auto scale = height / TIKTOK_BADGE_HEIGHT;
    if (artwork.badge.text.isEmpty())
    {
        return {18 * scale, height};
    }
    const QFontMetricsF metrics(badgeFont(height));
    const auto textWidth =
        std::min(metrics.horizontalAdvance(artwork.badge.text), height * 4);
    return {std::ceil(22 * scale + textWidth), height};
}

void paintTikTokBadge(QPainter &painter, const QRectF &rect,
                      const TikTokBadgeArtwork &artwork, const ImagePtr &image,
                      bool light, TikTokBadgePaint part)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto &badge = artwork.badge;
    const auto shouldDraw = [part](const ImagePtr &source) {
        return part == TikTokBadgePaint::All ||
               (part == TikTokBadgePaint::Animated) == source->animated();
    };
    bool animatedBackground = false;
    auto iconRect = rect;
    if (badge.combined)
    {
        const auto &color = !light && badge.darkBackgroundColor.isValid()
                                ? badge.darkBackgroundColor
                                : badge.backgroundColor;
        if (color.isValid() && part != TikTokBadgePaint::Animated)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const auto radius = rect.height() * 4 / TIKTOK_BADGE_HEIGHT;
            painter.drawRoundedRect(rect, radius, radius);
        }
        const auto &background = !light && !artwork.darkBackground->isEmpty()
                                     ? artwork.darkBackground
                                     : artwork.background;
        animatedBackground = background->animated();
        if (!background->isEmpty())
        {
            if (const auto pixmap = background->pixmapOrLoad();
                pixmap && shouldDraw(background))
            {
                painter.drawPixmap(rect, *pixmap, pixmap->rect());
            }
        }
        const auto scale = rect.height() / TIKTOK_BADGE_HEIGHT;
        iconRect = QRectF(rect.topLeft() + QPointF(3 * scale, scale),
                          QSizeF(12 * scale, 12 * scale));
        if (!badge.text.isEmpty() &&
            (part != TikTokBadgePaint::Animated || animatedBackground))
        {
            const auto font = badgeFont(rect.height());
            painter.setFont(font);
            painter.setPen(badge.textColor.isValid() ? badge.textColor
                                                     : QColor(Qt::white));
            const QRectF textRect(rect.x() + 15 * scale, rect.y(),
                                  rect.width() - 22 * scale, rect.height());
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetricsF(font).elidedText(
                                 badge.text, Qt::ElideRight, textRect.width()));
        }
    }
    if (const auto pixmap = image->pixmapOrLoad();
        pixmap && (shouldDraw(image) ||
                   (part == TikTokBadgePaint::Animated && animatedBackground)))
    {
        auto size = QSizeF(pixmap->size());
        size.scale(iconRect.size(), Qt::KeepAspectRatio);
        const QRectF target(
            iconRect.center() - QPointF(size.width() / 2, size.height() / 2),
            size);
        painter.drawPixmap(target, *pixmap, pixmap->rect());
    }
    painter.restore();
}

TikTokBadgeElement::TikTokBadgeElement(const TikTokBadge &badge, EmotePtr emote,
                                       std::optional<MessageElementFlags> flags)
    : BadgeElement(emote,
                   flags.value_or(MessageElementFlags{badgeFlag(badge.scene)}))
    , artwork_(badgeArtwork(badge))
{
    this->setTooltip(badge.label.toHtmlEscaped());
}

const std::shared_ptr<const TikTokBadgeArtwork> &TikTokBadgeElement::artwork()
    const
{
    return this->artwork_;
}

MessageLayoutElement *TikTokBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    const auto scale =
        image->size().height() > 0 ? size.height() / image->size().height() : 1;
    return new TikTokBadgeLayoutElement(
        *this, image,
        tikTokBadgeSize(*this->artwork_, image, BADGE_HEIGHT * scale),
        this->artwork_);
}

std::unique_ptr<MessageElement> TikTokBadgeElement::clone() const
{
    auto result = std::make_unique<TikTokBadgeElement>(this->artwork_->badge,
                                                       this->emote_);
    result->artwork_ = this->artwork_;
    result->cloneFrom(*this);
    return result;
}

void appendTikTokBadges(MessageBuilder &builder, const TikTokAuthor &author)
{
    for (const auto &badge : author.badges)
    {
        if (auto emote = tikTokEmote(badge.label, badge.imageUrl, 18))
        {
            builder.emplace<TikTokBadgeElement>(badge, std::move(emote));
        }
    }
}
}
