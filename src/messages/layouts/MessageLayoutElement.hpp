// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Link.hpp"
#include "messages/MessageColor.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QString>

#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QPainter;
class QPixmap;

namespace chatterino {
class MessageElement;
class Image;
class Paint;
using ImagePtr = std::shared_ptr<Image>;
enum class FontStyle : uint8_t;
enum class MessageElementFlag : int64_t;
struct MessageColors;

struct AnimatedMessageShadow {
    QColor color{Qt::black};
    qreal opacity{};
    QPointF offset;
    qreal blurRadius{};
    bool emotes = true;
};

struct FragmentHighlight {
    HighlightMatch match;

    std::shared_ptr<Paint> textPaint;

    bool connectsBefore = false;
    bool connectsAfter = false;
};

void paintFragmentHighlightBackground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight);
void paintFragmentHighlightForeground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight);

class MessageLayoutElement
{
public:
    MessageLayoutElement(MessageElement &creator_, QSizeF size);
    virtual ~MessageLayoutElement();

    MessageLayoutElement(const MessageLayoutElement &) = delete;
    MessageLayoutElement &operator=(const MessageLayoutElement &) = delete;

    MessageLayoutElement(MessageLayoutElement &&) = delete;
    MessageLayoutElement &operator=(MessageLayoutElement &&) = delete;

    bool reversedNeutral = false;

    const QRectF &getRect() const;
    MessageElement &getCreator() const;
    void setPosition(QPointF point);
    bool hasTrailingSpace() const;
    bool usesCompactEmoteLayout() const;
    size_t getLine() const;
    void setLine(size_t line);

    MessageLayoutElement *setTrailingSpace(bool value);
    MessageLayoutElement *setCompactEmoteLayout(bool enabled);

    MessageLayoutElement *setLink(const Link &link);

    MessageLayoutElement *setText(const QString &text_);

    virtual void addCopyTextToString(QString &str, uint32_t from = 0,
                                     uint32_t to = UINT32_MAX) const = 0;
    virtual size_t getSelectionIndexCount() const = 0;
    virtual void paint(QPainter &painter,
                       const MessageColors &messageColors) = 0;

    virtual void paint(QPainter &painter, const MessageColors &messageColors,
                       bool hoverAnimateOnly)
    {
        (void)hoverAnimateOnly;
        this->paint(painter, messageColors);
    }
    virtual void paint(QPainter &painter, const MessageColors &messageColors,
                       bool hoverAnimateOnly, bool isHovered)
    {
        (void)isHovered;
        this->paint(painter, messageColors, hoverAnimateOnly);
    }

    virtual bool hasAnimatedContent() const
    {
        return false;
    }
    virtual bool usesOwnAnimationTimer() const
    {
        return false;
    }

    virtual QRegion paintAnimated(
        QPainter &painter, qreal yOffset,
        const AnimatedMessageShadow *shadow = nullptr) = 0;
    virtual QRegion paintAnimated(QPainter &painter, qreal yOffset,
                                  const AnimatedMessageShadow *shadow,
                                  bool hoverAnimateOnly, bool isHovered)
    {
        (void)hoverAnimateOnly;
        (void)isHovered;
        return this->paintAnimated(painter, yOffset, shadow);
    }
    virtual int getMouseOverIndex(QPointF abs) const = 0;
    virtual qreal getXFromIndex(size_t index) = 0;

    Link getLink() const;
    const QString &getText() const;
    FlagsEnum<MessageElementFlag> getFlags() const;

    int getWordId() const;
    void setWordId(int wordId);

    virtual void setFragmentHighlight(FragmentHighlight highlight);
    virtual const FragmentHighlight *fragmentHighlight() const;
    virtual void clearFragmentHighlightContinuity(bool before, bool after);
    virtual QString getFragmentTooltip(QPointF point) const;

protected:
    bool trailingSpace = true;

private:
    QString text_;
    QRectF rect_;
    std::optional<Link> link_;
    MessageElement &creator_;
    bool compactEmoteLayout_ = true;
    size_t line_{};

    int wordId_ = -1;
};

class ImageLayoutElement : public MessageLayoutElement
{
public:
    ImageLayoutElement(MessageElement &creator, ImagePtr image, QSizeF size,
                       qreal horizontalPadding = 0.0);

    void setHoverImage(ImagePtr image);
    const ImagePtr &getHoverImage() const;
    void setPreviewImage(ImagePtr image);
    bool hasPreviewImage() const;
    void releasePickerImages() const;

