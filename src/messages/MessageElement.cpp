// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageElement.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/Helpers.hpp"
#include "util/Variant.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTextLayout>
#include <QTimer>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

#ifdef CHATTERINO_WITH_PRIVATE_QT_API
#    include <QtGui/private/qtextengine_p.h>
#endif

namespace chatterino {

using namespace literals;

namespace {

bool channelAvatarRelayoutQueued = false;

void queueChannelAvatarRelayout()
{
    if (channelAvatarRelayoutQueued)
    {
        return;
    }
    channelAvatarRelayoutQueued = true;
    auto *application = QCoreApplication::instance();
    if (application == nullptr)
    {
        channelAvatarRelayoutQueued = false;
        return;
    }
    QTimer::singleShot(0, application, [] {
        channelAvatarRelayoutQueued = false;
        getApp()->getWindows()->forceLayoutChannelViews();
    });
}

// Computes the bounding box for the given vector of images
QSizeF getBoundingBoxSize(const std::vector<ImagePtr> &images)
{
    qreal width = 0;
    qreal height = 0;
    for (const auto &img : images)
    {
        QSizeF s = img->size();
        width = std::max(width, s.width());
        height = std::max(height, s.height());
    }

    return {width, height};
}

std::shared_ptr<Paint> getPaintForExactMatch(const HighlightMatch &match)
{
    if (match.paintID.isEmpty() || !getSettings()->displaySevenTVPaints)
    {
        return {};
    }

    return getApp()->getSeventvPaints()->getPaintByID(match.paintID);
}

bool hasExactMatchMarker(const HighlightMatch &match)
{
    return match.style != HighlightMatchStyle::None;
}

bool hasExactTextAppearance(const HighlightMatch &match)
{
    return match.style != HighlightMatchStyle::None || !match.paintID.isEmpty();
}

QSizeF textElementSize(const QString &text, const QFontMetricsF &metrics,
                       const QFont *layoutFont)
{
    QSizeF size{
        metrics.horizontalAdvance(text),
        metrics.height(),
    };

    if (layoutFont == nullptr || text.isEmpty())
    {
        return size;
    }

    size.setHeight(std::max(size.height(), metrics.lineSpacing()));

    QTextLayout layout(text, *layoutFont);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
    {
        line.setLineWidth(std::max<qreal>(100000.0, size.width() + 100.0));
        size.setWidth(std::max(size.width(), line.naturalTextWidth()));
        size.setHeight(std::max(size.height(), line.height()));
    }
    layout.endLayout();

    const QRectF singleLineBounds = metrics.boundingRect(
        QRectF(0, 0, 100000, 100000),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, text);
    size.setWidth(std::max(size.width(), singleLineBounds.width()));

    return {
        std::ceil(size.width()),
        std::ceil(size.height()),
    };
}

EmotePtr getKickBadge()
{
    static EmotePtr ptr = std::make_shared<const Emote>(Emote{
        .name = {u"Kick"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-kick-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-kick-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return ptr;
}

EmotePtr getTwitchBadge()
{
    static EmotePtr ptr = std::make_shared<const Emote>(Emote{
        .name = {u"Twitch"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-twitch-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-twitch-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return ptr;
}

EmotePtr getYouTubeBadge()
{
    static EmotePtr ptr = std::make_shared<const Emote>(Emote{
        .name = {u"YouTube"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-youtube-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-youtube-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return ptr;
}

EmotePtr getTikTokBadge()
{
    static const auto badge = std::make_shared<const Emote>(Emote{
        .name = {u"TikTok"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-tiktok-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-tiktok-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return badge;
}

}  // namespace

struct MessageElement::Metadata {
    Link link;
    QString tooltip;
};

MessageElement::MessageElement(MessageElementFlags flags)
    : flags_(flags)
{
    DebugCount::increase(DebugObject::MessageElement);
}

MessageElement::~MessageElement()
{
    DebugCount::decrease(DebugObject::MessageElement);
}

MessageElement *MessageElement::setLink(const Link &link)
{
    const bool linkIsEmpty = !link.isValid() && link.value.isEmpty();
    if (linkIsEmpty && !this->metadata_)
    {
        return this;
    }

    if (!this->metadata_)
    {
        this->metadata_ = std::make_unique<Metadata>();
    }
    this->metadata_->link = link;
    if (!this->metadata_->link.isValid() &&
        this->metadata_->link.value.isEmpty() &&
        (!this->hasTooltipOverride_ || this->metadata_->tooltip.isEmpty()))
    {
        this->metadata_.reset();
    }
    return this;
}

MessageElement *MessageElement::setTooltip(const QString &tooltip)
{
    this->hasTooltipOverride_ = tooltip != this->getDefaultTooltip();

    if (!this->hasTooltipOverride_ || tooltip.isEmpty())
    {
        if (this->metadata_)
        {
            this->metadata_->tooltip.clear();
            if (!this->metadata_->link.isValid() &&
                this->metadata_->link.value.isEmpty())
            {
                this->metadata_.reset();
            }
        }
        return this;
    }

    if (!this->metadata_)
    {
        this->metadata_ = std::make_unique<Metadata>();
    }
    this->metadata_->tooltip = tooltip;
    return this;
}

MessageElement *MessageElement::setTrailingSpace(bool value)
{
    this->trailingSpace = value;
    return this;
}

const QString &MessageElement::getTooltip() const
{
    static const QString EMPTY_TOOLTIP;
    if (!this->hasTooltipOverride_)
    {
        return this->getDefaultTooltip();
    }
    return this->metadata_ ? this->metadata_->tooltip : EMPTY_TOOLTIP;
}

const QString &MessageElement::getDefaultTooltip() const
{
    static const QString EMPTY_TOOLTIP;
    return EMPTY_TOOLTIP;
}

Link MessageElement::getLink() const
{
    return this->metadata_ ? this->metadata_->link : Link{};
}

bool MessageElement::hasTrailingSpace() const
{
    return this->trailingSpace;
}

MessageElementFlags MessageElement::getFlags() const
{
    return this->flags_;
}

void MessageElement::addFlags(MessageElementFlags flags)
{
    this->flags_.set(flags);
}

void MessageElement::cloneFrom(const MessageElement &source)
{
    this->metadata_ = source.metadata_
                          ? std::make_unique<Metadata>(*source.metadata_)
                          : nullptr;
    this->flags_ = source.flags_;
    this->trailingSpace = source.trailingSpace;
    this->hasTooltipOverride_ = source.hasTooltipOverride_;
}

QJsonObject MessageElement::toJson() const
{
    const auto link = this->metadata_ ? this->metadata_->link : Link{};
    return {
        {"trailingSpace"_L1, this->trailingSpace},
        {
            "link"_L1,
            {{
                {"type"_L1, qmagicenum::enumNameString(link.type)},
                {"value"_L1, link.value},
            }},
        },
        {"tooltip"_L1, this->getTooltip()},
        {"flags"_L1, qmagicenum::enumFlagsName(this->flags_.value())},
    };
}

// IMAGE
ImageElement::ImageElement(ImagePtr image, MessageElementFlags flags)
    : MessageElement(flags)
    , image_(std::move(image))
{
}

void ImageElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        container.addElement(new ImageLayoutElement(
            *this, this->image_, this->image_->size() * container.getScale()));
    }
}

std::unique_ptr<MessageElement> ImageElement::clone() const
{
    auto el = std::make_unique<ImageElement>(this->image_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

ImagePtr ImageElement::image() const
{
    return this->image_;
}

QJsonObject ImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ImageElement"_s;
    base["url"_L1] = this->image_->url().string;

    return base;
}

std::string_view ImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

CircularImageElement::CircularImageElement(ImagePtr image, int padding,
                                           QColor background,
                                           MessageElementFlags flags)
    : MessageElement(flags)
    , image_(std::move(image))
    , padding_(padding)
    , background_(background)
{
}

void CircularImageElement::addToContainer(MessageLayoutContainer &container,
                                          const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        if (this->getFlags().has(MessageElementFlag::ReplyButton))
        {
            container.ensureSingleSpaceBeforeNextElement();
        }

        auto imgSize = QSize(this->image_->width(), this->image_->height()) *
                       container.getScale();

        container.addElement(new ImageWithCircleBackgroundLayoutElement(
            *this, this->image_, imgSize, this->background_, this->padding_));
    }
}

std::unique_ptr<MessageElement> CircularImageElement::clone() const
{
    auto el = std::make_unique<CircularImageElement>(
        this->image_, this->padding_, this->background_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject CircularImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"CircularImageElement"_s;
    base["url"_L1] = this->image_->url().string;
    base["padding"_L1] = this->padding_;
    base["background"_L1] = this->background_.name(QColor::HexArgb);

    return base;
}

std::string_view CircularImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// EMOTE
EmoteElement::EmoteElement(const EmotePtr &emote, MessageElementFlags flags,
                           const MessageColor &textElementColor,
                           bool gigantified, qreal horizontalImagePadding,
                           bool isTwitchGif)
    : MessageElement(flags)
    , textColor_(textElementColor)
    , emote_(emote)
    , gigantified_(gigantified)
    , isTwitchGif_(isTwitchGif)
    , horizontalImagePadding_(std::max<qreal>(0.0, horizontalImagePadding))
{
}

const QString &EmoteElement::getDefaultTooltip() const
{
    return this->emote_->tooltip.string;
}

EmotePtr EmoteElement::getEmote() const
{
    return this->emote_;
}

const ImagePtr &EmoteElement::getImageForTooltip() const
{
    return this->isTwitchGif_
               ? this->emote_->images.getImage(
                     getSettings()->highQualityTwitchGifs ? 2.F : 1.F,
                     ImageSet::ScaleMode::Exact)
               : this->emote_->images.getImage(3.F);
}

bool EmoteElement::isGigantified() const
{
    return this->gigantified_;
}

bool EmoteElement::isTwitchGif() const
{
    return this->isTwitchGif_;
}

void EmoteElement::setAnimatedPreview()
{
    this->setStaticPreview();
    this->preview_->animated = true;
}

void EmoteElement::setStaticPreview(bool enabled)
{
    if (enabled && !this->preview_)
    {
        this->preview_ = std::make_unique<PreviewState>();
    }
    if (this->preview_)
    {
        this->preview_->enabled = enabled;
    }
}

void EmoteElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasNone(this->getFlags()))
    {
        return;
    }

    const bool tracksMessageText =
        getSettings()->highlightMatchedFragments && ctx.showHighlights() &&
        ctx.message.highlightMatches &&
        this->getFlags().hasAny(MessageElementFlag::Emote,
                                MessageElementFlag::EmojiAll) &&
        !this->getFlags().has(MessageElementFlag::RepliedMessage);

    const bool renderGigantified =
        this->gigantified_ && getSettings()->enableGigantifyEmotes;
    const bool renderTwitchGif =
        this->isTwitchGif_ && getSettings()->enableTwitchGifs;
    const bool gifAsEmote =
        renderTwitchGif && getSettings()->twitchGifsAsEmotes;

    if (this->isTwitchGif_ && !getSettings()->enableTwitchGifs)
    {
        this->ensureText(false);
        auto textCtx = ctx;
        textCtx.flags = MessageElementFlag::Misc;
        textCtx.trackMessageText = tracksMessageText;
        this->textElement_->setTrailingSpace(this->hasTrailingSpace());
        this->textElement_->addToContainer(container, textCtx);
        ctx.messageTextCursor_ = textCtx.messageTextCursor_;
        return;
    }

    if (ctx.flags.has(MessageElementFlag::EmoteImage))
    {
        const auto imageScale =
            renderTwitchGif ? (getSettings()->highQualityTwitchGifs ? 2.F : 1.F)
            : renderGigantified ? 3.F
                                : container.getImageScale();
        const auto scaleMode = renderGigantified || renderTwitchGif
                                   ? ImageSet::ScaleMode::Exact
                                   : ImageSet::ScaleMode::Emote;
        ImagePtr image;
        if (this->preview_ && this->preview_->enabled)
        {
            auto &preview = *this->preview_;
            auto previewSource =
                this->emote_->images.getImage(imageScale, scaleMode);
            if (preview.source != previewSource || preview.image == nullptr)
            {
                preview.source = std::move(previewSource);
                preview.image = preview.animated
                                    ? preview.source->getEmotePickerImage(true)
                                    : preview.source->getFirstFramePreview();
                preview.animation = preview.animated
                                        ? preview.source->getEmotePickerImage()
                                        : preview.source->getPickerAnimation();
                preview.smoothAnimation =
                    preview.animated
                        ? preview.source->getEmotePickerImage(false, true)
                        : nullptr;
            }
            if (!preview.animated)
            {
                preview.image->load();
            }
            image = preview.animated && !preview.animation->isEmpty()
                        ? preview.animation
                        : preview.image;
        }
        else
        {
            image =
                this->emote_->images.getImageOrLoaded(imageScale, scaleMode);
        }

        if (image->isEmpty())
        {
            this->ensureText(true);
        }
        else
        {
            const auto messageTextRange =
                tracksMessageText
                    ? ctx.claimMessageTextRange(this->emote_->name.string)
                    : std::optional<MessageLayoutContext::MessageTextRange>{};
            QSizeF size;
            if (renderGigantified)
            {
                if (!container.atStartOfLine())
                {
                    container.breakLine();
                }

                const auto logicalSize = std::min<qreal>(
                    GIGANTIFIED_LOGICAL_SIZE * container.getScale(),
                    std::max<qreal>(1.0, container.remainingWidth()));
                size = QSizeF(logicalSize, logicalSize);
            }
            else if (gifAsEmote)
            {
                const auto height = TWITCH_GIF_EMOTE_HEIGHT *
                                    container.getEmoteScale() *
                                    getSettings()->emoteScale.getValue();
                const auto imageSize = image->size();
                const auto aspect = imageSize.height() > 0
                                        ? imageSize.width() / imageSize.height()
                                        : 1.0;
                size = QSizeF(height * aspect, height);
            }
            else if (renderTwitchGif)
            {
                if (!container.atStartOfLine())
                {
                    container.breakLine();
                }

                auto gifScale = static_cast<qreal>(
                    getSettings()->twitchGifScale.getValue());
                if (!std::isfinite(gifScale))
                {
                    gifScale = 0.75;
                }
                gifScale = std::clamp<qreal>(gifScale, 0.5, 2.0);
                const auto maxLogicalSize = std::min<qreal>(
                    TWITCH_GIF_LOGICAL_SIZE * container.getScale() * gifScale,
                    std::max<qreal>(1.0, container.remainingWidth()));
                const auto imgSize = image->size();
                if (imgSize.width() > 0 && imgSize.height() > 0)
                {
                    const auto aspect = static_cast<qreal>(imgSize.width()) /
                                        static_cast<qreal>(imgSize.height());
                    if (aspect >= 1.0)
                    {
                        size = QSizeF(maxLogicalSize, maxLogicalSize / aspect);
                    }
                    else
                    {
                        size = QSizeF(maxLogicalSize * aspect, maxLogicalSize);
                    }
                }
                else
                {
                    size = QSizeF(maxLogicalSize, maxLogicalSize);
                }
            }
            else
            {
                bool isBadge = this->getFlags().hasAny(MessageElementFlag::Badges);
                auto scale = isBadge ? container.getBadgeScale()
                                     : container.getEmoteScale();
                auto emoteScale = getSettings()->emoteScale.getValue();
                size = image->size() * scale * emoteScale;
            }

            auto *layoutElement = this->makeImageLayoutElement(
                this->preview_ && this->preview_->enabled &&
                        this->preview_->animated
                    ? this->preview_->image
                    : image,
                size);
            if (this->preview_ && this->preview_->enabled)
            {
                if (auto *imageElement =
                        dynamic_cast<ImageLayoutElement *>(layoutElement))
                {
                    if (this->preview_->animated)
                    {
                        imageElement->setPreviewImage(
                            this->preview_->animation);
                        imageElement->setHoverImage(
                            this->preview_->smoothAnimation);
                    }
                    else
                    {
                        imageElement->setHoverImage(this->preview_->animation);
                    }
                }
            }
            if (messageTextRange && ctx.message.highlightMatches)
            {
                const auto rangeStart = messageTextRange->start;
                const auto rangeEnd = rangeStart + messageTextRange->length;
                const auto found = std::ranges::find_if(
                    *ctx.message.highlightMatches, [&](const auto &match) {
                        return hasExactMatchMarker(match) &&
                               match.start < rangeEnd &&
                               match.start + match.length > rangeStart;
                    });
                if (found != ctx.message.highlightMatches->end())
                {
                    layoutElement->setFragmentHighlight(
                        {*found, getPaintForExactMatch(*found)});
                }
            }
            if (renderGigantified || (renderTwitchGif && !gifAsEmote))
            {
                layoutElement->setCompactEmoteLayout(false);
                container.addElementNoLineBreak(layoutElement);
            }
            else
            {
                container.addElement(layoutElement);
            }
            return;
        }
    }
    else
    {
        this->ensureText(false);
    }

    auto textCtx = ctx;
    textCtx.flags = MessageElementFlag::Misc;
    textCtx.trackMessageText = tracksMessageText;
    this->textElement_->addToContainer(container, textCtx);

    ctx.messageTextCursor_ = textCtx.messageTextCursor_;
}

MessageLayoutElement *EmoteElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    const auto sourceHeight = image->size().height();
    const auto paddingScale =
        sourceHeight > 0.0 ? size.height() / sourceHeight : 1.0;
    return new ImageLayoutElement(*this, image, size,
                                  this->horizontalImagePadding_ * paddingScale);
}

std::unique_ptr<MessageElement> EmoteElement::clone() const
{
    auto el = std::make_unique<EmoteElement>(
        this->emote_, this->getFlags(), this->textColor_, this->gigantified_,
        this->horizontalImagePadding_, this->isTwitchGif_);
    if (this->preview_)
    {
        el->preview_ = std::make_unique<PreviewState>(*this->preview_);
        if (el->preview_->animated)
        {
            el->preview_->source.reset();
            el->preview_->image.reset();
            el->preview_->animation.reset();
            el->preview_->smoothAnimation.reset();
        }
    }
    el->cloneFrom(*this);
    return el;
}

void EmoteElement::ensureText(bool asFallback)
{
    if (!this->textElement_ || asFallback != this->usingFallbackColor_)
    {
        const auto color = asFallback ? MessageColor::System : this->textColor_;
        this->textElement_ = std::make_unique<TextElement>(
            this->emote_->getCopyString(), MessageElementFlag::Misc, color);
        this->textElement_->setTrailingSpace(this->trailingSpace);
        this->usingFallbackColor_ = asFallback;
    }
    if (asFallback)
    {
        QString tooltip = this->emote_->tooltip.string;
        auto image = this->emote_->images.getImageOrLoaded(1.0);
        if (image && !image->failureReason().isEmpty())
        {
            tooltip += "<br><b>Failed to load:</b> " +
                       image->failureReason().toHtmlEscaped();
        }
        else
        {
            tooltip += "<br><i>(Failed to load image)</i>";
        }
        this->textElement_->setTooltip(tooltip);
    }
}

QJsonObject EmoteElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"EmoteElement"_s;
    base["emote"_L1] = this->emote_->toJson();
    if (this->textElement_)
    {
        base["text"_L1] = this->textElement_->toJson();
    }
    if (this->gigantified_)
    {
        base["gigantified"_L1] = true;
    }
    if (this->isTwitchGif_)
    {
        base["twitchGif"_L1] = true;
    }
    if (this->horizontalImagePadding_ > 0.0)
    {
        base["horizontalImagePadding"_L1] = this->horizontalImagePadding_;
    }

    return base;
}

std::string_view EmoteElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

LayeredEmoteElement::LayeredEmoteElement(
    std::vector<LayeredEmoteElement::Emote> &&emotes, MessageElementFlags flags,
    const MessageColor &textElementColor)
    : MessageElement(flags)
    , emotes_(std::move(emotes))
    , textElementColor_(textElementColor)
{
    this->updateTooltips();
}

void LayeredEmoteElement::addEmoteLayer(const LayeredEmoteElement::Emote &emote)
{
    this->emotes_.push_back(emote);
    if (this->modifierData_)
    {
        this->modifierData_->copyTokens.push_back(emote.ptr);
    }
    this->updateTooltips();
}

void LayeredEmoteElement::addModifier(const EmotePtr &modifier)
{
    if (modifier == nullptr ||
        modifier->modifierPlacement == EmoteModifierPlacement::None)
    {
        return;
    }
    if (!this->modifierData_)
    {
        this->modifierData_ = std::make_unique<ModifierData>();
        this->modifierData_->copyTokens.reserve(this->emotes_.size() + 1);
        for (const auto &emote : this->emotes_)
        {
            this->modifierData_->copyTokens.push_back(emote.ptr);
        }
    }

    this->modifierData_->modifiers.push_back(modifier);
    if (modifier->modifierPlacement == EmoteModifierPlacement::Prefix)
    {
        const auto firstBase = std::ranges::find_if(
            this->modifierData_->copyTokens, [](const auto &token) {
                return token->modifierPlacement !=
                       EmoteModifierPlacement::Prefix;
            });
        this->modifierData_->copyTokens.insert(firstBase, modifier);
    }
    else
    {
        this->modifierData_->copyTokens.push_back(modifier);
    }
    this->updateTooltips();
}

void LayeredEmoteElement::addToContainer(MessageLayoutContainer &container,
                                         const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        const bool tracksMessageText =
            getSettings()->highlightMatchedFragments && ctx.showHighlights() &&
            ctx.message.highlightMatches &&
            !this->getFlags().has(MessageElementFlag::RepliedMessage);
        if (ctx.flags.has(MessageElementFlag::EmoteImage))
        {
            auto images = this->getLoadedImages(container.getImageScale());
            struct Part {
                EmotePtr icon;
                QString copyText;
            };
            QVarLengthArray<Part, 1> parts{{nullptr, this->modifierData_
                                                         ? this->getCopyString()
                                                         : QString{}}};
            if (!images.empty() && this->modifierData_ &&
                std::ranges::any_of(
                    this->getModifiers(), [](const auto &emote) {
                        return !getSettings()->isEmoteModifierEnabled(
                            emote->name.string);
                    }))
            {
                parts.clear();
                QString prefix;
                bool addedBase = false;
                for (const auto &token : this->modifierData_->copyTokens)
                {
                    const bool disabled =
                        token->modifierPlacement !=
                            EmoteModifierPlacement::None &&
                        !getSettings()->isEmoteModifierEnabled(
                            token->name.string);
                    const bool isBase =
                        !addedBase && token == this->emotes_.front().ptr;
                    if (isBase || disabled)
                    {
                        parts.push_back({disabled ? token : nullptr, prefix});
                        prefix.clear();
                        addedBase |= isBase;
                    }
                    auto &copyText =
                        parts.empty() ? prefix : parts.back().copyText;
                    if (!copyText.isEmpty())
                    {
                        copyText += ' ';
                    }
                    copyText += token->getCopyString();
                }
            }
            if (!images.empty())
            {
                auto emoteScale = getSettings()->emoteScale.getValue();
                bool isBadge = this->getFlags().hasAny(MessageElementFlag::Badges);
                auto scale = isBadge ? container.getBadgeScale()
                                     : container.getEmoteScale();

                auto largestSize =
                    getBoundingBoxSize(images) * scale * emoteScale;
                std::vector<QSizeF> individualSizes;
                individualSizes.reserve(images.size());
                for (const auto &img : images)
                {
                    individualSizes.push_back(img->size() * scale * emoteScale);
                }

                for (qsizetype i = 0; i < parts.size(); ++i)
                {
                    const auto &part = parts[i];
                    MessageLayoutElement *layoutElement;
                    if (part.icon)
                    {
                        auto &data = *this->modifierData_;
                        data.icons.resize(data.modifiers.size());
                        const auto index = static_cast<size_t>(
                            std::ranges::find(data.modifiers, part.icon) -
                            data.modifiers.begin());
                        auto &icon = data.icons[index];
                        if (!icon)
                        {
                            icon = std::make_shared<EmoteElement>(
                                part.icon, this->getFlags(),
                                this->textElementColor_);
                        }
                        const auto image = part.icon->images.getImageOrLoaded(
                            container.getImageScale());
                        if (image->isEmpty())
                        {
                            icon->ensureText(true);
                            auto *text = icon->textElement_.get();
                            text->setText(part.copyText);
                            text->setTrailingSpace(i + 1 < parts.size() ||
                                                   this->hasTrailingSpace());
                            auto textCtx = ctx;
                            textCtx.flags = MessageElementFlag::Misc;
                            textCtx.trackMessageText = tracksMessageText;
                            text->addToContainer(container, textCtx);
                            ctx.messageTextCursor_ = textCtx.messageTextCursor_;
                            continue;
                        }
                        layoutElement = new ImageLayoutElement(
                            *icon, image, image->size() * scale * emoteScale);
                    }
                    else
                    {
                        layoutElement = this->makeImageLayoutElement(
                            images, individualSizes, largestSize);
                    }

                    if (this->modifierData_)
                    {
                        layoutElement->setText(
                            TwitchEmotes::cleanUpEmoteCode(part.copyText));
                    }
                    layoutElement->setTrailingSpace(i + 1 < parts.size() ||
                                                    this->hasTrailingSpace());
                    const auto messageTextRange =
                        tracksMessageText
                            ? ctx.claimMessageTextRange(
                                  part.copyText.isNull() ? this->getCopyString()
                                                         : part.copyText)
                            : std::optional<
                                  MessageLayoutContext::MessageTextRange>{};
                    if (messageTextRange && ctx.message.highlightMatches)
                    {
                        const auto rangeStart = messageTextRange->start;
                        const auto rangeEnd =
                            rangeStart + messageTextRange->length;
                        const auto found = std::ranges::find_if(
                            *ctx.message.highlightMatches,
                            [&](const auto &match) {
                                return hasExactMatchMarker(match) &&
                                       match.start < rangeEnd &&
                                       match.start + match.length > rangeStart;
                            });
                        if (found != ctx.message.highlightMatches->end())
                        {
                            layoutElement->setFragmentHighlight(
                                {*found, getPaintForExactMatch(*found)});
                        }
                    }
                    container.addElement(layoutElement);
                }
                return;
            }
        }

        if (this->textElement_)
        {
            this->textElement_->setTrailingSpace(this->hasTrailingSpace());
            auto textCtx = ctx;
            textCtx.flags = MessageElementFlag::Misc;
            textCtx.trackMessageText = tracksMessageText;
            this->textElement_->addToContainer(container, textCtx);
            ctx.messageTextCursor_ = textCtx.messageTextCursor_;
        }
    }
}

std::vector<ImagePtr> LayeredEmoteElement::getLoadedImages(float scale)
{
    std::vector<ImagePtr> res;
    res.reserve(this->emotes_.size());

    for (const auto &emote : this->emotes_)
    {
        auto image = emote.ptr->images.getImageOrLoaded(scale);
        if (image->isEmpty())
        {
            continue;
        }
        res.push_back(image);
    }
    return res;
}

MessageLayoutElement *LayeredEmoteElement::makeImageLayoutElement(
    const std::vector<ImagePtr> &images, const std::vector<QSizeF> &sizes,
    QSizeF largestSize)
{
    uint32_t flags = 0;
    for (const auto &modifier : this->getModifiers())
    {
        if (getSettings()->isEmoteModifierEnabled(modifier->name.string))
        {
            flags |= modifier->modifierFlags;
        }
    }
    return new LayeredImageLayoutElement(*this, images, sizes, largestSize,
                                         flags);
}

void LayeredEmoteElement::updateTooltips()
{
    if (!this->emotes_.empty())
    {
        QString copyStr = this->getCopyString();
        this->textElement_.reset(new TextElement(
            copyStr, MessageElementFlag::Misc, this->textElementColor_));
        this->setTooltip(copyStr);
    }

    std::vector<QString> result;
    result.reserve(this->emotes_.size());

    for (const auto &emote : this->emotes_)
    {
        result.push_back(emote.ptr->tooltip.string);
    }

    this->emoteTooltips_ = std::move(result);
}

const std::vector<QString> &LayeredEmoteElement::getEmoteTooltips() const
{
    return this->emoteTooltips_;
}

QString LayeredEmoteElement::getCleanCopyString() const
{
    QString result;
    bool first = true;
    const auto append = [&result, &first](const EmotePtr &emote) {
        if (!first)
        {
            result += ' ';
        }
        first = false;
        result += TwitchEmotes::cleanUpEmoteCode(emote->getCopyString());
    };
    if (this->modifierData_)
    {
        for (const auto &token : this->modifierData_->copyTokens)
        {
            append(token);
        }
    }
    else
    {
        for (const auto &emote : this->emotes_)
        {
            append(emote.ptr);
        }
    }
    return result;
}

QString LayeredEmoteElement::getCopyString() const
{
    QString result;
    bool first = true;
    const auto append = [&result, &first](const EmotePtr &emote) {
        if (!first)
        {
            result += ' ';
        }
        first = false;
        result += emote->getCopyString();
    };
    if (this->modifierData_)
    {
        for (const auto &token : this->modifierData_->copyTokens)
        {
            append(token);
        }
    }
    else
    {
        for (const auto &emote : this->emotes_)
        {
            append(emote.ptr);
        }
    }
    return result;
}

const std::vector<LayeredEmoteElement::Emote> &LayeredEmoteElement::getEmotes()
    const
{
    return this->emotes_;
}

const std::vector<EmotePtr> &LayeredEmoteElement::getModifiers() const
{
    static const std::vector<EmotePtr> noModifiers;
    return this->modifierData_ ? this->modifierData_->modifiers : noModifiers;
}

std::vector<LayeredEmoteElement::Emote> LayeredEmoteElement::getUniqueEmotes()
    const
{
    // Functor for std::copy_if that keeps track of seen elements
    struct NotDuplicate {
        bool operator()(const Emote &element)
        {
            return this->seen.insert(element.ptr).second;
        }

    private:
        std::set<EmotePtr> seen;
    };

    // Get unique emotes while maintaining relative layering order
    NotDuplicate dup;
    std::vector<Emote> unique;
    std::copy_if(this->emotes_.begin(), this->emotes_.end(),
                 std::back_insert_iterator(unique), dup);

    return unique;
}

const MessageColor &LayeredEmoteElement::textElementColor() const
{
    return this->textElementColor_;
}

std::unique_ptr<MessageElement> LayeredEmoteElement::clone() const
{
    auto emotes = this->getEmotes();
    auto el = std::make_unique<LayeredEmoteElement>(
        std::move(emotes), this->getFlags(), this->textElementColor());
    if (this->modifierData_)
    {
        el->modifierData_ =
            std::make_unique<ModifierData>(*this->modifierData_);
        el->modifierData_->icons.clear();
        el->updateTooltips();
    }
    el->cloneFrom(*this);
    return el;
}

QJsonObject LayeredEmoteElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"LayeredEmoteElement"_s;

    QJsonArray emotes;
    for (const auto &emote : this->emotes_)
    {
        emotes.append({{
            {"flags"_L1, qmagicenum::enumFlagsName(emote.flags.value())},
            {"emote"_L1, emote.ptr->toJson()},
        }});
    }
    base["emotes"_L1] = emotes;

    QJsonArray modifiers;
    for (const auto &modifier : this->getModifiers())
    {
        modifiers.append(modifier->toJson());
    }
    if (!modifiers.isEmpty())
    {
        base["modifiers"_L1] = modifiers;
    }

    QJsonArray tooltips;
    for (const auto &tooltip : this->emoteTooltips_)
    {
        tooltips.append(tooltip);
    }
    base["tooltips"_L1] = tooltips;

    if (this->textElement_)
    {
        base["text"_L1] = this->textElement_->toJson();
    }

    base["textElementColor"_L1] = this->textElementColor_.toString();

    return base;
}

std::string_view LayeredEmoteElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// BADGE
BadgeElement::BadgeElement(const EmotePtr &emote, MessageElementFlags flags)
    : MessageElement(flags)
    , emote_(emote)
{
}

const QString &BadgeElement::getDefaultTooltip() const
{
    return this->emote_->tooltip.string;
}

void BadgeElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    const auto elementFlags = this->getFlags();
    const auto rawFlags =
        static_cast<MessageElementFlags::Int>(elementFlags.value());
    constexpr auto HOMIES_SUPPORTER_FLAG =
        static_cast<MessageElementFlags::Int>(
            MessageElementFlag::BadgeHomiesSupporter);
    constexpr auto HOMIES_CUSTOM_FLAG =
        static_cast<MessageElementFlags::Int>(
            MessageElementFlag::BadgeHomiesCustom);

    const auto isHomiesSupporter =
        (rawFlags & HOMIES_SUPPORTER_FLAG) != 0;
    const auto isHomiesCustom = (rawFlags & HOMIES_CUSTOM_FLAG) != 0;
    const auto isHomiesBadge = isHomiesSupporter || isHomiesCustom;

    if (isHomiesBadge)
    {
        const auto *settings = getSettings();
        const auto enabled =
            ctx.flags.hasAny(elementFlags) &&
            ((isHomiesSupporter &&
              settings->showBadgesHomiesSupporter.getValue()) ||
             (isHomiesCustom &&
              settings->showBadgesHomiesCustom.getValue()));

        if (!enabled)
        {
            return;
        }
    }
    else if (!ctx.flags.hasAny(elementFlags))
    {
        return;
    }

    const auto baseScale = container.getScale();
    const auto badgeScale = container.getBadgeScale();
    const auto badgeImageScale =
        baseScale > 0.F ? container.getImageScale() * (badgeScale / baseScale)
                        : container.getImageScale();
    const auto image = isHomiesBadge
                           ? this->emote_->images.getImageOrLoadedNoLoad(
                                 badgeImageScale, ImageSet::ScaleMode::Exact)
                           : this->emote_->images.getImageOrLoaded(
                                 badgeImageScale, ImageSet::ScaleMode::Exact);
    if (image->isEmpty())
    {
        return;
    }

    container.addElement(
        this->makeImageLayoutElement(image, image->size() * badgeScale));
}

EmotePtr BadgeElement::getEmote() const
{
    return this->emote_;
}

void BadgeElement::setTwitchBadge(QString setID, QString version)
{
    this->twitchBadgeSetID_ = std::move(setID);
    this->twitchBadgeVersion_ = std::move(version);
}

std::optional<QString> BadgeElement::twitchBadgeSetID() const
{
    if (this->twitchBadgeSetID_.isEmpty())
    {
        return std::nullopt;
    }
    return this->twitchBadgeSetID_;
}

std::optional<QString> BadgeElement::twitchBadgeVersion() const
{
    if (this->twitchBadgeVersion_.isEmpty())
    {
        return std::nullopt;
    }
    return this->twitchBadgeVersion_;
}

MessageLayoutElement *BadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element = new ImageLayoutElement(*this, image, size);

    return element;
}

std::unique_ptr<MessageElement> BadgeElement::clone() const
{
    auto el = std::make_unique<BadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    if (!this->twitchBadgeSetID_.isEmpty())
    {
        el->setTwitchBadge(this->twitchBadgeSetID_, this->twitchBadgeVersion_);
    }
    return el;
}

QJsonObject BadgeElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"BadgeElement"_s;
    base["emote"_L1] = this->emote_->toJson();

    return base;
}

std::string_view BadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// MOD BADGE
ModBadgeElement::ModBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_)
    : BadgeElement(data, flags_)
{
}

MessageLayoutElement *ModBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    static const QColor modBadgeBackgroundColor("#34AE0A");

