#pragma once

#include <QPixmap>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace chatterino::detail {

class StreamingGif
{
public:
    using ReadyCallback = std::function<void(bool firstFrame)>;

    StreamingGif(QByteArray data, int maxDimension, int64_t maxBytes,
                 bool firstFrameOnly = false, ReadyCallback ready = {},
                 std::shared_ptr<void> allocation = {});
    ~StreamingGif();

    const QPixmap &current() const;
    const QString &error() const;
    bool finished() const;
    bool decoding() const;
    int64_t residentBytes() const;
    int64_t compressedBytes() const;

    bool advance(unsigned long position);
    bool frameDue(unsigned long position) const;

private:
    struct State;
    struct Completion;
    void decodeNext();

    std::shared_ptr<State> state_;
    std::unique_ptr<Completion> completion_;
    QPixmap current_;
    QString error_;
    ReadyCallback ready_;
    int64_t residentBytes_ = 0;
    int64_t compressedBytes_ = 0;
    unsigned long framePosition_ = 0;
    unsigned long requestedPosition_ = 0;
    unsigned long frameDelay_ = 100;
    bool clockStarted_ = false;
    bool finished_ = false;
    bool decoding_ = false;
};

}
