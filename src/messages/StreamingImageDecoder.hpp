#pragma once

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <webp/demux.h>

#include <cstdint>
#include <memory>

namespace chatterino::detail {

class StreamingImageDecoder
{
public:
    StreamingImageDecoder(QByteArray data, int maxDimension, int64_t maxBytes,
                          bool firstFrameOnly = false);

    QImage takeFrame();
    unsigned long frameDelay() const;
    void moveToThread(QThread *thread);
    const QString &error() const;
    bool finished() const;
    int64_t residentBytes() const;
    int64_t compressedBytes() const;

    bool readNext();

private:
    bool openReader();
    bool readFrame();
    bool readWebPFrame();
    void finish(QString error = {});

    QBuffer buffer_;
    std::unique_ptr<QImageReader> reader_;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> webp_{
        nullptr, WebPAnimDecoderDelete};
    QImage current_;
    QSize sourceSize_;
    QSize scaledSize_;
    QString error_;
    int64_t residentBytes_ = 0;
    unsigned long frameDelay_ = 100;
    int loopsRemaining_ = 0;
    int frameCount_ = 0;
    int framesRead_ = 0;
    int webpTimestamp_ = 0;
    bool finished_ = false;
};

}