    auto *element = new ImageWithBackgroundLayoutElement(
        *this, image, size, modBadgeBackgroundColor);

    return element;
}

std::unique_ptr<MessageElement> ModBadgeElement::clone() const
{
    auto el = std::make_unique<ModBadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    if (!this->twitchBadgeSetID_.isEmpty())
    {
        el->setTwitchBadge(this->twitchBadgeSetID_, this->twitchBadgeVersion_);
    }
    return el;
}

QJsonObject ModBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"ModBadgeElement"_s;

    return base;
}

std::string_view ModBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// VIP BADGE
VipBadgeElement::VipBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_)
    : BadgeElement(data, flags_)
{
}

MessageLayoutElement *VipBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element = new ImageLayoutElement(*this, image, size);

    return element;
}

std::unique_ptr<MessageElement> VipBadgeElement::clone() const
{
    auto el = std::make_unique<VipBadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    if (!this->twitchBadgeSetID_.isEmpty())
    {
        el->setTwitchBadge(this->twitchBadgeSetID_, this->twitchBadgeVersion_);
    }
    return el;
}

QJsonObject VipBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"VipBadgeElement"_s;

    return base;
}

std::string_view VipBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// FFZ Badge
FfzBadgeElement::FfzBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_, QColor color_)
    : BadgeElement(data, flags_)
    , color(std::move(color_))
{
}

