#include "messages/StreamingImageDecoder.hpp"

#include <QtEndian>

#include <algorithm>
#include <limits>

namespace chatterino::detail {

StreamingImageDecoder::StreamingImageDecoder(QByteArray data, int maxDimension,
                                             int64_t maxBytes,
                                             bool firstFrameOnly)
{
    const bool isWebP = data.size() >= 20 && data.startsWith("RIFF") &&
                        data.mid(8, 4) == "WEBP";
    if (data.size() < 13 || data.size() > maxBytes ||
        (!isWebP && !data.startsWith("GIF87a") && !data.startsWith("GIF89a")))
    {
        this->finish(QStringLiteral("Invalid or oversized GIF response"));
        return;
    }

    QSize sourceSize;
    int webpChunks = 0;
    uint64_t webpDuration = 0;
    if (isWebP)
    {
        WebPBitstreamFeatures features{};
        if (uint64_t(qFromLittleEndian<quint32>(data.constData() + 4)) + 8 !=
                uint64_t(data.size()) ||
            WebPGetFeatures(reinterpret_cast<const uint8_t *>(data.constData()),
                            data.size(), &features) != VP8_STATUS_OK)
        {
            this->finish(QStringLiteral("Invalid WebP response"));
            return;
        }
        sourceSize = {features.width, features.height};

        for (uint64_t offset = 12; offset < uint64_t(data.size());)
        {
            if (offset + 8 > uint64_t(data.size()))
            {
                this->finish(QStringLiteral("Truncated WebP chunk"));
                return;
            }
            const auto *chunk = data.constData() + offset;
            const uint64_t length = qFromLittleEndian<quint32>(chunk + 4);
            const uint64_t next = offset + 8 + length + (length & 1);
            if (next > uint64_t(data.size()) || ++webpChunks > 10032)
            {
                this->finish(
                    QStringLiteral("Invalid or oversized WebP animation"));
                return;
            }
            if (QByteArrayView(chunk, 4) == "ANMF")
            {
                if (length < 16)
                {
                    this->finish(QStringLiteral("Invalid WebP frame header"));
                    return;
                }
                webpDuration +=
                    qFromLittleEndian<quint32>(chunk + 20) & 0xffffff;

                if (webpDuration > uint64_t(std::numeric_limits<int>::max()))
                {
                    this->finish(
                        QStringLiteral("WebP animation duration is too long"));
                    return;
                }
            }
            offset = next;
        }
    }
    else
    {
        sourceSize = {qFromLittleEndian<quint16>(data.constData() + 6),
                      qFromLittleEndian<quint16>(data.constData() + 8)};
    }
    this->sourceSize_ = sourceSize;
    if (sourceSize.isEmpty())
    {
        this->finish(QStringLiteral("GIF has invalid dimensions"));
        return;
    }
    this->scaledSize_ = sourceSize;
    if (maxDimension > 0)
    {
        this->scaledSize_.scale(maxDimension, maxDimension,
                                Qt::KeepAspectRatio);

        this->scaledSize_ = this->scaledSize_.boundedTo(sourceSize);
        this->scaledSize_.setWidth(std::max(1, this->scaledSize_.width()));
        this->scaledSize_.setHeight(std::max(1, this->scaledSize_.height()));
    }

    this->residentBytes_ =
        int64_t(data.size()) * (isWebP ? 1 : 2) + 64 * 1024 +
        int64_t(webpChunks) * 256 +
        int64_t(sourceSize.width()) * sourceSize.height() * 4 * 3 +
        int64_t(this->scaledSize_.width()) * this->scaledSize_.height() * 4 * 2;
    if (this->residentBytes_ > maxBytes)
    {
        this->finish(QStringLiteral("GIF exceeded the image memory limit"));
        return;
    }
    this->buffer_.setData(std::move(data));
    this->buffer_.open(QIODevice::ReadOnly);
    if (isWebP)
    {
        WebPAnimDecoderOptions options{};
        if (!WebPAnimDecoderOptionsInit(&options))
        {
            this->finish(QStringLiteral("Could not initialize WebP decoder"));
            return;
        }
        options.color_mode =
            Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? MODE_bgrA : MODE_rgbA;

        const WebPData input{
            reinterpret_cast<const uint8_t *>(this->buffer_.data().constData()),
            size_t(this->buffer_.size())};
        this->webp_.reset(WebPAnimDecoderNew(&input, &options));
        WebPAnimInfo info{};
        if (!this->webp_ || !WebPAnimDecoderGetInfo(this->webp_.get(), &info) ||
            info.canvas_width != uint32_t(sourceSize.width()) ||
            info.canvas_height != uint32_t(sourceSize.height()))
        {
            this->finish(QStringLiteral("Could not read WebP animation"));
            return;
        }
        this->frameCount_ = info.frame_count;

        this->loopsRemaining_ = info.loop_count == 0 ? -1 : info.loop_count - 1;
        if (this->readWebPFrame() && (firstFrameOnly || this->frameCount_ == 1))
        {
            this->finish();
        }
        return;
    }
    if (!this->openReader())
    {
        return;
    }
    this->loopsRemaining_ = this->reader_->loopCount();
    this->frameCount_ = this->reader_->imageCount();
    if (this->readFrame() && (firstFrameOnly || this->frameCount_ == 1))
    {
        this->finish();
    }
}

bool StreamingImageDecoder::openReader()
{
    this->reader_.reset();
    if (!this->buffer_.seek(0))
    {
        this->finish(QStringLiteral("Could not rewind GIF data"));
        return false;
    }
    this->reader_ = std::make_unique<QImageReader>(&this->buffer_,
                                                   QByteArrayLiteral("gif"));
    if (!this->reader_->canRead())
    {
        this->finish(this->reader_->errorString());
        return false;
    }
    if (this->scaledSize_.isValid())
    {
        this->reader_->setScaledSize(this->scaledSize_);
    }
    return true;
}

bool StreamingImageDecoder::readFrame()
{
    auto frame = this->reader_->read();
    if (frame.isNull())
    {
        this->finish(this->reader_->errorString());
        return false;
    }
    this->current_ = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (this->current_.isNull())
    {
        this->finish(QStringLiteral("Could not allocate GIF frame"));
        return false;
    }
    auto delay = this->reader_->nextImageDelay();

    this->frameDelay_ = delay <= 10 ? 100 : std::max(20, delay);
    ++this->framesRead_;
    return true;
}

bool StreamingImageDecoder::readWebPFrame()
{
    uint8_t *pixels = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(this->webp_.get(), &pixels, &timestamp))
    {
        this->finish(QStringLiteral("Could not decode WebP frame"));
        return false;
    }
    const auto delay = timestamp - this->webpTimestamp_;
    this->webpTimestamp_ = timestamp;

