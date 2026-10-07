#include "singletons/ThemeVideo.hpp"

#include "singletons/Settings.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "singletons/ThemeVideoDecoder.hpp"

#include <libyuv/planar_functions.h>
#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QPainter>
#include <QtConcurrent>
#include <QWidget>

#include <algorithm>
#include <iterator>
#include <vector>

namespace chatterino {
namespace {

QImage blurVideoFrame(QImage image, int radius)
{
    const int passRadius = std::max(1, (radius + 1) / 2);
    if (image.width() < 4 || image.height() < 2)
    {
        return blurThemeWallpaper(std::move(image), radius);
    }
    struct alignas(16) Sum {
        int32_t channels[4];
    };
    std::vector<Sum> sums(size_t(image.width()) * (2 * passRadius + 2));
    QImage scratch(image.size(), QImage::Format_RGB32);
    if (scratch.isNull())
    {
        return image;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        if (libyuv::ARGBBlur(image.constBits(), int(image.bytesPerLine()),
                             scratch.bits(), int(scratch.bytesPerLine()),
                             sums.front().channels, image.width() * 4,
                             image.width(), image.height(), passRadius) != 0)
        {
            return image;
        }
        image.swap(scratch);
    }

    for (int y = 0; y < image.height(); ++y)
    {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            pixels[x] |= 0xff000000;
        }
    }
    return image;
}

}

std::shared_ptr<ThemeVideo> ThemeVideo::acquire(const QString &source,
                                                QWidget *consumer)
{
    static QHash<QString, std::weak_ptr<ThemeVideo>> players;
    for (auto it = players.begin(); it != players.end();)
    {
        it = it.value().expired() ? players.erase(it) : std::next(it);
    }
    auto player = players.value(source).lock();
    if (!player)
    {
        player.reset(new ThemeVideo(source));
        players.insert(source, player);
    }
    if (!player->consumers_.contains(consumer))
    {
        player->consumers_.removeIf([](const auto &item) {
            return item.isNull();
        });
        player->consumers_.push_back(consumer);
        consumer->installEventFilter(player.get());
        consumer->window()->installEventFilter(player.get());
    }
    player->updatePlayback();
    return player;
}

ThemeVideo::ThemeVideo(QString source)
    : source_(std::move(source))
{
    this->timer_.setSingleShot(true);
    this->timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->timer_, &QTimer::timeout, this,
                     &ThemeVideo::decode);
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this,
                     [this] {
                         this->updatePlayback();
                     });
    getSettings()->animationsWhenFocused.connect(
        [this](const bool &) {
            this->updatePlayback();
        },
        this->focusSettingConnection_, false);
    this->clock_.start();
}

ThemeVideo::~ThemeVideo() = default;

QImage ThemeVideo::currentFrame() const
{
    return this->frame_.toImage();
}

void ThemeVideo::detach(QWidget *consumer)
{
    this->consumers_.removeAll(consumer);
    consumer->removeEventFilter(this);
    this->updatePlayback();
}

bool ThemeVideo::shouldPlay() const
{
    if (!this->frame_.isNull() && getSettings()->animationsWhenFocused &&
        QApplication::applicationState() != Qt::ApplicationActive)
    {
        return false;
    }
    return std::any_of(this->consumers_.begin(), this->consumers_.end(),
                       [](const auto &widget) {
                           return widget && widget->isVisible() &&
                                  !widget->window()->isMinimized();
                       });
}

void ThemeVideo::updatePlayback()
{
    if (!this->shouldPlay())
    {
        this->timer_.stop();

        if (!this->decoding_)
        {
            this->decoder_.reset();
        }
        this->deadline_ = this->clock_.elapsed();
    }
    else if (!this->failed_ && !this->decoding_ && !this->timer_.isActive() &&
             this->frameCount_ != 1)
    {
        this->deadline_ = this->clock_.elapsed();
        this->timer_.start(0);
    }
}

bool ThemeVideo::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type())
    {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::WindowStateChange:
        case QEvent::ParentChange:
            if (auto *widget = qobject_cast<QWidget *>(object))
            {
                widget->window()->installEventFilter(this);
            }

            QMetaObject::invokeMethod(
                this,
                [this] {
                    this->updatePlayback();
                },
                Qt::QueuedConnection);
            break;
        default:
            break;
    }
    return QObject::eventFilter(object, event);
}