MessageLayoutElement *FfzBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element =
        new ImageWithBackgroundLayoutElement(*this, image, size, this->color);

    return element;
}

std::unique_ptr<MessageElement> FfzBadgeElement::clone() const
{
    auto el = std::make_unique<FfzBadgeElement>(this->emote_, this->getFlags(),
                                                this->color);
    el->cloneFrom(*this);
    return el;
}

QJsonObject FfzBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"FfzBadgeElement"_s;
    base["color"_L1] = this->color.name(QColor::HexArgb);

    return base;
}

std::string_view FfzBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

void ChannelAvatarSource::setAvatarUrl(QString avatarUrl)
{
    if (this->avatarUrl_ == avatarUrl)
    {
        return;
    }
    this->avatarUrl_ = std::move(avatarUrl);
    if (this->avatarRequested_)
    {
        queueChannelAvatarRelayout();
    }
}

void ChannelAvatarSource::setTwitchUserId(QString userId)
{
    if (this->twitchUserId_ == userId)
    {
        return;
    }
    this->twitchUserId_ = std::move(userId);
    this->twitchUser_.reset();
    this->avatarSignalHolder_.clear();
    if (this->avatarRequested_)
    {
        queueChannelAvatarRelayout();
    }
}

QString ChannelAvatarSource::currentAvatarUrl() const
{
    if (!this->twitchUser_ && !this->twitchUserId_.isEmpty())
    {
        auto *users = getApp()->getTwitchUsers();
        this->twitchUser_ = users->resolveID({this->twitchUserId_});
        if (auto *concreteUsers = dynamic_cast<TwitchUsers *>(users))
        {
            this->avatarSignalHolder_.managedConnect(
                concreteUsers->userUpdated, [this](const QString &userID) {
                    if (userID == this->twitchUserId_ && this->twitchUser_ &&
                        this->twitchUser_->profilePictureUrl !=
                            this->loadedAvatarUrl_)
                    {
                        queueChannelAvatarRelayout();
                    }
                });
        }
    }
    if (this->twitchUser_ && !this->twitchUser_->profilePictureUrl.isEmpty())
    {
        return this->twitchUser_->profilePictureUrl;
    }
    return this->avatarUrl_;
}

