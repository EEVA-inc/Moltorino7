// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace chatterino {

namespace {

uint32_t toCachedMessageMetric(size_t value)
{
    return static_cast<uint32_t>(std::min(
        value, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
}

struct ClientDetectionMarker {
    const QPixmap *icon = nullptr;
    QColor color;
    QString tooltip;
};

std::optional<ClientDetectionMarker> clientDetectionMarker(
    const Message &message, const MessagePreferences &preferences)
{
    static const QPixmap desktopIcon(
        QStringLiteral(":/badges/client-desktop.svg"));
    static const QPixmap androidIcon(
        QStringLiteral(":/badges/client-android.svg"));
    static const QPixmap iosIcon(QStringLiteral(":/badges/client-ios.svg"));
    static const QPixmap unknownIcon(
        QStringLiteral(":/badges/client-unknown.svg"));

    using Status = Message::ClientDetectionStatus;
    switch (message.clientDetection)
    {
        case Status::Web:
            return ClientDetectionMarker{
                &desktopIcon, preferences.clientDetectionWebColor,
                QStringLiteral("Sent from Twitch Web")};
        case Status::Android:
            return ClientDetectionMarker{
                &androidIcon, preferences.clientDetectionAndroidColor,
                QStringLiteral("Sent from Twitch for Android")};
        case Status::IOS:
            return ClientDetectionMarker{
                &iosIcon, preferences.clientDetectionIosColor,
                QStringLiteral("Sent from Twitch for iOS")};
        case Status::Abnormal:
            if (preferences.enableAbnormalClientDetectionHighlight)
            {
                return ClientDetectionMarker{
                    &unknownIcon, preferences.clientDetectionAbnormalColor,
                    QStringLiteral("Unrecognized message source")};
            }
            return std::nullopt;
        case Status::Unknown:
            return std::nullopt;
    }

    return std::nullopt;
}
}

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    this->deleteBuffer();
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

int MessageLayout::getHeight() const
{
    return static_cast<int>(this->height_);
}

int MessageLayout::getFirstLineHeight() const
{
    return this->firstLineHeight_;
}

int MessageLayout::getWidth() const
{
    return this->width_;
}

size_t MessageLayout::getLineCount() const
{
    return this->lineCount_;
}

bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    return this->layoutImpl(ctx, shouldInvalidateBuffer, true);
}

bool MessageLayout::layoutForMeasurement(const MessageLayoutContext &ctx)
{
    return this->layoutImpl(ctx, false, false);
}

bool MessageLayout::layoutImpl(const MessageLayoutContext &ctx,
                               bool shouldInvalidateBuffer,
                               bool retainContainer)
{
    const bool hadContainer = this->container_ != nullptr;
    bool layoutRequired = retainContainer && !hadContainer;

    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;

    if (this->showHighlights_ != ctx.showHighlights())
    {
        this->showHighlights_ = ctx.showHighlights();
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        layoutRequired = true;
    }

    if (this->autoModReviewExpanded_ != ctx.autoModReviewExpanded)
    {
        this->autoModReviewExpanded_ = ctx.autoModReviewExpanded;
        this->flags.set(MessageLayoutFlag::AutoModReviewSelected,
                        ctx.autoModReviewExpanded);
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        layoutRequired = true;
    }

    if (this->flags.has(MessageLayoutFlag::AutoModReviewChannel) !=
        ctx.autoModReviewChannel)
    {
        this->flags.set(MessageLayoutFlag::AutoModReviewChannel,
                        ctx.autoModReviewChannel);
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        layoutRequired = true;
    }

    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    bool scaleChanged = this->scale_ != ctx.scale ||
                        this->imageScale_ != ctx.imageScale ||
                        this->emoteScale_ != ctx.emoteScale ||
                        this->badgeScale_ != ctx.badgeScale ||
                        this->centerBadges_ != ctx.centerBadges;
    layoutRequired |= scaleChanged;
    this->scale_ = ctx.scale;
    this->imageScale_ = ctx.imageScale;
    this->emoteScale_ = ctx.emoteScale;
    this->badgeScale_ = ctx.badgeScale;
    this->centerBadges_ = ctx.centerBadges;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->height_;
    this->actuallyLayout(ctx);
    if (widthChanged || this->height_ != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    if (!retainContainer && !hadContainer)
    {
        this->container_.reset();
    }

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
    ctx.resetMessageTextCursor();
#ifdef FOURTF
    this->layoutCount_++;
#endif

    if (!this->container_)
    {
        this->container_ = std::make_unique<MessageLayoutContainer>();
    }
    auto &container = *this->container_;

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);
    const bool hideGigantifyReward =
        this->message_->usesTwitchGigantifyPresentation();
    const bool hideClientNonce = this->message_->isHiddenByClientNonce();

    container.beginLayout(ctx.width, this->scale_, this->imageScale_,
                          this->emoteScale_, this->badgeScale_,
                          this->centerBadges_, messageFlags);

    std::optional<ClientDetectionMarker> clientMarker;
    if (ctx.preferences != nullptr &&
        ctx.preferences->enableClientDetectionIcon &&
        ctx.flags.has(MessageElementFlag::Username))
    {
        clientMarker = clientDetectionMarker(*this->message_, *ctx.preferences);
    }
    bool clientMarkerAdded = false;

    for (const auto &element : this->message_->elements)
    {
        if (hideClientNonce)
        {
            break;
        }
        if (element->getFlags().has(
                MessageElementFlag::AutoModReviewExpanded) &&
            !ctx.autoModReviewExpanded)
        {
            continue;
        }
        if (element->getFlags().has(MessageElementFlag::AutoModReviewCompact) &&
            ctx.autoModReviewExpanded)
        {
            continue;
        }
        if (hideGigantifyReward &&
            element->getFlags().hasAny(
                {MessageElementFlag::ChannelPointReward,
                 MessageElementFlag::ChannelPointRewardHeader}))
        {
            continue;
        }

        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {

            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {

                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {

                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        if (!clientMarkerAdded && clientMarker &&
            (element->getFlags().hasAny(
                 {MessageElementFlag::Badges, MessageElementFlag::Pronouns}) ||
             element->getFlags().has(MessageElementFlag::Username)))
        {
            container.addElement(new ClientDetectionLayoutElement(
                *clientMarker->icon, clientMarker->color, clientMarker->tooltip,
                container.getBadgeScale()));
            clientMarkerAdded = true;
        }

        element->addToContainer(container, ctx);
    }

    if (this->height_ != container.getHeight())
    {
        this->deleteBuffer();
    }

    container.endLayout();
    this->height_ = container.getHeight();
    this->firstLineHeight_ = container.getFirstLineHeight();
    this->width_ = static_cast<int>(container.getWidth());
    this->lineCount_ = toCachedMessageMetric(container.getLineCount());
    this->firstMessageCharacterIndex_ =
        toCachedMessageMetric(container.getFirstMessageCharacterIndex());
    this->lastCharacterIndex_ =
        toCachedMessageMetric(container.getLastCharacterIndex());

    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (container.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;
    if (!this->container_)
    {
        return result;
    }
    auto &container = *this->container_;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    const AnimatedMessageShadow animatedShadow{
        ctx.messageShadowColor,
        ctx.messageShadowOpacity / 100.0,
        QPointF(ctx.messageShadowOffset) * this->scale_,
        std::clamp(ctx.messageShadowBlur, 0, 8) * this->scale_,
        ctx.messageShadowEmotes,
    };
    const auto *shadow = ctx.paintMessageShadow && ctx.messageShadowOpacity > 0
                             ? &animatedShadow
                             : nullptr;
    const auto animatedRegions = container.paintAnimatedElements(
        ctx.painter, ctx.y, ctx.isCollapsed, shadow, ctx.hoveredElement,
        ctx.hoverAnimateOnly);
    result.animatedRegion += animatedRegions.periodic;
    result.selfTimedAnimatedRegion += animatedRegions.selfTimed;

    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (ctx.preferences.showHighlights && !ctx.isMentions &&
        ((this->message_->flags.has(MessageFlag::RedeemedChannelPointReward) &&
          !this->message_->usesTwitchGigantifyPresentation()) ||
         this->message_->flags.has(MessageFlag::RedeemedHighlight)) &&
        ctx.preferences.enableRedeemedHighlight)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                static_cast<int>(this->scale_ * 4),
                pixmap->height(),
            },
            *ColorProvider::instance().color(ColorType::RedeemedHighlight));
    }

    if (!ctx.selection.isEmpty())
    {
        container.paintSelection(ctx.painter, ctx.messageIndex,
                                 ctx.selection, ctx.y);
    }

    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                static_cast<qreal>(ctx.canvasWidth),
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + container.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->height_ *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull() || !this->container_)
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QColor backgroundColor = [&] {
        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();

    if (ctx.preferences.showHighlights &&
        this->flags.has(MessageLayoutFlag::AutoModReviewSelected))
    {
        auto selection = ctx.messageColors.selection;
        selection.setAlpha(26);
        backgroundColor = blendThemeHighlight(backgroundColor, selection, 0);
    }

    if (ctx.preferences.showHighlights)
    {
        if (this->message_->flags.has(MessageFlag::ElevatedMessage) &&
            ctx.preferences.enableElevatedMessageHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::ElevatedMessageHighlight),
                ctx.highlightOpacityAdjustment);
        }

        else if (this->message_->flags.has(MessageFlag::FirstMessage) &&
                 ctx.preferences.enableFirstMessageHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::FirstMessageHighlight),
                ctx.highlightOpacityAdjustment);
        }
        else if (this->message_->flags.has(MessageFlag::WatchStreak) &&
                 ctx.preferences.enableWatchStreakHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::WatchStreak),
                ctx.highlightOpacityAdjustment);
        }
        else if ((this->message_->flags.has(MessageFlag::Highlighted) ||
                  this->message_->flags.has(MessageFlag::HighlightedWhisper)) &&
                 !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
        {
            assert(this->message_->highlightColor);
            if (this->message_->highlightColor)
            {
                backgroundColor = blendThemeHighlight(
                    backgroundColor, *this->message_->highlightColor,
                    ctx.highlightOpacityAdjustment);
            }
        }
        else if (this->message_->flags.has(MessageFlag::Announcement) &&
                 ctx.preferences.enableAnnouncementHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(colorTypeFromHelixAnnouncementColor(
                    this->message_->announcementColor,
                    ctx.preferences.enableColoredAnnouncementHighlight)),
                ctx.highlightOpacityAdjustment);
        }
        else if (this->message_->flags.has(MessageFlag::Subscription) &&
                 ctx.preferences.enableSubHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::Subscription),
                ctx.highlightOpacityAdjustment);
        }
        else if ((this->message_->flags.has(MessageFlag::RedeemedHighlight) ||
                  (this->message_->flags.has(
                       MessageFlag::RedeemedChannelPointReward) &&
                   !this->message_->usesTwitchGigantifyPresentation())) &&
                 ctx.preferences.enableRedeemedHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::RedeemedHighlight),
                ctx.highlightOpacityAdjustment);
        }
        else if (this->message_->flags.has(MessageFlag::ChatWarning) &&
                 ctx.preferences.enableAutomodHighlight)
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::AutomodHighlight),
                ctx.highlightOpacityAdjustment);
        }
        else if (this->message_->flags.has(MessageFlag::AutoMod) ||
                 this->message_->flags.has(MessageFlag::LowTrustUsers))
        {
            if (this->message_->autoModReview)
            {
                if (ctx.preferences.enableAutomodHighlight)
                {
                    auto accent =
                        *ctx.colorProvider.color(ColorType::AutomodHighlight);
                    if (this->flags.has(
                            MessageLayoutFlag::AutoModReviewChannel))
                    {
                        accent.setAlpha(18);
                    }
                    backgroundColor =
                        blendThemeHighlight(backgroundColor, accent,
                                            ctx.highlightOpacityAdjustment);
                }
            }
            else if (ctx.preferences.enableAutomodHighlight &&
                     (this->message_->flags.has(
                          MessageFlag::AutoModOffendingMessage) ||
                      this->message_->flags.has(
                          MessageFlag::AutoModOffendingMessageHeader)))
            {
                backgroundColor = blendThemeHighlight(
                    backgroundColor,
                    *ctx.colorProvider.color(ColorType::AutomodHighlight),
                    ctx.highlightOpacityAdjustment);
            }
            else if (ctx.preferences.enableAutomodHighlight)
            {
                backgroundColor = blendThemeHighlight(
                    backgroundColor,
                    HighlightPhrase::FALLBACK_AUTOMOD_HIGHLIGHT_COLOR,
                    ctx.highlightOpacityAdjustment);
            }
        }
        else if (this->message_->flags.has(MessageFlag::Debug))
        {
            backgroundColor = QColor("#4A273D");
        }
        else if ((ctx.preferences.enableClientDetectionHighlight &&
                  this->message_->clientDetection !=
                      Message::ClientDetectionStatus::Unknown &&
                  this->message_->clientDetection !=
                      Message::ClientDetectionStatus::Abnormal) ||
                 (ctx.preferences.enableAbnormalClientDetectionHighlight &&
                  this->message_->clientDetection ==
                      Message::ClientDetectionStatus::Abnormal))
        {
            switch (this->message_->clientDetection)
            {
                case Message::ClientDetectionStatus::Web:
                    backgroundColor = blendThemeHighlight(
                        backgroundColor,
                        ctx.preferences.clientDetectionWebColor,
                        ctx.highlightOpacityAdjustment);
                    break;
                case Message::ClientDetectionStatus::Android:
                    backgroundColor = blendThemeHighlight(
                        backgroundColor,
                        ctx.preferences.clientDetectionAndroidColor,
                        ctx.highlightOpacityAdjustment);
                    break;
                case Message::ClientDetectionStatus::IOS:
                    backgroundColor = blendThemeHighlight(
                        backgroundColor,
                        ctx.preferences.clientDetectionIosColor,
                        ctx.highlightOpacityAdjustment);
                    break;
                case Message::ClientDetectionStatus::Abnormal:
                    backgroundColor = blendThemeHighlight(
                        backgroundColor,
                        ctx.preferences.clientDetectionAbnormalColor,
                        ctx.highlightOpacityAdjustment);
                    break;
                case Message::ClientDetectionStatus::Unknown:
                    break;
            }
        }
        else if (this->message_->flags.has(
                     MessageFlag::UncategorizedNotification))
        {
            backgroundColor = blendThemeHighlight(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::Subscription),
                ctx.highlightOpacityAdjustment);
        }
    }

    painter.fillRect(buffer->rect(), backgroundColor);
    if (ctx.preferences.showHighlights && this->message_->autoModReview)
    {
        const bool selected =
            this->flags.has(MessageLayoutFlag::AutoModReviewSelected);
        auto accent =
            selected ? ctx.messageColors.linkText
                     : *ctx.colorProvider.color(ColorType::AutomodHighlight);
        accent.setAlpha(selected ? 205 : 110);
        painter.fillRect(
            QRectF(0, 0, std::max(1.0, (selected ? 2.0 : 1.0) * this->scale_),
                   buffer->height()),
            accent);
    }

    if (ctx.paintMessageShadow && ctx.messageShadowOpacity > 0)
    {
        QImage shadow(buffer->size(), QImage::Format_ARGB32_Premultiplied);
        shadow.setDevicePixelRatio(buffer->devicePixelRatio());
        shadow.fill(Qt::transparent);
        {
            QPainter shadowPainter(&shadow);
            shadowPainter.setRenderHint(QPainter::SmoothPixmapTransform);

            this->container_->paintElements(shadowPainter, ctx, false,
                                            ctx.messageShadowEmotes);
            shadowPainter.setCompositionMode(
                QPainter::CompositionMode_SourceIn);
            shadowPainter.fillRect(
                QRectF(QPointF(), shadow.deviceIndependentSize()),
                ctx.messageShadowColor);
        }
        const int blur = std::clamp(ctx.messageShadowBlur, 0, 8);
        if (blur > 0)
        {
            const auto pixelRadius =
                qRound(blur * this->scale_ * shadow.devicePixelRatio());
            shadow = blurThemeShadow(std::move(shadow), pixelRadius);
        }
        const auto shadowPixmap = QPixmap::fromImage(std::move(shadow));

        painter.save();
        const QPointF offset{
            ctx.messageShadowOffset.x() * this->scale_,
            ctx.messageShadowOffset.y() * this->scale_,
        };
        painter.setOpacity(ctx.messageShadowOpacity / 100.0);
        painter.drawPixmap(offset, shadowPixmap);
        painter.restore();
    }

    assert(this->container_);
    this->container_->paintElements(painter, ctx);

#ifdef FOURTF
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_->getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    if (this->container_)
    {
        this->container_->releasePickerImages();
    }
    this->deleteBuffer();
    this->container_.reset();
}

bool MessageLayout::hasCache() const
{
    return this->container_ != nullptr;
}

const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{

    return this->container_ ? this->container_->getElementAt(point) : nullptr;
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{

    if (hoveredElement->getWordId() != -1)
    {
        assert(this->container_);
        return this->container_->getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->lastCharacterIndex_;
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->firstMessageCharacterIndex_;
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_ ? this->container_->getSelectionIndex(position) : 0;
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    if (this->container_)
    {
        this->container_->addSelectionText(str, from, to, copymode);
    }
}

}
