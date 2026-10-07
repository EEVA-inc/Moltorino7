#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <array>
#include <optional>
#include <utility>

namespace chatterino::tiktok {
using KeyWords = std::array<quint32, 12>;

struct CanvasCounters {
    quint32 textRequests = 3;
    quint32 textFailures = 0;
    quint32 imageRequests = 0;
    quint32 imageFailures = 0;
};

struct CapturedDynosaurFields {
    QByteArray extendedProofValue;
    QByteArray extendedScmVersion;
    QByteArray extendedWireValue;
    QByteArray location;
    QByteArray extendedBundleProofHash;
};

struct SigningState {
    SigningState(QString agent, quint32 seconds, quint32 gnarlySeedValue,
                 quint32 dynosaurSeedValue)
        : userAgent(std::move(agent))
        , timestampSeconds(seconds)
        , gnarlySeed(gnarlySeedValue)
        , dynosaurSeed(dynosaurSeedValue)
    {
    }

    QString userAgent;
    quint32 timestampSeconds;
    quint32 gnarlySeed;
    quint32 dynosaurSeed;

    quint32 environmentCode = 129;
    quint32 behaviorCode = 14;
    quint32 performanceData = 0;
    quint32 performanceMode = 1;
    bool environmentFlag = false;
    bool combinedCanvas = false;
    std::optional<CanvasCounters> canvas = CanvasCounters{};
    QString msToken;
    QString location = QStringLiteral("0");
    quint32 extendedBundleSeed = 0;
    QString extendedBundleProof;
    QString extendedScmVersion = QStringLiteral("0");

    QString extendedProofValue = QStringLiteral("0");
    std::optional<CapturedDynosaurFields> capturedDynosaurFields;
};

struct DeterministicKeys {
    KeyWords dynosaur;
    KeyWords gnarly;
};

struct SigningResult {
    QString dynosaur;
    QString gnarly;
    QString xBogus = QStringLiteral("1");
    QString signedQuery;

    QByteArray dynosaurPayload;
    QByteArray gnarlyPayload;
};

[[nodiscard]] SigningResult signExactQuery(
    const QString &query, const QString &body, const SigningState &state,
    const std::optional<DeterministicKeys> &fixtureKeys = std::nullopt);
}