ImagePtr ChannelAvatarSource::image() const
{
    this->avatarRequested_ = true;
    const auto avatarUrl = this->currentAvatarUrl();
    if (avatarUrl != this->loadedAvatarUrl_)
    {
        this->loadedAvatarUrl_ = avatarUrl;
        this->avatarImage_ = avatarUrl.isEmpty()
                                 ? ImagePtr{}
                                 : Image::fromAutoscaledUrl({avatarUrl}, 18);
    }
    return this->avatarImage_;
}

ChannelNameElement::ChannelNameElement(
    const QString &text, std::shared_ptr<ChannelAvatarSource> avatarSource)
    : TextElement(text, MessageElementFlag::ChannelName, MessageColor::System)
    , avatarSource_(std::move(avatarSource))
{
}

void ChannelNameElement::addToContainer(MessageLayoutContainer &container,
                                        const MessageLayoutContext &ctx)
{
    if (!ctx.flags.has(MessageElementFlag::ChannelAvatar))
    {
        TextElement::addToContainer(container, ctx);
        return;
    }

    const auto avatarImage =
        this->avatarSource_ ? this->avatarSource_->image() : ImagePtr{};
    if (avatarImage && !avatarImage->isEmpty())
    {
        const auto size = QSizeF{18, 18} * container.getBadgeScale();
        auto *element = new ImageLayoutElement(*this, avatarImage, size);
        element->setLink(this->getLink());
        element->setTrailingSpace(this->hasTrailingSpace());
        container.addElement(element);
        return;
    }

    auto fallbackContext = ctx;
    fallbackContext.flags.set(MessageElementFlag::ChannelName);
    TextElement::addToContainer(container, fallbackContext);
}

std::unique_ptr<MessageElement> ChannelNameElement::clone() const
{
    auto element =
        std::make_unique<ChannelNameElement>(this->text_, this->avatarSource_);
    element->cloneFrom(*this);
    return element;
}

AutoModActionElement::AutoModActionElement(AutoModActionKind kind,
                                           int timeoutSeconds,
                                           MessageElementFlags flags)
    : MessageElement(flags)
    , kind_(kind)
    , timeoutSeconds_(timeoutSeconds)
{
}

