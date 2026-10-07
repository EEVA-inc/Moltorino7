#include "messages/StreamingGif.hpp"

#include "messages/StreamingImageDecoder.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QtConcurrent>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <new>

namespace chatterino::detail {
namespace {

std::atomic_bool &decoderPoolStopping()
{
    static std::atomic_bool stopping{false};
    return stopping;
}

QThreadPool &decoderPool()
{
    static QThreadPool pool;
    static const auto configured = [&] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(5000);
        QObject::connect(QCoreApplication::instance(),
                         &QCoreApplication::aboutToQuit, &pool, [] {
                             auto &workers = decoderPool();
                             decoderPoolStopping().store(
                                 true, std::memory_order_relaxed);

                             workers.waitForDone();
                         });
        return true;
    }();
    (void)configured;
    return pool;
}

struct DecodedFrame {
    QImage image;
    QString error;
    int64_t residentBytes = 0;
    int64_t compressedBytes = 0;
    unsigned long delay = 100;
    bool finished = false;
};

}

struct StreamingGif::State {
    std::unique_ptr<StreamingImageDecoder> decoder;
    QByteArray input;
    int maxDimension;
    int64_t maxBytes;
    bool firstFrameOnly;
    std::atomic_bool cancelled{false};
    std::shared_ptr<void> allocation;

    DecodedFrame next()
    {
        const auto detach = qScopeGuard([this] {
            if (this->decoder)
            {
                this->decoder->moveToThread(nullptr);
            }
        });
        if (!this->decoder)
        {
            this->decoder = std::make_unique<StreamingImageDecoder>(
                std::move(this->input), this->maxDimension, this->maxBytes,
                this->firstFrameOnly);
        }
        else
        {
            this->decoder->moveToThread(QThread::currentThread());
            this->decoder->readNext();
        }

        DecodedFrame frame{
            .image = this->decoder->takeFrame(),
            .error = this->decoder->error(),
            .residentBytes = this->decoder->residentBytes(),
            .compressedBytes = this->decoder->compressedBytes(),
            .delay = this->decoder->frameDelay(),
            .finished = this->decoder->finished(),
        };
        if (frame.finished)
        {
            this->decoder.reset();
        }
        return frame;
    }
};

struct StreamingGif::Completion {
    std::unique_ptr<QFutureWatcher<DecodedFrame>> watcher =
        std::make_unique<QFutureWatcher<DecodedFrame>>(
            QCoreApplication::instance());

    ~Completion()
    {
        this->watcher->disconnect();
        this->watcher.release()->deleteLater();
    }
};

StreamingGif::StreamingGif(QByteArray data, int maxDimension, int64_t maxBytes,
                           bool firstFrameOnly, ReadyCallback ready,
                           std::shared_ptr<void> allocation)
    : state_(std::make_shared<State>())
    , completion_(std::make_unique<Completion>())
    , ready_(std::move(ready))
    , residentBytes_(maxBytes)
    , compressedBytes_(data.size())
{
    this->state_->input = std::move(data);
    this->state_->maxDimension = maxDimension;
    this->state_->maxBytes = maxBytes;
    this->state_->firstFrameOnly = firstFrameOnly;
    this->state_->allocation = std::move(allocation);
    auto &watcher = *this->completion_->watcher;
    QObject::connect(
        &watcher, &QFutureWatcher<DecodedFrame>::finished, &watcher, [this] {
            auto frame = this->completion_->watcher->future().takeResult();
            const bool first = this->current_.isNull();
            this->decoding_ = false;
            if (!frame.image.isNull())
            {
                this->current_ = QPixmap::fromImage(std::move(frame.image));
            }
            this->error_ = std::move(frame.error);
            this->finished_ = frame.finished;
            this->frameDelay_ = frame.delay;
            this->compressedBytes_ = frame.compressedBytes;
            this->residentBytes_ = frame.finished
                                       ? int64_t(this->current_.width()) *
                                             this->current_.height() * 4
                                       : frame.residentBytes;
            if (frame.finished)
            {
                this->state_.reset();
            }
            if (this->clockStarted_ &&
                this->requestedPosition_ - this->framePosition_ >=
                    this->frameDelay_)
            {
                this->framePosition_ = this->requestedPosition_;
            }

            auto ready = this->ready_;
            if (ready)
            {
                ready(first);
            }
        });
    this->decodeNext();
}

StreamingGif::~StreamingGif()
{
    if (this->state_)
    {
        this->state_->cancelled.store(true, std::memory_order_relaxed);
    }

    this->completion_.reset();
}

void StreamingGif::decodeNext()
{
    this->decoding_ = true;
    this->completion_->watcher->setFuture(QtConcurrent::run(
        &decoderPool(), [weak = std::weak_ptr<State>(this->state_)] {
            const auto state = weak.lock();
            if (!state || state->cancelled.load(std::memory_order_relaxed) ||
                decoderPoolStopping().load(std::memory_order_relaxed))
            {
                return DecodedFrame{.finished = true};
            }
            DecodedFrame frame;
            try
            {
                frame = state->next();
            }
            catch (const std::bad_alloc &)
            {
                state->decoder.reset();
                state->input.clear();
                frame.error = QStringLiteral("Not enough memory to decode GIF");
                frame.finished = true;
            }
            if (state->cancelled.load(std::memory_order_relaxed))
            {
                return DecodedFrame{};
            }
            return frame;
        }));
}

bool StreamingGif::advance(unsigned long position)
{
    if (this->finished_ || this->current_.isNull() || this->decoding_)
    {
        return false;
    }
    if (!this->clockStarted_)
    {
        this->clockStarted_ = true;
        this->framePosition_ = position;
        return false;
    }
    if (!this->frameDue(position))
    {
        return false;
    }
    this->framePosition_ += this->frameDelay_;
    this->requestedPosition_ = position;
    this->decodeNext();
    return true;
}

bool StreamingGif::frameDue(unsigned long position) const
{
    return !this->finished_ && !this->decoding_ && this->clockStarted_ &&
           position - this->framePosition_ >= this->frameDelay_;
}

const QPixmap &StreamingGif::current() const
{
    return this->current_;
}

const QString &StreamingGif::error() const
{
    return this->error_;
}

bool StreamingGif::finished() const
{
    return this->finished_;
}

bool StreamingGif::decoding() const
{
    return this->decoding_;
}

int64_t StreamingGif::residentBytes() const
{
    return this->residentBytes_;
}

int64_t StreamingGif::compressedBytes() const
{
    return this->compressedBytes_;
}

}
