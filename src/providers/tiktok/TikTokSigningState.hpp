#pragma once

#include "providers/tiktok/TikTokSigning.hpp"

namespace chatterino::tiktok {
struct DecodedEnvelope {
    QByteArray payload;
    KeyWords keys;
};

struct RecoveredSigningState {
    SigningState snapshot;
    quint32 dynosaurTimestamp;
    quint32 gnarlyTimestamp;
    quint64 textCount;
    quint64 imageCount;

    quint8 environmentFlagCandidates;
    quint8 performanceModeCandidates;
    quint8 combinedCanvasCandidates;

    bool extendedBundleSeedKnown;

    [[nodiscard]] SigningState atTimestamp(quint32 newTime,
                                           const QString &msToken) const;
};

[[nodiscard]] DecodedEnvelope decodeSignatureEnvelope(const QString &signature);
[[nodiscard]] RecoveredSigningState recoverSigningState(
    const QString &dynosaur, const QString &gnarly, const QString &userAgent);
}