void AutoModActionElement::addToContainer(MessageLayoutContainer &container,
                                          const MessageLayoutContext &ctx)
{
    if (!ctx.flags.hasAny(this->getFlags()))
    {
        return;
    }

    const QSizeF size{container.getScale() * 16, container.getScale() * 16};
    MessageLayoutElement *element = nullptr;
    if (this->kind_ == AutoModActionKind::Ban)
    {
        const ModerationAction action(u"/ban {user.name}"_s);
        if (const auto &image = action.getImage())
        {
            element = new ImageLayoutElement(*this, *image, size);
        }
    }
    else if (this->kind_ == AutoModActionKind::Timeout)
    {
        const ModerationAction action(
            u"/timeout {user.name} %1"_s.arg(this->timeoutSeconds_));
        element = new TextIconLayoutElement(*this, action.getLine1(),
                                            action.getLine2(),
                                            container.getScale(), size);
    }
    if (element == nullptr)
    {
        return;
    }
    element->setLink(this->getLink());
    element->setTrailingSpace(this->hasTrailingSpace());
    container.addElement(element);
}

std::unique_ptr<MessageElement> AutoModActionElement::clone() const
{
    auto element = std::make_unique<AutoModActionElement>(
        this->kind_, this->timeoutSeconds_, this->getFlags());
    element->cloneFrom(*this);
    return element;
}

QJsonObject AutoModActionElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"] = QStringLiteral("AutoModActionElement");
    base["kind"] = static_cast<int>(this->kind_);
    base["timeoutSeconds"] = this->timeoutSeconds_;
    return base;
}

std::string_view AutoModActionElement::type() const
{
    return TYPE;
}

AutoModActionKind AutoModActionElement::kind() const
{
    return this->kind_;
}

int AutoModActionElement::timeoutSeconds() const
{
    return this->timeoutSeconds_;
}

// TEXT
TextElement::TextElement(const QString &text, MessageElementFlags flags,
                         const MessageColor &color, FontStyle style)
    : MessageElement(flags)
    , text_(text)
    , hasWords_(true)
    , color_(color)
    , style_(style)
{
    // fourtf: add logic to store multiple spaces after message
}

TextElement::TextElement(QStringList &&words, MessageElementFlags flags,
                         const MessageColor &color, FontStyle style)
    : MessageElement(flags)
    , color_(color)
    , style_(style)
{
    this->setWords(std::move(words));
}

void TextElement::addToContainer(MessageLayoutContainer &container,
                                 const MessageLayoutContext &ctx)
{
    const auto words = this->words();
    this->addWordsToContainer(words, container, ctx);
}

void TextElement::addWordsToContainer(const QStringList &words,
                                      MessageLayoutContainer &container,
                                      const MessageLayoutContext &ctx,
                                      bool spaceBetweenWords)
{
    auto *app = getApp();

    if (this->getFlags().has(MessageElementFlag::ChannelName) &&
        ctx.flags.hasAny(MessageElementFlag::PlatformBadgeAlways,
                         MessageElementFlag::PlatformBadgeIfUnselected) &&
        ctx.selectedChannel != nullptr)
    {
        EmotePtr emote;
        if (!ctx.flags.has(MessageElementFlag::PlatformBadgeIfUnselected) ||
            ctx.message.platform != ctx.selectedChannel->messagePlatform())
        {
            switch (ctx.message.platform)
            {
                case MessagePlatform::AnyOrTwitch:
                    emote = getTwitchBadge();
                    break;
                case MessagePlatform::Kick:
                    emote = getKickBadge();
                    break;
                case MessagePlatform::YouTube:
                    emote = getYouTubeBadge();
                    break;
                case MessagePlatform::TikTok:
                    emote = getTikTokBadge();
                    break;
            }
        }

        if (emote)
        {
            auto image =
                emote->images.getImageOrLoaded(container.getImageScale());
            if (image->isEmpty())
            {
                return;
            }
            auto *el = new ImageLayoutElement(
                *this, image, image->size() * container.getScale());
            el->setLink(Link{});
            container.addElement(el);
        }
    }

    if (ctx.flags.hasAny(this->getFlags()))
    {
        if (this->getFlags().has(MessageElementFlag::RepeatedMessageCounter))
        {
            container.ensureSingleSpaceBeforeNextElement();
        }

        const auto effectiveStyle =
            this->getFlags().has(MessageElementFlag::Username)
                ? FontStyle::ChatUsername
                : this->style_;
        auto metrics = app->getFonts()->getFontMetrics(effectiveStyle,
                                                       container.getScale());
#ifdef Q_OS_WIN
        const bool measureUsernameWithLayout = false;
#else
        const bool measureUsernameWithLayout =
            this->getFlags().has(MessageElementFlag::Username);
#endif
        const auto usernameFont = measureUsernameWithLayout
                                      ? app->getFonts()->getFont(
                                            effectiveStyle,
                                            container.getScale())
                                      : QFont{};
        const QFont *layoutFont =
            measureUsernameWithLayout ? &usernameFont : nullptr;

        const auto tracksMessageText =
            getSettings()->highlightMatchedFragments &&
            ctx.showHighlights() && ctx.message.highlightMatches &&
            (this->getFlags().has(MessageElementFlag::Text) ||
             ctx.trackMessageText) &&
            !this->getFlags().has(MessageElementFlag::IgnoreExactMatch) &&
            !this->getFlags().has(MessageElementFlag::RepliedMessage);

        for (qsizetype wordIndex = 0; wordIndex < words.size(); ++wordIndex)
        {
            const auto &word = words[wordIndex];
            const bool trailingSpace =
                this->hasTrailingSpace() ||
                (spaceBetweenWords && wordIndex + 1 < words.size());
            auto wordId = container.nextWordId();
            const auto messageTextRange =
                tracksMessageText
                    ? ctx.claimMessageTextRange(word)
                    : std::optional<MessageLayoutContext::MessageTextRange>{};
            qsizetype wordSearchFrom = 0;

            auto getTextLayoutElement = [&](QString text, QSizeF size,
                                            bool hasTrailingSpace) {
                auto color = this->color_.getColor(ctx.messageColors);
                app->getThemes()->normalizeColor(color);

                auto *e = new TextLayoutElement(
                    *this, text, size, color,
                    effectiveStyle, this->color_.type(), container.getScale(),
                    container.getImageScale() / container.getScale());
                e->setTrailingSpace(hasTrailingSpace);
                e->setText(text);
                e->setWordId(wordId);

                if (messageTextRange && ctx.message.highlightMatches)
                {
                    auto wordOffset =
                        word.indexOf(text, wordSearchFrom, Qt::CaseSensitive);
                    if (wordOffset < 0)
                    {
                        wordOffset = word.indexOf(text, wordSearchFrom,
                                                  Qt::CaseInsensitive);
                    }
                    if (wordOffset < 0)
                    {
                        return e;
                    }
                    wordSearchFrom = wordOffset + text.size();
                    const auto pieceStart =
                        messageTextRange->start + wordOffset;
                    const auto pieceEnd = pieceStart + text.size();
                    std::vector<FragmentHighlight> localHighlights;
                    for (const auto &match : *ctx.message.highlightMatches)
                    {
                        if (!hasExactTextAppearance(match))
                        {
                            continue;
                        }
                        const auto overlapStart =
                            std::max(pieceStart, match.start);
                        const auto overlapEnd =
                            std::min(pieceEnd, match.start + match.length);
                        if (overlapEnd <= overlapStart)
                        {
                            continue;
                        }
                        auto local = match;
                        local.start = overlapStart - pieceStart;
                        local.length = overlapEnd - overlapStart;
                        const bool canConnectAcrossWords =
                            !ctx.message.messageText.isRightToLeft();
                        localHighlights.emplace_back(FragmentHighlight{
                            std::move(local),
                            getPaintForExactMatch(match),
                            canConnectAcrossWords && match.start < pieceStart,
                            canConnectAcrossWords &&
                                match.start + match.length > pieceEnd,
                        });
                    }
                    e->setFragmentHighlights(std::move(localHighlights));
                }

                return e;
            };

            auto size = textElementSize(word, metrics, layoutFont);
            auto width = size.width();

            // see if the text fits in the current line
            if (container.fitsInLine(width))
            {
                container.addElementNoLineBreak(
                    getTextLayoutElement(word, size, trailingSpace));
                continue;
            }

            // see if the text fits in the next line
            if (!container.atStartOfLine())
            {
                container.breakLine();

                if (container.fitsInLine(width))
                {
                    container.addElementNoLineBreak(
                        getTextLayoutElement(word, size, trailingSpace));
                    continue;
                }
            }

            // We done goofed, we need to wrap the text.
            // If we allow the use of private Qt APIs, we can use Qt's text
            // engine to accurately calculate the width of the text. Otherwise,
            // we have to fall back to using horizontalAdvance which has some
            // corner cases when processing whole words (see #5944).
#ifdef CHATTERINO_WITH_PRIVATE_QT_API
            auto font =
                app->getFonts()->getFont(effectiveStyle, container.getScale());

            // This code is similar to the one from QTextEngine::elidedText in
            // the mode Qt::ElideRight (because that's essentially what we're
            // doing here): https://github.com/qt/qtbase/blob/560bf5a07720eaa8cc589f424743db8ed1f1d902/src/gui/text/qtextengine.cpp#L3145
            // A difference is that, once we detected EOL, we start again.

            // The start of the current line in `word`
            qsizetype actualStart = 0;
            // This is treated like a view (from `actualStart`) over the word.
            // It's a QString because QStackTextEngine doesn't support
            // QStringViews as arguments.
            QString view = word;

            // This is essentially a loop over every line of text.
            do
            {
                QStackTextEngine engine(view, font);
                engine.validate();  // initialize the internal state

                int pos = 0;
                int nextBreak = 0;
                QFixed currentWidth = 0;
                int to = static_cast<int>(view.size());
                bool needsBreak = false;

                // Find the next grapheme boundary (`nextBreak`) at which we
                // need to break because the text wouldn't fit into the
                // container anymore.
                do
                {
                    pos = nextBreak;

                    ++nextBreak;
                    while (nextBreak < engine.layoutData->string.size() &&
                           !engine.attributes()[nextBreak].graphemeBoundary)
                    {
                        ++nextBreak;
                    }

                    auto nextWidth =
                        currentWidth + engine.width(pos, nextBreak - pos);
                    if (!container.fitsInLine(nextWidth.toReal()))
                    {
                        needsBreak = true;
                        if (pos == 0)
                        {
                            // Make sure that we consume at least one glyph.
                            // So this element will overflow
                            currentWidth = nextWidth;
                        }
                        else
                        {
                            // We didn't consume the glyph, it's for the next line
                            nextBreak = pos;
                        }
                        break;
                    }
                    currentWidth = nextWidth;
                } while (nextBreak < to);
                // Now we either processed the whole text or we need to break
                auto currentText = word.sliced(actualStart, nextBreak);
                auto currentSize =
                    textElementSize(currentText, metrics, layoutFont);
                if (layoutFont != nullptr)
                {
                    currentSize.setWidth(
                        std::max(currentSize.width(),
                                 std::ceil(currentWidth.toReal())));
                }
                else
                {
                    currentSize.setWidth(currentWidth.toReal());
                }
                container.addElementNoLineBreak(getTextLayoutElement(
                    currentText, currentSize, !needsBreak && trailingSpace));
                if (needsBreak)
                {
                    container.breakLine();
                }

                actualStart += nextBreak;
                // Update the view
                view = QString::fromRawData(word.constData() + actualStart,
                                            word.size() - actualStart);
                assert(needsBreak || view.isEmpty());
            } while (!view.isEmpty());
#else
            auto textLength = word.length();
            int wordStart = 0;
            width = 0;

            for (int i = 0; i < textLength; i++)
            {
                auto isSurrogate = word.size() > i + 1 &&
                                   QChar::isHighSurrogate(word[i].unicode());

                auto charWidth = isSurrogate
                                     ? metrics.horizontalAdvance(word.mid(i, 2))
                                     : metrics.horizontalAdvance(word[i]);

                if (!container.fitsInLine(width + charWidth))
                {
                    auto currentText = word.mid(wordStart, i - wordStart);
                    auto currentSize =
                        textElementSize(currentText, metrics, layoutFont);
                    if (layoutFont != nullptr)
                    {
                        currentSize.setWidth(
                            std::max(currentSize.width(), std::ceil(width)));
                    }
                    else
                    {
                        currentSize.setWidth(width);
                    }
                    container.addElementNoLineBreak(getTextLayoutElement(
                        currentText, currentSize, false));
                    container.breakLine();

                    wordStart = i;
                    width = charWidth;

                    if (isSurrogate)
                    {
                        i++;
                    }
                    continue;
                }

                width += charWidth;

                if (isSurrogate)
                {
                    i++;
                }
            }
            //add the final piece of wrapped text
            auto currentText = word.mid(wordStart);
            auto currentSize = textElementSize(currentText, metrics, layoutFont);
            if (layoutFont != nullptr)
            {
                currentSize.setWidth(
                    std::max(currentSize.width(), std::ceil(width)));
            }
            else
            {
                currentSize.setWidth(width);
            }
            container.addElementNoLineBreak(
                getTextLayoutElement(currentText, currentSize, trailingSpace));
#endif
        }
    }
}

