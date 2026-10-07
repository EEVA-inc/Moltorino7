#include "singletons/ThemeVideoDecoder.hpp"

#include <libyuv/convert_argb.h>
#include <QFile>
#include <QVector>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <webm/callback.h>
#include <webm/webm_parser.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace chatterino {
namespace {

constexpr int MAX_PACKET_BYTES = 2 * 1024 * 1024;
constexpr auto INVALID = webm::Status(webm::Status::kInvalidElementValue);
constexpr auto OK = webm::Status(webm::Status::kOkCompleted);

struct Packet {
    qint64 offset;
    int size;
    int time;
};

class VideoIndex final : public webm::Reader, public webm::Callback
{
public:
    explicit VideoIndex(QFile &file)
        : file_(file)
    {
    }

    QVector<Packet> packets;
    QSize size;
    int duration = 0;

    bool read()
    {
        if (!this->file_.seek(0))
        {
            return false;
        }
        webm::WebmParser parser;
        const auto status = parser.Feed(this, this);
        if ((!status.completed_ok() &&
             status.code != webm::Status::kEndOfFile) ||
            !this->ebml_ || this->tracks_ != 1 || this->packets.isEmpty())
        {
            return false;
        }

        const int last = this->packets.back().time;
        if (this->duration <= last ||
            this->duration > THEME_VIDEO_DURATION_MS ||
            this->duration - last > 5000 || this->packets.front().time != 0)
        {
            return false;
        }
        return true;
    }

    webm::Status Read(size_t count, uint8_t *buffer, uint64_t *read) override
    {
        *read = 0;
        if (count > THEME_BACKGROUND_BYTES)
        {
            return INVALID;
        }
        const auto result =
            this->file_.read(reinterpret_cast<char *>(buffer), qint64(count));
        if (result <= 0)
        {
            return webm::Status(webm::Status::kEndOfFile);
        }
        *read = uint64_t(result);
        return result == qint64(count) ? OK
                                       : webm::Status(webm::Status::kOkPartial);
    }

    webm::Status Skip(uint64_t count, uint64_t *skipped) override
    {
        *skipped = 0;
        if (count > uint64_t(this->file_.size() - this->file_.pos()) ||
            !this->file_.seek(this->file_.pos() + qint64(count)))
        {
            return INVALID;
        }
        *skipped = count;
        return OK;
    }

    uint64_t Position() const override
    {
        return uint64_t(this->file_.pos());
    }

    webm::Status OnElementBegin(const webm::ElementMetadata &metadata,
                                webm::Action *action) override
    {
        *action = webm::Action::kRead;
        if (++this->elements_ > 50000)
        {
            return INVALID;
        }

        switch (metadata.id)
        {
            case webm::Id::kEbml:
            case webm::Id::kSegment:
            case webm::Id::kInfo:
            case webm::Id::kTracks:
            case webm::Id::kTrackEntry:
            case webm::Id::kVideo:
            case webm::Id::kCluster:
            case webm::Id::kSimpleBlock:
            case webm::Id::kBlockGroup:
            case webm::Id::kBlock:
                break;
            case webm::Id::kDocType:
            case webm::Id::kTimecodeScale:
            case webm::Id::kDuration:
            case webm::Id::kTrackNumber:
            case webm::Id::kTrackType:
            case webm::Id::kCodecId:
            case webm::Id::kPixelWidth:
            case webm::Id::kPixelHeight:
            case webm::Id::kAlphaMode:
            case webm::Id::kTimecode:
                if (metadata.size > 64)
                {
                    return INVALID;
                }
                break;
            default:
                *action = webm::Action::kSkip;
                break;
        }
        return OK;
    }

    webm::Status OnEbml(const webm::ElementMetadata &,
                        const webm::Ebml &ebml) override
    {
        if (this->ebml_ || ebml.doc_type.value() != "webm")
        {
            return INVALID;
        }
        this->ebml_ = true;
        return OK;
    }

    webm::Status OnInfo(const webm::ElementMetadata &,
                        const webm::Info &info) override
    {
        if (this->duration != 0 || info.timecode_scale.value() < 1000 ||
            info.timecode_scale.value() > 1000000000)
        {
            return INVALID;
        }
        this->timeScale_ = info.timecode_scale.value();
        const double milliseconds =
            info.duration.value() * this->timeScale_ / 1e6;
        if (!std::isfinite(milliseconds) || milliseconds < 1 ||
            milliseconds > THEME_VIDEO_DURATION_MS)
        {
            return INVALID;
        }
        this->duration = qRound(milliseconds);
        return OK;
    }

