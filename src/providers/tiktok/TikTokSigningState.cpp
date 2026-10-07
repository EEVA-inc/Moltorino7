#include "providers/tiktok/TikTokSigningState.hpp"

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace chatterino::tiktok {
namespace {
constexpr char Alphabet[] =
    "u09tbS3UvgDEe6r-ZVMXzLpsAohTn7mdINQlW412GqBjfYiyk8JORCF5/xKHwacP";
constexpr char Standard[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::array<quint32, 4> Sigma{1196819126U, 600974999U, 3863347763U,
                                       1451689750U};
constexpr quint32 FrozenTime = 1767225600U;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::invalid_argument(message);
    }
}

quint8 byteAt(const QByteArray &bytes, qsizetype index)
{
    return static_cast<quint8>(bytes[index]);
}

quint32 bigInteger(const QByteArray &bytes)
{
    require(bytes.size() == 2 || bytes.size() == 4,
            "Invalid integer field width");
    quint32 value = 0;
    for (const unsigned char byte : bytes)
    {
        value = (value << 8) | byte;
    }
    require(bytes.size() == 2 || value > 65535U,
            "Noncanonical integer field width");
    return value;
}

void quarter(std::array<quint32, 16> &state, size_t a, size_t b, size_t c,
             size_t d)
{
    state[a] += state[b];
    state[d] = std::rotl(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = std::rotl(state[b] ^ state[c], 12);
    state[a] += state[b];
    state[d] = std::rotl(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = std::rotl(state[b] ^ state[c], 7);
}

QByteArray applyStream(QByteArray bytes, const KeyWords &keys)
{
    std::array<quint32, 16> state{};
    std::copy(Sigma.begin(), Sigma.end(), state.begin());
    std::copy(keys.begin(), keys.end(), state.begin() + 4);
    quint32 lowNibbles = 0;
    for (const auto key : keys)
    {
        lowNibbles += key & 15U;
    }
    const quint32 rounds = 5U + (lowNibbles & 15U);
    for (qsizetype offset = 0; offset < bytes.size(); offset += 64)
    {
        auto work = state;
        for (quint32 pass = 0; pass < rounds; ++pass)
        {
            if ((pass & 1U) == 0)
            {
                quarter(work, 0, 4, 8, 12);
                quarter(work, 1, 5, 9, 13);
                quarter(work, 2, 6, 10, 14);
                quarter(work, 3, 7, 11, 15);
            }
            else
            {
                quarter(work, 0, 5, 10, 15);
                quarter(work, 1, 6, 11, 12);
                quarter(work, 2, 7, 12, 13);
                quarter(work, 3, 4, 13, 14);
            }
        }
        for (size_t i = 0; i < work.size(); ++i)
        {
            work[i] += state[i];
        }
        for (qsizetype i = 0; i < 64 && offset + i < bytes.size(); ++i)
        {
            bytes[offset + i] = static_cast<char>(
                byteAt(bytes, offset + i) ^
                ((work[static_cast<size_t>(i >> 2)] >> (8 * (i & 3))) & 255U));
        }
        ++state[12];
    }
    return bytes;
}

QString lowText(const QByteArray &bytes, bool variantB = false)
{
    require(bytes.size() >= 6, "Encoded text field is too short");
    const qsizetype length = (byteAt(bytes, bytes.size() - 2) << 8) |
                             byteAt(bytes, bytes.size() - 1);
    require(bytes.size() == std::max<qsizetype>(length + 2, 6),
            "Encoded text length mismatch");
    QString text;
    text.reserve(length);
    for (qsizetype i = 0; i < length; ++i)
    {
        const quint32 index = static_cast<quint32>(i);
        quint32 value =
            ((byteAt(bytes, i) - (variantB ? 0U : 1U)) & 255U) ^ 187U;
        if (variantB)
        {
            value = ((value >> 1) | (value << 7)) & 255U;
            value ^= 165U;
        }
        else
        {
            value = ((value >> 2) | (value << 6)) & 255U;
        }
        value = ((value - (variantB ? 0U : 1U) - (170U & index)) & 255U) ^
                (((variantB ? 102U : 103U) + index) & 255U);
        text.append(QChar(static_cast<ushort>(value)));
    }
    for (qsizetype i = length; i < bytes.size() - 2; ++i)
    {
        require(byteAt(bytes, i) == static_cast<quint8>((221 + i) & 255),
                "Encoded text padding mismatch");
    }
    return text;
}

quint64 textNumber(const QByteArray &bytes, bool variantB = false)
{
    const QString text = lowText(bytes, variantB);
    bool ok = false;
    const quint64 number = text.toULongLong(&ok);
    require(ok && QString::number(number) == text,
            "Invalid canonical decimal field");
    return number;
}

quint32 textUint32(const QByteArray &bytes)
{
    const quint64 value = textNumber(bytes);
    require(value <= std::numeric_limits<quint32>::max(),
            "Decimal field exceeds uint32");
    return static_cast<quint32>(value);
}

quint32 mixedState(quint32 timestamp, quint32 seed, quint32 environment)
{
    return (((timestamp >> 16) ^ (seed >> 16) ^ timestamp ^ seed) & 65535U) |
           (environment << 16);
}

quint32 derivedState(quint32 timestamp, quint32 seed, quint32 environment,
                     quint32 performance, bool flag, bool performanceMode)
{
    const quint32 low = (flag || (environment & 32U)) ? FrozenTime : timestamp;
    const quint32 high = performanceMode ? performance : timestamp;
    return ((low ^ seed) & 65535U) | (((high ^ (seed >> 16)) & 65535U) << 16);
}

template <size_t Count>
std::array<QByteArray, Count> parseFields(
    const QByteArray &payload, qsizetype start, quint8 firstKey,
    bool fixedOrder, std::array<quint8, Count> *order = nullptr)
{
    std::array<QByteArray, Count> fields;
    std::array<bool, Count> seen{};
    qsizetype position = start;
    for (size_t index = 0; index < Count; ++index)
    {
        require(payload.size() - position >= 3, "Truncated field header");
        const quint8 key = byteAt(payload, position++);
        const qsizetype length =
            (byteAt(payload, position) << 8) | byteAt(payload, position + 1);
        position += 2;
        require(key >= firstKey && static_cast<size_t>(key - firstKey) < Count,
                "Unknown payload field");
        const size_t slot = key - firstKey;
        require(!seen[slot] && (!fixedOrder || slot == index),
                "Duplicate or reordered payload field");
        require(length > 0 && length <= payload.size() - position,
                "Invalid field length");
        seen[slot] = true;
        fields[slot] = payload.sliced(position, length);
        if (order)
        {
            (*order)[index] = key;
        }
        position += length;
    }
    require(position == payload.size(), "Trailing payload data");
    return fields;
}

QByteArray fnv(const QString &text)
{
    quint32 hash = 2166136260U;
    for (const unsigned char byte : text.toUtf8())
    {
        const quint32 value = (hash ^ byte) * 16777619U;
        hash = value + value * 32U;
    }
    QByteArray result(4, '\0');
    for (int i = 0; i < 4; ++i)
    {
        result[i] = static_cast<char>((hash >> ((3 - i) * 8)) & 255U);
    }
    return result;
}

void validateUserAgent(const QString &userAgent)
{
    require(!userAgent.isEmpty(), "Missing matching User-Agent");
    for (qsizetype i = 0; i < userAgent.size(); ++i)
    {
        if (userAgent[i].isHighSurrogate())
        {
            require(++i < userAgent.size() && userAgent[i].isLowSurrogate(),
                    "Malformed User-Agent UTF16");
        }
        else
        {
            require(!userAgent[i].isLowSurrogate(),
                    "Malformed User-Agent UTF16");
        }
    }
}

bool hexText(const QByteArray &bytes, qsizetype length)
{
    return bytes.size() == length &&
           std::all_of(bytes.begin(), bytes.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

quint32 numericString(const QByteArray &bytes)
{
    bool ok = false;
    const double value = bytes.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value == 0)
    {
        return 0;
    }
    const double remainder = std::fmod(std::trunc(value), 4294967296.0);
    return static_cast<quint32>(remainder < 0 ? remainder + 4294967296.0
                                              : remainder);
}

void validateGnarly(const std::array<QByteArray, 17> &fields,
                    const std::array<quint8, 17> &observedOrder)
{
    quint32 numericChecksum = 0;
    quint32 prefixChecksum = 0;
    for (size_t i = 1; i < fields.size(); ++i)
    {
        const bool text = i == 3 || i == 4 || i == 5 || i == 9 || i == 10;
        if (text)
        {
            require(i >= 9 || hexText(fields[i], 32), "Invalid MD5 field");
            numericChecksum ^= numericString(fields[i]);
            quint32 prefix = 0;
            for (qsizetype j = 0; j < std::min<qsizetype>(4, fields[i].size());
                 ++j)
            {
                prefix = (prefix << 8) | byteAt(fields[i], j);
            }
            prefixChecksum ^= prefix;
        }
        else
        {
            const quint32 value = bigInteger(fields[i]);
            numericChecksum ^= value;
            if (i < 16)
            {
                prefixChecksum ^= value;
            }
        }
    }
    require(numericChecksum == bigInteger(fields[0]) &&
                prefixChecksum == bigInteger(fields[16]),
            "Gnarly checksum mismatch");
    require(fields[9] == "5.3.2" && fields[10] == "1.0.0.417" &&
                bigInteger(fields[11]) == 1,
            "Unsupported Gnarly version or constants");
    std::array<quint8, 17> expectedOrder{};
    for (size_t i = 0; i < expectedOrder.size(); ++i)
    {
        expectedOrder[i] = static_cast<quint8>(i);
    }
    quint32 lcg = bigInteger(fields[8]);
    for (size_t i = expectedOrder.size() - 1; i > 0; --i)
    {
        lcg = 1664525U * lcg + 1013904223U;
        const size_t j = static_cast<size_t>((quint64{lcg} * (i + 1)) >> 32);
        std::swap(expectedOrder[i], expectedOrder[j]);
    }
    require(expectedOrder == observedOrder, "Gnarly field shuffle mismatch");
}

void validateDynosaur(const std::array<QByteArray, 25> &fields)
{
    quint32 checksum = 222;
    for (size_t i = 1; i < fields.size(); ++i)
    {
        require(fields[i].size() >= 2, "Dynosaur field is too short");
        checksum ^= byteAt(fields[i], 1);
    }
    require(checksum == textNumber(fields[0], true),
            "Dynosaur checksum mismatch");
    require(lowText(fields[1], true) == QStringLiteral("1") &&
                lowText(fields[2], true) == QStringLiteral("1") &&
                lowText(fields[10]) == QStringLiteral("5.3.2") &&
                lowText(fields[17]) == QStringLiteral("1.0.0.417"),
            "Unsupported Dynosaur version or constants");
    for (const size_t i : {size_t{3}, size_t{9}, size_t{13}, size_t{23}})
    {
        require(textNumber(fields[i]) == 0, "Unknown Dynosaur reserved field");
    }
    for (const size_t i : {size_t{11}, size_t{14}, size_t{16}, size_t{24}})
    {
        require(fields[i].size() == 4, "Invalid Dynosaur hash width");
    }
    for (const size_t i : {size_t{8}, size_t{18}, size_t{19}, size_t{21}})
    {
        (void)lowText(fields[i]);
    }
    const auto wire = lowText(fields[19]).toLatin1();
    require(wire == "0" || hexText(wire, 32), "Unknown extended wire format");
}
}

DecodedEnvelope decodeSignatureEnvelope(const QString &signature)
{
    require(signature.size() >= 68 && signature.size() <= 1024 * 1024 &&
                signature.size() % 4 == 0,
            "Invalid signature size");
    QByteArray standard;
    standard.reserve(signature.size());
    bool padding = false;
    int paddingCount = 0;
    for (const auto letter : signature)
    {
        if (letter == u'=')
        {
            padding = true;
            require(++paddingCount <= 2, "Invalid signature padding");
            standard.append('=');
            continue;
        }
        require(!padding && letter.unicode() < 128,
                "Invalid signature character");
        const char byte = letter.toLatin1();
        const auto *found =
            std::find(std::begin(Alphabet), std::end(Alphabet) - 1, byte);
        require(found != std::end(Alphabet) - 1, "Unknown signature alphabet");
        standard.append(Standard[found - Alphabet]);
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        standard, QByteArray::AbortOnBase64DecodingErrors);
    require(bool(decoded) && decoded.decoded.toBase64() == standard,
            "Invalid canonical base64 envelope");
    const auto &bytes = decoded.decoded;
    require(bytes.size() > 49 && byteAt(bytes, 0) == 75,
            "Unsupported signature envelope");
    const qsizetype cipherLength = bytes.size() - 49;
    quint64 sum = 0;
    for (qsizetype i = 1; i < bytes.size(); ++i)
    {
        sum += byteAt(bytes, i);
    }
    const qsizetype insert =
        static_cast<qsizetype>(sum % static_cast<quint64>(cipherLength + 1));
    KeyWords keys{};
    for (size_t i = 0; i < keys.size(); ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            keys[i] |=
                quint32{byteAt(bytes,
                               1 + insert + static_cast<qsizetype>(i * 4) + j)}
                << (8 * j);
        }
    }
    const QByteArray cipher =
        bytes.sliced(1, insert) + bytes.sliced(49 + insert);
    return {applyStream(cipher, keys), keys};
}

RecoveredSigningState recoverSigningState(const QString &dynosaur,
                                          const QString &gnarly,
                                          const QString &userAgent)
{
    validateUserAgent(userAgent);
    const auto dyn = decodeSignatureEnvelope(dynosaur);
    const auto gna = decodeSignatureEnvelope(gnarly);
    const auto d = parseFields<25>(dyn.payload, 0, 32, true);
    require(!gna.payload.isEmpty() && byteAt(gna.payload, 0) == 17,
            "Unsupported Gnarly field count");
    std::array<quint8, 17> order{};
    const auto g = parseFields<17>(gna.payload, 1, 0, false, &order);
    validateDynosaur(d);
    validateGnarly(g, order);
    require(d[16] == fnv(userAgent) &&
                g[5] == QCryptographicHash::hash(userAgent.toUtf8(),
                                                 QCryptographicHash::Md5)
                            .toHex(),
            "User-Agent hashes do not match");
    const quint32 dynTimestamp = textUint32(d[7]);
    const quint32 gnarlyTimestamp = bigInteger(g[6]);
    const quint32 dynSeed = textUint32(d[20]);
    const quint32 gnaSeed = bigInteger(g[8]);
    SigningState snapshot(userAgent, gnarlyTimestamp, gnaSeed, dynSeed);
    snapshot.environmentCode = textUint32(d[6]);
    snapshot.behaviorCode = textUint32(d[22]);
    snapshot.performanceData = textUint32(d[12]);
    require(snapshot.environmentCode == bigInteger(g[1]) &&
                snapshot.behaviorCode == bigInteger(g[2]) &&
                snapshot.performanceData == bigInteger(g[7]),
            "Signature pair has inconsistent state");
    require(mixedState(gnarlyTimestamp, gnaSeed, snapshot.environmentCode) ==
                bigInteger(g[14]),
            "Gnarly mixed state mismatch");

    const quint64 text = textNumber(d[15]);
    const quint64 images = textNumber(d[5]);
    constexpr quint64 MaxCounter = std::numeric_limits<quint32>::max();
    require(text <= MaxCounter && images <= MaxCounter &&
                static_cast<quint32>(text) == bigInteger(g[12]) &&
                g[12].size() == (text <= 65535U ? 2 : 4),
            "Unsupported or inconsistent canvas totals");

    snapshot.canvas = CanvasCounters{static_cast<quint32>(text), 0,
                                     static_cast<quint32>(images), 0};
    quint8 canvasCandidates = 0;
    for (quint8 mode = 0; mode < 2; ++mode)
    {
        const quint64 total = images + (mode ? text : 0);
        if (total <= MaxCounter &&
            static_cast<quint32>(total) == bigInteger(g[13]) &&
            g[13].size() == (total <= 65535U ? 2 : 4))
        {
            canvasCandidates |= static_cast<quint8>(1U << mode);
        }
    }
    require(canvasCandidates != 0, "Canvas mode does not match");
    snapshot.combinedCanvas = canvasCandidates == 2;

    quint8 flagCandidates = 0;
    quint8 performanceCandidates = 0;
    const quint32 observedDynMixed = textUint32(d[4]);
    const quint32 observedDerived = bigInteger(g[15]);
    for (quint8 flag = 0; flag < 2; ++flag)
    {
        const quint32 mixed =
            flag ? snapshot.environmentCode << 16
                 : mixedState(dynTimestamp, dynSeed, snapshot.environmentCode);
        if (mixed != observedDynMixed)
        {
            continue;
        }
        for (quint8 mode = 0; mode < 2; ++mode)
        {
            if (derivedState(gnarlyTimestamp, gnaSeed, snapshot.environmentCode,
                             snapshot.performanceData, flag != 0,
                             mode != 0) == observedDerived)
            {
                flagCandidates |= static_cast<quint8>(1U << flag);
                performanceCandidates |= static_cast<quint8>(1U << mode);
            }
        }
    }
    require(flagCandidates != 0 && performanceCandidates != 0,
            "No compatible environment or performance mode");
    snapshot.environmentFlag = flagCandidates == 2;
    snapshot.performanceMode = performanceCandidates == 2 ? 2U : 1U;
    snapshot.capturedDynosaurFields =
        CapturedDynosaurFields{d[8], d[18], d[19], d[21], d[24]};
    return {std::move(snapshot),
            dynTimestamp,
            gnarlyTimestamp,
            text,
            images,
            flagCandidates,
            performanceCandidates,
            canvasCandidates,
            lowText(d[19]) == QStringLiteral("0")};
}

SigningState RecoveredSigningState::atTimestamp(quint32 newTime,
                                                const QString &msToken) const
{
    if (environmentFlagCandidates == 3)
    {
        require(mixedState(newTime, snapshot.dynosaurSeed,
                           snapshot.environmentCode) ==
                    (snapshot.environmentCode << 16),
                "Environment flag is ambiguous at requested time");
        require(
            derivedState(newTime, snapshot.gnarlySeed, snapshot.environmentCode,
                         snapshot.performanceData, false,
                         snapshot.performanceMode == 2) ==
                derivedState(newTime, snapshot.gnarlySeed,
                             snapshot.environmentCode, snapshot.performanceData,
                             true, snapshot.performanceMode == 2),
            "Environment flag is ambiguous at requested time");
    }
    if (performanceModeCandidates == 3)
    {
        require((newTime & 65535U) == (snapshot.performanceData & 65535U),
                "Performance mode is ambiguous at requested time");
    }
    SigningState state = snapshot;
    state.timestampSeconds = newTime;
    state.msToken = msToken;
    return state;
}
}