std::unique_ptr<MessageElement> TextElement::clone() const
{
    auto el = std::make_unique<TextElement>(QString(), this->getFlags(),
                                            this->color_, this->style_);
    el->text_ = this->text_;
    el->hasWords_ = this->hasWords_;
    el->hasExplicitWordBoundaries_ = this->hasExplicitWordBoundaries_;
    if (this->wordsWithNulls_)
    {
        el->wordsWithNulls_ =
            std::make_unique<QStringList>(*this->wordsWithNulls_);
    }
    el->cloneFrom(*this);
    return el;
}

void TextElement::shareTextStorageWith(const QString &text)
{
    if (text.capacity() > 0 && this->text_.constData() != text.constData() &&
        this->text_.isNull() == text.isNull() && this->text_ == text)
    {
        this->text_ = text;
    }
}

QStringList TextElement::words() const
{
    if (this->wordsWithNulls_)
    {
        return *this->wordsWithNulls_;
    }
    if (!this->hasWords_)
    {
        return {};
    }
    if (this->hasExplicitWordBoundaries_)
    {
        return this->text_.split(QChar::Null, Qt::KeepEmptyParts);
    }
    return this->text_.split(u' ', Qt::KeepEmptyParts);
}

void TextElement::setWords(QStringList words)
{
    this->hasWords_ = !words.isEmpty();
    this->hasExplicitWordBoundaries_ = true;
    this->text_ = words.join(QChar::Null);
    this->wordsWithNulls_.reset();
    if (std::ranges::any_of(words, [](const auto &word) {
            return word.contains(QChar::Null);
        }))
    {
        this->wordsWithNulls_ =
            std::make_unique<QStringList>(std::move(words));
    }
}

void TextElement::setText(QString text)
{
    this->wordsWithNulls_.reset();
    this->text_ = std::move(text);
    this->hasWords_ = true;
    this->hasExplicitWordBoundaries_ = false;
}

const MessageColor &TextElement::color() const noexcept
{
    return this->color_;
}

FontStyle TextElement::fontStyle() const noexcept
{
    return this->style_;
}

void TextElement::appendText(QStringView text)
{
    if (this->wordsWithNulls_ ||
        (this->hasExplicitWordBoundaries_ && text.contains(QChar::Null)))
    {
        auto words = this->words();
        for (auto word : text.split(u' '))
        {
            words.append(word.toString());
        }
        this->setWords(std::move(words));
        return;
    }
    if (this->hasExplicitWordBoundaries_)
    {
        if (this->hasWords_)
        {
            this->text_.append(QChar::Null);
        }
        for (const auto character : text)
        {
            this->text_.append(character == u' ' ? QChar::Null : character);
        }
        this->hasWords_ = true;
        return;
    }

    if (this->hasWords_)
    {
        this->text_.append(u' ');
    }
    this->text_.append(text);
    this->hasWords_ = true;
}

void TextElement::appendText(const QString &text)
{
    if (this->hasExplicitWordBoundaries_)
    {
        this->appendText(QStringView{text});
        return;
    }

    if (this->hasWords_)
    {
        this->text_.append(u' ');
    }
    this->text_.append(text);
    this->hasWords_ = true;
}

QJsonObject TextElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"TextElement"_s;
    base["words"_L1] = QJsonArray::fromStringList(this->words());
    base["color"_L1] = this->color_.toString();
    base["style"_L1] = qmagicenum::enumNameString(this->style_);

    return base;
}

std::string_view TextElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

PronounElement::PronounElement(QString username)
    : TextElement(QString(), MessageElementFlag::Pronouns, MessageColor::System,
                  FontStyle::ChatMedium)
    , username_(std::move(username).trimmed().toLower())
{
    this->setLink({Link::Url, u"https://pr.alejo.io/"_s});
}

void PronounElement::addToContainer(MessageLayoutContainer &container,
                                    const MessageLayoutContext &ctx)
{
    if (!ctx.flags.hasAny(this->getFlags()))
    {
        return;
    }

    const auto pronouns =
        getApp()->getPronouns()->getCachedUserPronoun(this->username_);
    if (!pronouns.has_value() || pronouns->isUnspecified())
    {
        this->setWords({});
        return;
    }

    const auto formatted = pronouns->format();
    this->setText(u"(%1)"_s.arg(formatted));
    this->setTooltip(u"Pronouns: %1\nProvided by pr.alejo.io"_s.arg(formatted));
    TextElement::addToContainer(container, ctx);
}

std::unique_ptr<MessageElement> PronounElement::clone() const
{
    auto element = std::make_unique<PronounElement>(this->username_);
    element->cloneFrom(*this);
    return element;
}

QJsonObject PronounElement::toJson() const
{
    auto base = TextElement::toJson();
    base["type"_L1] = u"PronounElement"_s;
    base["username"_L1] = this->username_;
    return base;
}

std::string_view PronounElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

SingleLineTextElement::SingleLineTextElement(const QString &text,
                                             MessageElementFlags flags,
                                             const MessageColor &color,
                                             FontStyle style)
    : MessageElement(flags)
    , color_(color)
    , style_(style)
    , words_(text.split(' '))
{
}

void SingleLineTextElement::addToContainer(MessageLayoutContainer &container,
                                           const MessageLayoutContext &ctx)
{
    auto *app = getApp();

    if (ctx.flags.hasAny(this->getFlags()))
    {
        auto metrics =
            app->getFonts()->getFontMetrics(this->style_, container.getScale());

        auto getTextLayoutElement = [&](QString text, qreal width,
                                        bool hasTrailingSpace) {
            auto color = this->color_.getColor(ctx.messageColors);
            app->getThemes()->normalizeColor(color);

            auto *e = new TextLayoutElement(
                *this, text, QSizeF(width, metrics.height()), color,
                this->style_, this->color_.type(), container.getScale());
            e->setTrailingSpace(hasTrailingSpace);
            e->setText(text);

            return e;
        };

        static const auto ellipsis = QStringLiteral("…");

        // String to continuously append words onto until we place it in the container
        // once we encounter an emote or reach the end of the message text. */
        QString currentText;

        bool firstIteration = true;
        for (const auto &word : this->words_)
        {
            if (firstIteration)
            {
                firstIteration = false;
            }
            else
            {
                currentText += ' ';
            }

            bool done = false;
            for (const auto &parsedWord :
                 app->getEmotes()->getEmojis()->parse(word))
            {
                if (std::holds_alternative<QStringView>(parsedWord))
                {
                    currentText += std::get<QStringView>(parsedWord);
                    QString prev =
                        currentText;  // only increments the ref-count
                    currentText =
                        metrics.elidedText(currentText, Qt::ElideRight,
                                           container.remainingWidth());
                    if (currentText != prev)
                    {
                        done = true;
                        break;
                    }
                }
                else if (std::holds_alternative<EmotePtr>(parsedWord))
                {
                    auto emote = std::get<EmotePtr>(parsedWord);
                    auto image =
                        emote->images.getImageOrLoaded(container.getScale());
                    if (!image->isEmpty())
                    {
                        auto emoteScale = getSettings()->emoteScale.getValue();

                        auto currentWidth =
                            metrics.horizontalAdvance(currentText);
                        auto emoteSize =
                            image->size() * emoteScale * container.getScale();

                        if (!container.fitsInLine(currentWidth +
                                                  emoteSize.width()))
                        {
                            currentText += ellipsis;
                            done = true;
                            break;
                        }

                        // Add currently pending text to container, then add the emote after.
                        container.addElementNoLineBreak(getTextLayoutElement(
                            currentText, currentWidth, false));
                        currentText.clear();

                        container.addElementNoLineBreak(
                            (new ImageLayoutElement(*this, image, emoteSize))
                                ->setLink(this->getLink())
                                ->setTrailingSpace(false));
                    }
                }
            }

            if (done)
            {
                break;
            }
        }

        // Add the last of the pending message text to the container.
        if (!currentText.isEmpty())
        {
            auto width = metrics.horizontalAdvance(currentText);
            container.addElementNoLineBreak(
                getTextLayoutElement(currentText, width, false));
        }

        container.breakLine();
    }
}