    webm::Status OnTrackEntry(const webm::ElementMetadata &,
                              const webm::TrackEntry &track) override
    {
        const auto &video = track.video.value();
        const auto width = video.pixel_width.value();
        const auto height = video.pixel_height.value();
        if (++this->tracks_ != 1 || track.codec_id.value() != "V_VP8" ||
            track.track_type.value() != webm::TrackType::kVideo ||
            track.track_number.value() == 0 || video.alpha_mode.value() != 0 ||
            width < 2 || height < 2 || width > THEME_VIDEO_DIMENSION ||
            height > THEME_VIDEO_DIMENSION ||
            width * height > THEME_VIDEO_PIXELS)
        {
            return INVALID;
        }
        this->track_ = track.track_number.value();
        this->size = QSize(int(width), int(height));
        return OK;
    }

    webm::Status OnClusterBegin(const webm::ElementMetadata &,
                                const webm::Cluster &cluster,
                                webm::Action *) override
    {
        if (this->timeScale_ == 0 || this->tracks_ != 1 ||
            cluster.timecode.value() >
                uint64_t(THEME_VIDEO_DURATION_MS) * 1000000 / this->timeScale_)
        {
            return INVALID;
        }
        this->cluster_ = qint64(cluster.timecode.value());
        return OK;
    }

    webm::Status OnBlockBegin(const webm::ElementMetadata &,
                              const webm::Block &block, webm::Action *) override
    {
        const auto ticks = this->cluster_ + block.timecode;
        if (block.track_number != this->track_ || block.num_frames != 1 ||
            ticks < 0)
        {
            return INVALID;
        }
        const auto time = ticks * qint64(this->timeScale_) / 1000000;
        if (time >= THEME_VIDEO_DURATION_MS ||
            (!this->packets.isEmpty() && time - this->packets.back().time < 40))
        {
            return INVALID;
        }
        this->time_ = int(time);
        return OK;
    }

    webm::Status OnSimpleBlockBegin(const webm::ElementMetadata &metadata,
                                    const webm::SimpleBlock &block,
                                    webm::Action *action) override
    {
        return this->OnBlockBegin(metadata, block, action);
    }

    webm::Status OnFrame(const webm::FrameMetadata &metadata,
                         webm::Reader *reader, uint64_t *remaining) override
    {
        if (metadata.size < 3 || metadata.size > MAX_PACKET_BYTES ||
            this->packets.size() >= THEME_VIDEO_DURATION_MS / 40 ||
            metadata.position > uint64_t(this->file_.size()) ||
            metadata.size > uint64_t(this->file_.size()) - metadata.position)
        {
            return INVALID;
        }
        this->packets.push_back(
            {qint64(metadata.position), int(metadata.size), this->time_});
        return webm::Callback::OnFrame(metadata, reader, remaining);
    }

private:
    QFile &file_;
    uint64_t timeScale_ = 0;
    uint64_t track_ = 0;
    qint64 cluster_ = 0;
    int time_ = 0;
    int tracks_ = 0;
    int elements_ = 0;
    bool ebml_ = false;
};

}

struct ThemeVideoDecoder::Private {
    explicit Private(const QString &source)
        : file(source)
    {
        this->file.moveToThread(nullptr);
    }
    ~Private()
    {
        if (this->initialized)
        {
            vpx_codec_destroy(&this->codec);
        }
    }
    QFile file;
    QVector<Packet> packets;
    QByteArray bytes;
    QSize size;
    vpx_codec_ctx_t codec{};
    vpx_image_t *frame = nullptr;
    QString error;
    int next = 0;
    int duration = 0;
    int frameDuration = 0;
    bool initialized = false;
};

bool isThemeVideo(const QString &source)
{
    return source.endsWith(QLatin1String(".webm"), Qt::CaseInsensitive);
}

ThemeVideoDecoder::ThemeVideoDecoder(const QString &source)
    : data_(std::make_unique<Private>(source))
{
}
ThemeVideoDecoder::~ThemeVideoDecoder() = default;

