#include "TikTokLoginQrCode.hpp"

#include "qrcodegen.hpp"
#include "quirc.h"

#include <QByteArray>
#include <QPainter>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace chatterino {
namespace {
std::optional<QByteArray> decode(const QImage &source)
{
    if (source.isNull() || source.width() > 2048 || source.height() > 2048)
    {
        return std::nullopt;
    }

    const auto border = std::max(16, source.width() / 8);
    QImage image(source.width() + border * 2, source.height() + border * 2,
                 QImage::Format_Grayscale8);
    image.fill(Qt::white);
    QPainter background(&image);
    background.drawImage(border, border, source);
    background.end();
    std::unique_ptr<quirc, decltype(&quirc_destroy)> reader(quirc_new(),
                                                            &quirc_destroy);
    if (!reader || quirc_resize(reader.get(), image.width(), image.height()))
    {
        return std::nullopt;
    }
    auto *pixels = quirc_begin(reader.get(), nullptr, nullptr);
    for (int y = 0; y < image.height(); ++y)
    {
        std::memcpy(pixels + y * image.width(), image.constScanLine(y),
                    image.width());
    }
    quirc_end(reader.get());
    if (quirc_count(reader.get()) != 1)
    {
        return std::nullopt;
    }
    quirc_code code{};
    quirc_data data{};
    quirc_extract(reader.get(), 0, &code);
    if (quirc_decode(&code, &data) != QUIRC_SUCCESS || data.payload_len < 1 ||
        data.payload_len > 2048)
    {
        return std::nullopt;
    }
    return QByteArray(reinterpret_cast<const char *>(data.payload),
                      data.payload_len);
}
}

QImage renderTikTokLoginQr(const QImage &source, const QImage &logo,
                           int pixelSize)
{
    if (source.isNull() || pixelSize < 128 || pixelSize > 2048)
    {
        return {};
    }
    const auto fallback = [&] {
        return source.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio,
                             Qt::FastTransformation);
    };
    const auto payload = decode(source);
    if (!payload || logo.isNull())
    {
        return fallback();
    }
    try
    {
        const std::vector<uint8_t> bytes(payload->begin(), payload->end());
        const auto code = qrcodegen::QrCode::encodeBinary(
            bytes, qrcodegen::QrCode::Ecc::HIGH);
        const auto size = code.getSize();
        const auto unit = pixelSize / (size + 8);
        if (unit < 3)
        {
            return fallback();
        }
        const auto offset = (pixelSize - size * unit) / 2;
        QImage result(pixelSize, pixelSize, QImage::Format_RGB32);
        result.fill(Qt::white);
        QPainter painter(&result);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                if (code.getModule(x, y))
                {
                    painter.fillRect(offset + x * unit, offset + y * unit,
                                     unit, unit, Qt::black);
                }
            }
        }

        const auto modules = std::clamp((size / 5) | 1, 5, 9);
        const auto start = offset + ((size - modules) / 2) * unit;
        const QRect backing(start, start, modules * unit, modules * unit);
        painter.fillRect(backing, Qt::white);
        const auto icon = backing.adjusted(unit, unit, -unit, -unit);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(icon, logo);
        painter.end();
        if (decode(result) == payload)
        {
            return result;
        }
    }
    catch (const std::length_error &)
    {
    }
    return fallback();
}
}