std::unique_ptr<MessageElement> SingleLineTextElement::clone() const
{
    auto el = std::make_unique<SingleLineTextElement>(
        QString(), this->getFlags(), this->color_, this->style_);
    el->words_ = this->words_;
    el->cloneFrom(*this);
    return el;
}

QJsonObject SingleLineTextElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"SingleLineTextElement"_s;
    QJsonArray words = QJsonArray::fromStringList(this->words_);
    base["words"_L1] = words;
    base["color"_L1] = this->color_.toString();
    base["style"_L1] = qmagicenum::enumNameString(this->style_);

    return base;
}

std::string_view SingleLineTextElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

LinkElement::LinkElement(const Parsed &parsed, const QString &fullUrl,
                         MessageElementFlags flags, const MessageColor &color,
                         FontStyle style)
    : TextElement(QStringList(), flags, color, style)
    , linkInfo_(fullUrl)
    , lowercase_(parsed.lowercase)
    , original_(parsed.original)
{
    this->setTooltip(parsed.original);
}

void LinkElement::addToContainer(MessageLayoutContainer &container,
                                 const MessageLayoutContext &ctx)
{
    const auto &source =
        getSettings()->lowercaseDomains ? this->lowercase_ : this->original_;

    // Split the URL into segments at natural break points so long URLs
    // can wrap mid-link instead of being treated as a single unbreakable word.
    // Break points: before '/', '?', '&', '#', '='  (keep delimiter at start
    // of next segment so the visual break is clean).
    QStringList segments;
    QString current;
    const auto &url = source;
    // Skip the protocol prefix (e.g. "https://") so we don't break there
    int start = 0;
    int protocolEnd = url.indexOf("://");
    if (protocolEnd != -1)
    {
        start = protocolEnd + 3;  // past "://"
        current = url.left(start);
    }

    for (int i = start; i < url.size(); i++)
    {
        QChar ch = url[i];
        // Break BEFORE these characters (except at the very start)
        if (!current.isEmpty() && current.size() > 1 &&
            (ch == '/' || ch == '?' || ch == '&' || ch == '#' ||
             ch == '='))
        {
            segments.append(current);
            current.clear();
        }
        current += ch;
    }
    if (!current.isEmpty())
    {
        segments.append(current);
    }

    // Temporarily disable trailing space so URL segments render contiguously
    // (no visible gap between "example.com" and "/path").
    bool originalTrailingSpace = this->trailingSpace;
    this->trailingSpace = false;

    // Lay out all segments except the last with no trailing space
    if (!segments.isEmpty())
    {
        QStringList allButLast = segments.mid(0, segments.size() - 1);
        QString lastWord = segments.last();

        this->addWordsToContainer(allButLast, container, ctx, false);

        // Lay out the last segment with the original trailing space setting
        this->trailingSpace = originalTrailingSpace;
        this->addWordsToContainer({lastWord}, container, ctx);
    }

    this->setText(source);
    this->trailingSpace = originalTrailingSpace;
}

QStringList LinkElement::words() const
{
    return this->hasWords_ ? QStringList{this->text_} : QStringList{};
}

Link LinkElement::getLink() const
{
    return {Link::Url, this->linkInfo_.url()};
}

std::unique_ptr<MessageElement> LinkElement::clone() const
{
    auto el = std::make_unique<LinkElement>(
        Parsed{
            .lowercase = this->lowercase_,
            .original = this->original_,
        },
        this->linkInfo_.originalUrl(), this->getFlags(), this->color(),
        this->fontStyle());
    el->cloneFrom(*this);
    return el;
}

QJsonObject LinkElement::toJson() const
{
    auto base = TextElement::toJson();
    base["type"_L1] = u"LinkElement"_s;
    base["link"_L1] = this->linkInfo_.originalUrl();
    base["lowercase"_L1] = QJsonArray::fromStringList(this->lowercase());
    base["original"_L1] = QJsonArray::fromStringList(this->original());

    return base;
}

std::string_view LinkElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

QStringList LinkElement::lowercase() const
{
    return {this->lowercase_};
}

QStringList LinkElement::original() const
{
    return {this->original_};
}

MentionElement::MentionElement(const QString &displayName, QString loginName_,
                               MessageColor fallbackColor_,
                               MessageColor userColor_)
    : TextElement(displayName,
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_)
    , userLoginName_(std::move(loginName_))
{
}

MentionElement::MentionElement(QStringList &&words, MessageColor fallbackColor_,
                               MessageColor userColor_)
    : TextElement(std::move(words),
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_)
{
}

template <typename>
MentionElement::MentionElement(const QString &displayName, QString loginName_,
                               MessageColor fallbackColor_, QColor userColor_)
    : TextElement(displayName,
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_.isValid() ? userColor_ : fallbackColor_)
    , userLoginName_(std::move(loginName_))
{
}

template MentionElement::MentionElement(const QString &displayName,
                                        QString loginName_,
                                        MessageColor fallbackColor_,
                                        QColor userColor_);

void MentionElement::addToContainer(MessageLayoutContainer &container,
                                    const MessageLayoutContext &ctx)
{
    if (getSettings()->colorUsernames)
    {
        this->color_ = this->userColor_;
    }
    else
    {
        this->color_ = this->fallbackColor_;
    }

    if (getSettings()->boldUsernames)
    {
        this->style_ = FontStyle::ChatMediumBold;
    }
    else
    {
        this->style_ = FontStyle::ChatMedium;
    }

    TextElement::addToContainer(container, ctx);
}

std::unique_ptr<MessageElement> MentionElement::clone() const
{
    std::unique_ptr<MentionElement> el{new MentionElement(
        this->words(), this->fallbackColor_, this->userColor_)};
    el->userLoginName_ = this->userLoginName_;
    el->cloneFrom(*this);
    return el;
}

MessageElement *MentionElement::setLink(const Link &link)
{
    assert(false && "MentionElement::setLink should not be called. Pass "
                    "through a valid login name in the constructor and it will "
                    "automatically be a UserInfo link");

    return TextElement::setLink(link);
}

Link MentionElement::getLink() const
{
    if (this->userLoginName_.isEmpty())
    {
        // Some rare mention elements don't have the knowledge of the login name
        return {};
    }

    return {Link::UserInfo, this->userLoginName_};
}

QJsonObject MentionElement::toJson() const
{
    auto base = TextElement::toJson();
    base["type"_L1] = u"MentionElement"_s;
    base["fallbackColor"_L1] = this->fallbackColor_.toString();
    base["userColor"_L1] = this->userColor_.toString();
    base["userLoginName"_L1] = this->userLoginName_;

    return base;
}

std::string_view MentionElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// TIMESTAMP
TimestampElement::TimestampElement()
    : TimestampElement(getApp()->isTest() ? QTime::fromMSecsSinceStartOfDay(0)
                                          : QTime::currentTime())
{
}

TimestampElement::TimestampElement(QTime time)
    : TextElement(TimestampElement::formatTime(time),
                  MessageElementFlag::Timestamp, MessageColor::Timestamp,
                  FontStyle::TimestampMedium)
    , time_(time)
{
}

void TimestampElement::addToContainer(MessageLayoutContainer &container,
                                      const MessageLayoutContext &ctx)
{
    if (!ctx.flags.has(MessageElementFlag::Timestamp))
    {
        return;
    }

    if (getSettings()->timestampFormat != this->format_)
    {
        this->format_ = getSettings()->timestampFormat.getValue();
        this->setText(this->formatTime(this->time_));
    }

    TextElement::addToContainer(container, ctx);
}

QString TimestampElement::formatTime(const QTime &time)
{
    static QLocale locale("en_US");
    QString format = locale.toString(time, getSettings()->timestampFormat);
    if (format.isEmpty() || format.size() > 128)
    {
        return format;
    }

    static std::mutex mutex;
    static QCache<QString, QString> strings(512);
    std::lock_guard lock(mutex);
    if (const auto *shared = strings.object(format))
    {
        return *shared;
    }
    strings.insert(format, new QString(format));
    return format;
}

std::unique_ptr<MessageElement> TimestampElement::clone() const
{
    auto el = std::make_unique<TimestampElement>(this->time_);
    el->cloneFrom(*this);
    return el;
}

QJsonObject TimestampElement::toJson() const
{
    auto base = MessageElement::toJson();
    auto element = TextElement::toJson();

    element["flags"_L1] = u"Timestamp"_s;
    element["tooltip"_L1] =
        this->format_.isEmpty() ? QString{} : this->getTooltip();
    element["trailingSpace"_L1] = true;

    base["type"_L1] = u"TimestampElement"_s;
    base["time"_L1] = this->time_.toString(Qt::ISODate);
    base["element"_L1] = element;
    base["format"_L1] = this->format_;

    return base;
}

std::string_view TimestampElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// TWITCH MODERATION
namespace {

int normalizeInlineActionMode(int mode)
{
    if (mode <= 0)
    {
        return 0;
    }
    if (mode >= 2)
    {
        return 2;
    }
    return 1;
}

void addModerationActionToContainer(MessageElement &source,
                                    const ModerationAction &action,
                                    MessageLayoutContainer &container,
                                    const QSizeF &size)
{
    if (const auto &image = action.getImage())
    {
        container.addElement(
            (new ImageLayoutElement(source, *image, size))
                ->setLink(Link(Link::UserAction, action.getAction())));
    }
    else
    {
        container.addElement(
            (new TextIconLayoutElement(source, action.getLine1(),
                                       action.getLine2(),
                                       container.getScale(), size))
                ->setLink(Link(Link::UserAction, action.getAction())));
    }
}

std::optional<ModerationAction> makeYouTubeModerationAction(
    const ModerationAction &action, const QString &targetChannelID)
{
    if (targetChannelID.isEmpty())
    {
        return std::nullopt;
    }

    const auto words = action.getAction().split(u' ', Qt::SkipEmptyParts);
    const auto name = words.value(0);
    QString command;
    if (name == u"/delete")
    {
        command = u"/delete {msg.id}"_s;
    }
    else if (words.size() >= 2 &&
             (name == u"/ban" || name == u"/unban" || name == u"/untimeout"))
    {
        command = name + u" id:"_s + targetChannelID;
    }
    else if (words.size() >= 2 && (name == u"/timeout" || name == u".timeout"))
    {
        const auto seconds =
            words.size() > 2 ? parseDurationToSeconds(words.at(2)) : 600;
        if (seconds <= 0)
        {
            return std::nullopt;
        }
        command = u"/timeout id:"_s + targetChannelID + u' ' +
                  QString::number(seconds);
    }
    else
    {
        return std::nullopt;
    }

    return ModerationAction(command, action.iconPath());
}

}  // namespace