bool ThemeVideoDecoder::open()
{
    auto &d = *this->data_;
    if (d.initialized)
    {
        return true;
    }
    if (!d.file.open(QIODevice::ReadOnly) ||
        d.file.size() > THEME_BACKGROUND_BYTES)
    {
        d.error =
            QStringLiteral("Moltorino couldn't read that video background.");
        return false;
    }
    VideoIndex index(d.file);
    if (!index.read())
    {
        d.error = QStringLiteral(
            "That video background is invalid or exceeds its limits.");
        return false;
    }
    d.packets = std::move(index.packets);
    d.size = index.size;
    d.duration = index.duration;
    vpx_codec_dec_cfg_t config{};
    config.threads = 1;
    if (vpx_codec_dec_init(&d.codec, vpx_codec_vp8_dx(), &config, 0) !=
        VPX_CODEC_OK)
    {
        d.error =
            QStringLiteral("Moltorino couldn't start the background decoder.");
        return false;
    }
    d.initialized = true;
    return true;
}

bool ThemeVideoDecoder::decodeNext()
{
    auto &d = *this->data_;
    if (!d.error.isEmpty() || (!d.initialized && !this->open()))
    {
        return false;
    }
    const auto &packet = d.packets[d.next];
    d.bytes.resize(packet.size);
    const auto fail = [&] {
        d.frame = nullptr;
        d.error =
            QStringLiteral("Moltorino couldn't decode that video background.");
        return false;
    };
    if (!d.file.seek(packet.offset) ||
        d.file.read(d.bytes.data(), packet.size) != packet.size)
    {
        return fail();
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(d.bytes.constData());
    const bool key = (bytes[0] & 1) == 0;
    if (!(bytes[0] & 0x10) || (d.next == 0 && !key))
    {
        return fail();
    }

    if (key)
    {
        vpx_codec_stream_info_t info{};
        info.sz = sizeof(info);
        if (vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), bytes, packet.size,
                                       &info) != VPX_CODEC_OK ||
            info.w != unsigned(d.size.width()) ||
            info.h != unsigned(d.size.height()))
        {
            return fail();
        }
    }
    if (vpx_codec_decode(&d.codec, bytes, packet.size, nullptr, 0) !=
        VPX_CODEC_OK)
    {
        return fail();
    }
    vpx_codec_iter_t iterator = nullptr;
    d.frame = vpx_codec_get_frame(&d.codec, &iterator);
    if (!d.frame || d.frame->fmt != VPX_IMG_FMT_I420 ||
        d.frame->d_w != unsigned(d.size.width()) ||
        d.frame->d_h != unsigned(d.size.height()))
    {
        return fail();
    }
    const int end =
        d.next + 1 < d.packets.size() ? d.packets[d.next + 1].time : d.duration;
    d.frameDuration = end - packet.time;
    d.next = (d.next + 1) % d.packets.size();
    return true;
}

bool ThemeVideoDecoder::copyFrame(QImage &image) const
{
    const auto &d = *this->data_;
    if (!d.frame)
    {
        return false;
    }
    if (image.size() != d.size || image.format() != QImage::Format_RGB32)
    {
        image = QImage(d.size, QImage::Format_RGB32);
    }
    if (image.isNull())
    {
        return false;
    }
    const auto *f = d.frame;
    return libyuv::I420ToARGB(f->planes[0], f->stride[0], f->planes[1],
                              f->stride[1], f->planes[2], f->stride[2],
                              image.bits(), int(image.bytesPerLine()),
                              d.size.width(), d.size.height()) == 0;
}

int ThemeVideoDecoder::frameDuration() const
{
    return this->data_->frameDuration;
}
int ThemeVideoDecoder::frameCount() const
{
    return int(this->data_->packets.size());
}
QString ThemeVideoDecoder::error() const
{
    return this->data_->error;
}

QImage themeVideoPoster(const QString &source, int maxDimension)
{
    ThemeVideoDecoder decoder(source);
    QImage image;
    if (!decoder.decodeNext() || !decoder.copyFrame(image))
    {
        return {};
    }
    maxDimension = std::clamp(maxDimension, 1, THEME_VIDEO_DIMENSION);
    if (image.width() > maxDimension || image.height() > maxDimension)
    {
        image = image.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

QString validateThemeVideo(const QString &source, std::stop_token cancellation)
{
    ThemeVideoDecoder decoder(source);
    if (!decoder.open())
    {
        return decoder.error();
    }
    for (int i = 0; i < decoder.frameCount(); ++i)
    {
        if (cancellation.stop_requested())
        {
            return QStringLiteral("Background preparation cancelled.");
        }
        if (!decoder.decodeNext())
        {
            return decoder.error();
        }
    }
    return {};
}

}