    void setFragmentHighlight(FragmentHighlight highlight) override;
    const FragmentHighlight *fragmentHighlight() const override;
    void clearFragmentHighlightContinuity(bool before, bool after) override;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    void paint(QPainter &painter, const MessageColors &messageColors,
               bool hoverAnimateOnly) override;
    void paint(QPainter &painter, const MessageColors &messageColors,
               bool hoverAnimateOnly, bool isHovered) override;
    bool hasAnimatedContent() const override;
    bool usesOwnAnimationTimer() const override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow,
                          bool hoverAnimateOnly, bool isHovered) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

    ImagePtr image_;
    ImagePtr hoverImage_;
    ImagePtr previewImage_;
    qreal horizontalPadding_ = 0.0;
    std::unique_ptr<FragmentHighlight> fragmentHighlight_;

    QRegion paintAnimatedImage(QPainter &painter, qreal yOffset,
                               const AnimatedMessageShadow *shadow,
                               const ImagePtr &image);
};

class LayeredImageLayoutElement : public MessageLayoutElement
{
public:
    LayeredImageLayoutElement(MessageElement &creator,
                              std::vector<ImagePtr> images,
                              std::vector<QSizeF> sizes, QSizeF largestSize,
                              uint32_t modifierFlags = 0);

    bool removesPreviousSpace() const;

    void setFragmentHighlight(FragmentHighlight highlight) override;
    const FragmentHighlight *fragmentHighlight() const override;
    void clearFragmentHighlightContinuity(bool before, bool after) override;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    void paint(QPainter &painter, const MessageColors &messageColors,
               bool hoverAnimateOnly) override;
    void paint(QPainter &painter, const MessageColors &messageColors,
               bool hoverAnimateOnly, bool isHovered) override;
    bool hasAnimatedContent() const override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow,
                          bool hoverAnimateOnly, bool isHovered) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    bool needsAnimatedPaint() const;
    QRegion paintModified(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow);

    std::vector<ImagePtr> images_;
    std::vector<QSizeF> sizes_;
    QSizeF contentSize_;
    uint32_t modifierFlags_ = 0;
    std::unique_ptr<FragmentHighlight> fragmentHighlight_;
};

class ImageWithBackgroundLayoutElement : public ImageLayoutElement
{
public:
    ImageWithBackgroundLayoutElement(MessageElement &creator, ImagePtr image,
                                     QSizeF size, QColor color);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;

private:
    QColor color_;
};

class ImageWithCircleBackgroundLayoutElement : public ImageLayoutElement
{
public:
    ImageWithCircleBackgroundLayoutElement(MessageElement &creator,
                                           ImagePtr image,
                                           const QSize &imageSize, QColor color,
                                           int padding);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;

private:
    const QColor color_;
    const QSize imageSize_;
    const int padding_;
};

class TextLayoutElement : public MessageLayoutElement
{
public:
    TextLayoutElement(MessageElement &creator_, QString &text, QSizeF size,
                      QColor color_, FontStyle style_,
                      MessageColor::Type messageColor, float scale_,
                      float dpr = 1.0F);

    QString getPaintTooltip() const;
    QString getFragmentTooltip(QPointF point) const override;
    void setFragmentHighlights(std::vector<FragmentHighlight> highlights);
    const std::vector<FragmentHighlight> &fragmentHighlights() const;
    void clearFragmentHighlightContinuity(bool before, bool after) override;
    void paintWithoutFragmentHighlights(QPainter &painter);

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

    QRegion paintMatchTextPaints(QPainter &painter, qreal yOffset,
                                 bool animated,
                                 const AnimatedMessageShadow *shadow = nullptr);
    qreal getTextXFromIndex(size_t index) const;
    QRectF fragmentRect(size_t from, size_t to) const;

    QColor color_;
    FontStyle style_;

    MessageColor::Type messageColor_;
    float scale_;
    float dpr_ = 1.0F;
    std::unique_ptr<std::vector<FragmentHighlight>> fragmentHighlights_;

private:
    void paintText(QPainter &painter, bool paintHighlights);
};

class TextIconLayoutElement : public MessageLayoutElement
{
public:
    TextIconLayoutElement(MessageElement &creator_, const QString &line1,
                          const QString &line2, float scale, QSizeF size);

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    float scale;
    QString line1;
    QString line2;
};

class ClientDetectionLayoutElement : public MessageLayoutElement
{
public:
    ClientDetectionLayoutElement(const QPixmap &platformIcon, QColor color,
                                 QString tooltip, float badgeScale);

    QString getFragmentTooltip(QPointF point) const override;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    const QPixmap *platformIcon_;
    QColor color_;
    QString tooltip_;
    float badgeScale_;
};

class ReplyCurveLayoutElement : public MessageLayoutElement
{
public:
    ReplyCurveLayoutElement(MessageElement &creator, qreal width,
                            float thickness, float radius, float neededMargin);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;

private:
    const QPen pen_;
    const float radius_;
    const float neededMargin_;
};

}