struct TwitchModerationElement::YouTubeActionData {
    QString targetChannelID;
    std::function<bool(const QString &)> canModerateUser;
};

TwitchModerationElement::TwitchModerationElement(
    bool canModerateUser, bool targetIsModOrBroadcaster,
    bool targetIsCurrentUser)
    : MessageElement(MessageElementFlag::ModeratorTools)
    , canModerateUser_(canModerateUser)
    , targetIsModOrBroadcaster_(targetIsModOrBroadcaster)
    , targetIsCurrentUser_(targetIsCurrentUser)
{
}

TwitchModerationElement::TwitchModerationElement(
    bool canModerateUser, bool targetIsModOrBroadcaster,
    bool targetIsCurrentUser, std::weak_ptr<Channel> sourceChannel)
    : TwitchModerationElement(canModerateUser, targetIsModOrBroadcaster,
                              targetIsCurrentUser)
{
    this->sourceChannel_ = std::move(sourceChannel);
    this->sourceChannelProvided_ = true;
}

TwitchModerationElement::TwitchModerationElement(
    std::function<bool(const QString &)> canModerateUser,
    QString youtubeTargetChannelID)
    : MessageElement(MessageElementFlag::ModeratorTools)
    , youtubeAction_(std::make_unique<YouTubeActionData>(YouTubeActionData{
          std::move(youtubeTargetChannelID), std::move(canModerateUser)}))
{
}

TwitchModerationElement::~TwitchModerationElement() = default;

void TwitchModerationElement::addToContainer(MessageLayoutContainer &container,
                                             const MessageLayoutContext &ctx)
{
    const bool inModerationMode =
        ctx.flags.has(MessageElementFlag::ModeratorTools);
    auto *settings = getSettings();
    const auto selfDeleteMode =
        normalizeInlineActionMode(settings->showSelfDeleteButton.getValue());
    const auto pinOnModeratorsMode =
        normalizeInlineActionMode(
            settings->showPinButtonOnModeratorsMode.getValue());
    const bool showSelfDeleteOutsideModerationMode =
        this->targetIsCurrentUser_ && selfDeleteMode == 2;
    const bool showPinOutsideModerationMode =
        this->targetIsModOrBroadcaster_ && pinOnModeratorsMode == 2;
    if (!inModerationMode && !showSelfDeleteOutsideModerationMode &&
        !showPinOutsideModerationMode)
    {
        return;
    }

    QSizeF size{
        container.getScale() * 16,
        container.getScale() * 16,
    };

    bool hasVisiblePinAction = false;
    bool hasVisibleDeleteAction = false;
    auto actions = settings->moderationActions.readOnly();
    for (const auto &action : *actions)
    {
        if (!this->shouldShowAction(action, inModerationMode, selfDeleteMode,
                                    pinOnModeratorsMode))
        {
            continue;
        }

        switch (action.getType())
        {
            case ModerationAction::Type::Pin:
                hasVisiblePinAction = true;
                break;
            case ModerationAction::Type::Delete:
                hasVisibleDeleteAction = true;
                break;
            default:
                break;
        }

        if (this->youtubeAction_)
        {
            const auto youtubeAction = makeYouTubeModerationAction(
                action, this->youtubeAction_->targetChannelID);
            if (!youtubeAction)
            {
                continue;
            }
            addModerationActionToContainer(*this, *youtubeAction, container,
                                           size);
        }
        else
        {
            addModerationActionToContainer(*this, action, container, size);
        }
    }

    auto addBuiltInAction = [&](const QString &command) {
        const ModerationAction action(command);
        addModerationActionToContainer(*this, action, container, size);
    };

    if (!this->youtubeAction_ && !hasVisiblePinAction &&
        this->targetIsModOrBroadcaster_ &&
        (settings->showModerationButtonsWithoutPermission ||
         this->sourceHasModRightsNow()) &&
        pinOnModeratorsMode != 0 &&
        (inModerationMode || pinOnModeratorsMode == 2))
    {
        addBuiltInAction("/pin {msg.id}");
    }

    if (!this->youtubeAction_ && !hasVisibleDeleteAction &&
        this->targetIsCurrentUser_ && selfDeleteMode != 0 &&
        (inModerationMode || selfDeleteMode == 2))
    {
        addBuiltInAction("/delete {msg.id}");
    }
}

std::unique_ptr<MessageElement> TwitchModerationElement::clone() const
{
    std::unique_ptr<TwitchModerationElement> el;
    if (this->youtubeAction_)
    {
        el = std::make_unique<TwitchModerationElement>(
            this->youtubeAction_->canModerateUser,
            this->youtubeAction_->targetChannelID);
    }
    else
    {
        if (this->sourceChannelProvided_)
        {
            el = std::make_unique<TwitchModerationElement>(
                this->canModerateUser_, this->targetIsModOrBroadcaster_,
                this->targetIsCurrentUser_, this->sourceChannel_);
        }
        else
        {
            el = std::make_unique<TwitchModerationElement>(
                this->canModerateUser_, this->targetIsModOrBroadcaster_,
                this->targetIsCurrentUser_);
        }
    }
    el->cloneFrom(*this);
    return el;
}

QJsonObject TwitchModerationElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"TwitchModerationElement"_s;

    return base;
}

std::string_view TwitchModerationElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

bool TwitchModerationElement::shouldShowAction(
    const ModerationAction &action, bool inModerationMode, int selfDeleteMode,
    int pinOnModeratorsMode) const
{
    if (this->youtubeAction_)
    {
        if (!inModerationMode || !this->canModerateUserNow())
        {
            return false;
        }
        return true;
    }

    if (!inModerationMode &&
        action.getType() != ModerationAction::Type::Delete &&
        action.getType() != ModerationAction::Type::Pin)
    {
        return false;
    }

    const bool canActInSourceChannel =
        getSettings()->showModerationButtonsWithoutPermission ||
        this->sourceHasModRightsNow();

    switch (action.getType())
    {
        case ModerationAction::Type::Delete:
            if (this->targetIsCurrentUser_)
            {
                return selfDeleteMode != 0 &&
                       (inModerationMode || selfDeleteMode == 2);
            }
            return canActInSourceChannel && this->canModerateUserNow() &&
                   inModerationMode;
        case ModerationAction::Type::Pin:
            if (this->targetIsModOrBroadcaster_)
            {
                return canActInSourceChannel && pinOnModeratorsMode != 0 &&
                       (inModerationMode || pinOnModeratorsMode == 2);
            }
            return canActInSourceChannel && this->canModerateUserNow() &&
                   inModerationMode;
        case ModerationAction::Type::Ban:
        case ModerationAction::Type::Timeout:
        case ModerationAction::Type::Custom:
            return canActInSourceChannel && this->canModerateUserNow();
    }

    return false;
}

bool TwitchModerationElement::canModerateUserNow() const
{
    if (this->youtubeAction_ && this->youtubeAction_->canModerateUser)
    {
        return this->youtubeAction_->canModerateUser(
            this->youtubeAction_->targetChannelID);
    }
    return this->canModerateUser_;
}

bool TwitchModerationElement::sourceHasModRightsNow() const
{
    if (!this->sourceChannelProvided_)
    {
        return true;
    }
    const auto source = this->sourceChannel_.lock();
    return source && source->hasModRights();
}

LinebreakElement::LinebreakElement(MessageElementFlags flags)
    : MessageElement(flags)
{
}

void LinebreakElement::addToContainer(MessageLayoutContainer &container,
                                      const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        container.breakLine();
    }
}

std::unique_ptr<MessageElement> LinebreakElement::clone() const
{
    auto el = std::make_unique<LinebreakElement>(this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject LinebreakElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"LinebreakElement"_s;

    return base;
}

std::string_view LinebreakElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

ScalingImageElement::ScalingImageElement(ImageSet images,
                                         MessageElementFlags flags)
    : MessageElement(flags)
    , images_(std::move(images))
{
}

void ScalingImageElement::addToContainer(MessageLayoutContainer &container,
                                         const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        const auto &image =
            this->images_.getImageOrLoaded(container.getImageScale());
        if (image->isEmpty())
        {
            return;
        }

        container.addElement(new ImageLayoutElement(
            *this, image, image->size() * container.getScale()));
    }
}

std::unique_ptr<MessageElement> ScalingImageElement::clone() const
{
    auto el =
        std::make_unique<ScalingImageElement>(this->images_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

const ImageSet &ScalingImageElement::images() const
{
    return this->images_;
}

QJsonObject ScalingImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ScalingImageElement"_s;
    base["image"_L1] = this->images_.getImage1()->url().string;

    return base;
}

std::string_view ScalingImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

ReplyCurveElement::ReplyCurveElement()
    : MessageElement(MessageElementFlag::RepliedMessage)
{
}

void ReplyCurveElement::addToContainer(MessageLayoutContainer &container,
                                       const MessageLayoutContext &ctx)
{
    static const qreal width = 18;       // Overall width
    static const float thickness = 1.5;  // Pen width
    static const int radius = 6;         // Radius of the top left corner
    static const int margin = 2;         // Top/Left/Bottom margin

    if (ctx.flags.hasAny(this->getFlags()))
    {
        float scale = container.getScale();
        container.addElement(
            new ReplyCurveLayoutElement(*this, width * scale, thickness * scale,
                                        radius * scale, margin * scale));
    }
}

std::unique_ptr<MessageElement> ReplyCurveElement::clone() const
{
    auto el = std::make_unique<ReplyCurveElement>();
    el->cloneFrom(*this);
    return el;
}

QJsonObject ReplyCurveElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ReplyCurveElement"_s;

    return base;
}

std::string_view ReplyCurveElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

}  // namespace chatterino