    QImage borrowed(pixels, this->sourceSize_.width(),
                    this->sourceSize_.height(),
                    Q_BYTE_ORDER == Q_LITTLE_ENDIAN
                        ? QImage::Format_ARGB32_Premultiplied
                        : QImage::Format_RGBA8888_Premultiplied);
    auto frame = this->scaledSize_ == this->sourceSize_
                     ? borrowed.copy()
                     : borrowed.scaled(this->scaledSize_, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    if (frame.isNull())
    {
        this->finish(QStringLiteral("Could not allocate WebP frame"));
        return false;
    }
    this->current_ = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (this->current_.isNull())
    {
        this->finish(QStringLiteral("Could not allocate WebP frame"));
        return false;
    }
    this->frameDelay_ = delay <= 10 ? 100 : std::max(20, delay);
    ++this->framesRead_;
    return true;
}

bool StreamingImageDecoder::readNext()
{
    if (this->finished_)
    {
        return false;
    }
    if (this->webp_ ? !WebPAnimDecoderHasMoreFrames(this->webp_.get())
                    : !this->reader_->canRead())
    {
        if (this->framesRead_ < this->frameCount_)
        {
            this->finish(
                QStringLiteral("GIF ended before all frames could be decoded"));
            return false;
        }
        if (this->framesRead_ == 1 || this->loopsRemaining_ == 0)
        {
            this->finish();
            return false;
        }
        if (this->loopsRemaining_ > 0)
        {
            --this->loopsRemaining_;
        }
        if (this->webp_)
        {
            WebPAnimDecoderReset(this->webp_.get());
            this->webpTimestamp_ = 0;
        }
        else if (!this->openReader())
        {
            return false;
        }
        this->framesRead_ = 0;
    }
    return this->webp_ ? this->readWebPFrame() : this->readFrame();
}

void StreamingImageDecoder::finish(QString error)
{
    this->error_ = std::move(error);
    this->finished_ = true;
    this->reader_.reset();
    this->webp_.reset();
    this->buffer_.close();
    this->buffer_.setData({});

    this->residentBytes_ =
        int64_t(this->current_.width()) * this->current_.height() * 4;
}

QImage StreamingImageDecoder::takeFrame()
{
    return std::move(this->current_);
}

unsigned long StreamingImageDecoder::frameDelay() const
{
    return this->frameDelay_;
}

void StreamingImageDecoder::moveToThread(QThread *thread)
{
    this->buffer_.moveToThread(thread);
}

const QString &StreamingImageDecoder::error() const
{
    return this->error_;
}

bool StreamingImageDecoder::finished() const
{
    return this->finished_;
}

int64_t StreamingImageDecoder::residentBytes() const
{
    return this->residentBytes_;
}

int64_t StreamingImageDecoder::compressedBytes() const
{
    return this->buffer_.size();
}

}