void ThemeVideo::decode()
{
    if (!this->shouldPlay() || this->decoding_ || this->failed_)
    {
        return;
    }
    if (!this->decoder_)
    {
        this->decoder_ = std::make_shared<ThemeVideoDecoder>(this->source_);
    }
    this->decoding_ = true;
    const auto decoder = this->decoder_;
    (void)QtConcurrent::run([decoder] {
        return decoder->decodeNext();
    }).then(this, [this, decoder](bool decoded) {
        this->decoding_ = false;
        if (!this->shouldPlay())
        {
            this->decoder_.reset();
            return;
        }
        if (!decoded)
        {
            this->failed_ = true;
            this->decoder_.reset();
            return;
        }

        auto image = this->frame_.toImage();
        this->frame_ = {};
        if (!decoder->copyFrame(image))
        {
            this->failed_ = true;
            this->decoder_.reset();
            return;
        }
        this->frame_ =
            QPixmap::fromImage(std::move(image), Qt::NoFormatConversion);
        this->blurred_.clear();
        this->frameCount_ = decoder->frameCount();
        this->deadline_ += decoder->frameDuration();
        const auto now = this->clock_.elapsed();

        if (this->deadline_ <= now)
        {
            this->deadline_ = now + decoder->frameDuration();
        }
        Q_EMIT this->frameChanged();
        if (!this->shouldPlay())
        {
            this->decoder_.reset();
            return;
        }
        if (this->frameCount_ > 1)
        {
            this->timer_.start(int(
                std::max<qint64>(1, this->deadline_ - this->clock_.elapsed())));
        }
        else
        {
            this->decoder_.reset();
        }
    });
}

bool ThemeVideo::paint(QPainter &painter, const QRectF &target,
                       const ThemeCustomizationProfile &profile)
{
    if (this->frame_.isNull())
    {
        return false;
    }
    const QPixmap *image = &this->frame_;
    if (profile.wallpaperBlur > 0)
    {
        auto found = this->blurred_.constFind(profile.wallpaperBlur);
        if (found == this->blurred_.cend())
        {
            if (this->blurred_.size() >= 2)
            {
                this->blurred_.clear();
            }
            auto reduced = this->frame_.toImage();
            if (reduced.width() > 384 || reduced.height() > 384)
            {
                reduced = reduced.scaled(384, 384, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
            }
            const auto scale = double(reduced.width()) / this->frame_.width();
            this->blurred_.insert(
                profile.wallpaperBlur,
                QPixmap::fromImage(blurVideoFrame(
                    std::move(reduced),
                    std::max(1, qRound(profile.wallpaperBlur * scale)))));
            found = this->blurred_.constFind(profile.wallpaperBlur);
        }
        image = &found.value();
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setOpacity(profile.wallpaperOpacity / 100.0);

    if (profile.wallpaperMode == ThemeWallpaperMode::Tile)
    {
        painter.setClipRect(target, Qt::IntersectClip);
        const auto zoom = std::clamp(profile.wallpaperZoom, 100, 300) / 100.0;
        const auto xScale =
            double(this->frame_.width()) / image->width() * zoom;
        const auto yScale =
            double(this->frame_.height()) / image->height() * zoom;
        painter.translate(target.topLeft());
        painter.scale(xScale, yScale);
        painter.drawTiledPixmap(
            QRectF(0, 0, target.width() / xScale, target.height() / yScale),
            *image);
    }
    else
    {
        const auto layout = layoutThemeWallpaper(
            this->frame_.size(), target, profile.wallpaperMode,
            profile.wallpaperFocalX, profile.wallpaperFocalY,
            profile.wallpaperZoom);
        const auto xScale = double(image->width()) / this->frame_.width();
        const auto yScale = double(image->height()) / this->frame_.height();
        const QRectF source(
            layout.source.x() * xScale, layout.source.y() * yScale,
            layout.source.width() * xScale, layout.source.height() * yScale);
        painter.drawPixmap(layout.destination, *image, source);
    }
    painter.restore();
    return true;
}

}
