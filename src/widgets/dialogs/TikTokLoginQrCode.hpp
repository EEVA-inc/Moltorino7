#pragma once

#include <QImage>

namespace chatterino {

QImage renderTikTokLoginQr(const QImage &source, const QImage &logo,
                          int pixelSize);

}
