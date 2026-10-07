#include "providers/tiktok/TikTokSigning.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStringConverter>
#include <QStringList>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <variant>

namespace chatterino::tiktok {
namespace {
using Field = std::variant<quint64, QByteArray>;
constexpr std::array<quint32, 4> Sigma{1196819126U, 600974999U, 3863347763U,
                                       1451689750U};
constexpr char Alphabet[] =
    "u09tbS3UvgDEe6r-ZVMXzLpsAohTn7mdINQlW412GqBjfYiyk8JORCF5/xKHwacP";
constexpr char StandardAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void requireWellFormed(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i)
    {
        if (text[i].isHighSurrogate())
        {
            if (++i == text.size() || !text[i].isLowSurrogate())
            {
                throw std::invalid_argument("Malformed UTF16 input");
            }
        }
        else if (text[i].isLowSurrogate())
        {
            throw std::invalid_argument("Malformed UTF16 input");
        }
    }
}

void appendBig16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value & 255U));
}

void appendBig32(QByteArray &out, quint32 value)
{
    appendBig16(out, static_cast<quint16>(value >> 16));
    appendBig16(out, static_cast<quint16>(value));
}

QByteArray integerBytes(quint64 value)
{
    QByteArray bytes;
    if (value <= 65535U)
    {
        appendBig16(bytes, static_cast<quint16>(value));
    }
    else
    {
        appendBig32(bytes, static_cast<quint32>(value));
    }
    return bytes;
}

void appendField(QByteArray &out, quint8 key, const QByteArray &value)
{
    if (value.size() > std::numeric_limits<quint16>::max())
    {
        throw std::invalid_argument("Encoded field exceeds BE16 length limit");
    }
    out.append(static_cast<char>(key));
    appendBig16(out, static_cast<quint16>(value.size()));
    out.append(value);
}

QByteArray md5(const QString &text)
{
    return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Md5)
        .toHex();
}

QByteArray fnv(const QString &text)
{
    quint32 hash = 2166136260U;
    for (const unsigned char byte : text.toUtf8())
    {
        const quint32 value = (hash ^ byte) * 16777619U;
        hash = value + value * 32U;
    }
    QByteArray out;
    appendBig32(out, hash);
    return out;
}

QByteArray encodeText(const QString &text, bool variantB = false)
{
    const qsizetype length = text.size();
    if (length > 65533)
    {
        throw std::invalid_argument("Encoded text exceeds BE16 field limit");
    }
    const qsizetype size = std::max<qsizetype>(length + 2, 6);
    QByteArray out(size, '\0');
    for (qsizetype i = 0; i < length; ++i)
    {
        const quint32 index = static_cast<quint32>(i);
        quint32 value =
            (text[i].unicode() ^ ((variantB ? 102U : 103U) + index)) +
            (variantB ? 0U : 1U) + (170U & index);
        value &= 255U;
        if (variantB)
        {
            value ^= 165U;
            value = ((value << 1) | (value >> 7)) & 255U;
        }
        else
        {
            value = ((value << 2) | (value >> 6)) & 255U;
        }
        out[i] =
            static_cast<char>(((value ^ 187U) + (variantB ? 0U : 1U)) & 255U);
    }
    for (qsizetype i = length; i < size - 2; ++i)
    {
        out[i] = static_cast<char>((221 + i) & 255);
    }
    out[size - 2] = static_cast<char>((length >> 8) & 255);
    out[size - 1] = static_cast<char>(length & 255);
    return out;
}

void validateCapturedFields(const CapturedDynosaurFields &fields)
{
    for (const auto *encoded :
         {&fields.extendedProofValue, &fields.extendedScmVersion,
          &fields.extendedWireValue, &fields.location})
    {
        if (encoded->size() < 6 || encoded->size() > 65535)
        {
            throw std::invalid_argument("Invalid captured text field size");
        }
        const qsizetype size = encoded->size();
        const qsizetype length =
            (static_cast<unsigned char>((*encoded)[size - 2]) << 8) |
            static_cast<unsigned char>((*encoded)[size - 1]);
        if (size != std::max<qsizetype>(length + 2, 6))
        {
            throw std::invalid_argument("Invalid captured text field length");
        }
        for (qsizetype i = length; i < size - 2; ++i)
        {
            if (static_cast<unsigned char>((*encoded)[i]) !=
                static_cast<unsigned char>((221 + i) & 255))
            {
                throw std::invalid_argument("Invalid captured text padding");
            }
        }
    }
    if (fields.extendedBundleProofHash.size() != 4)
    {
        throw std::invalid_argument("Invalid captured proof hash length");
    }
}

quint32 jsNumericStringToUint32(const QByteArray &text)
{
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number == 0.0)
    {
        return 0;
    }
    double remainder = std::fmod(std::trunc(number), 4294967296.0);
    if (remainder < 0)
    {
        remainder += 4294967296.0;
    }
    return static_cast<quint32>(remainder);
}

quint32 checksum(const std::array<Field, 17> &fields, bool numericStrings)
{
    quint32 result = 0;
    for (const auto &field : fields)
    {
        if (const auto *number = std::get_if<quint64>(&field))
        {
            result ^= static_cast<quint32>(*number);
            continue;
        }
        const auto &text = std::get<QByteArray>(field);
        if (numericStrings)
        {
            result ^= jsNumericStringToUint32(text);
        }
        else
        {
            quint32 prefix = 0;
            for (qsizetype i = 0; i < std::min<qsizetype>(4, text.size()); ++i)
            {
                prefix = (prefix << 8) | static_cast<unsigned char>(text[i]);
            }
            result ^= prefix;
        }
    }
    return result;
}

quint64 textCount(const SigningState &state)
{
    return state.canvas ? quint64{state.canvas->textRequests} +
                              state.canvas->textFailures
                        : 0;
}

quint64 imageCount(const SigningState &state)
{
    return state.canvas ? quint64{state.canvas->imageRequests} +
                              state.canvas->imageFailures
                        : 0;
}

quint32 mixedState(quint32 timestamp, quint32 seed, quint32 environmentCode)
{
    return (((timestamp >> 16) ^ (seed >> 16) ^ timestamp ^ seed) & 65535U) |
           (environmentCode << 16);
}

QByteArray gnarlyPayload(const QString &query, const QString &body,
                         const SigningState &state)
{
    const quint32 timestamp = state.timestampSeconds;
    const quint32 seed = state.gnarlySeed;
    const quint32 lowTime =
        (state.environmentFlag || (state.environmentCode & 32U)) ? 1767225600U
                                                                 : timestamp;
    const quint32 highTime =
        state.performanceMode == 2U ? state.performanceData : timestamp;
    const quint32 derived = ((lowTime ^ seed) & 65535U) |
                            (((highTime ^ (seed >> 16)) & 65535U) << 16);
    const quint64 text = textCount(state);
    const quint64 images = imageCount(state);
    std::array<Field, 17> fields{
        quint64{0},
        quint64{state.environmentCode},
        quint64{state.behaviorCode},
        md5(query),
        md5(body),
        md5(state.userAgent),
        quint64{timestamp},
        quint64{state.performanceData},
        quint64{seed},
        QByteArray("5.3.2"),
        QByteArray("1.0.0.417"),
        quint64{1},
        text,
        state.combinedCanvas ? text + images : images,
        quint64{mixedState(timestamp, seed, state.environmentCode)},
        quint64{derived},
        quint64{0}};
    fields[16] = quint64{checksum(fields, false)};
    fields[0] = quint64{checksum(fields, true)};
    std::array<quint8, 17> order{};
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = static_cast<quint8>(i);
    }
    quint32 lcg = seed;
    for (size_t i = order.size() - 1; i > 0; --i)
    {
        lcg = 1664525U * lcg + 1013904223U;
        const size_t j = static_cast<size_t>((quint64{lcg} * (i + 1)) >> 32);
        std::swap(order[i], order[j]);
    }
    QByteArray out(1, static_cast<char>(fields.size()));
    for (const quint8 key : order)
    {
        const auto &field = fields[key];
        const auto bytes = std::holds_alternative<quint64>(field)
                               ? integerBytes(std::get<quint64>(field))
                               : std::get<QByteArray>(field);
        appendField(out, key, bytes);
    }
    return out;
}

QByteArray dynosaurPayload(const QString &query, const QString &body,
                           const SigningState &state)
{
    const auto number = [](quint64 value) {
        return encodeText(QString::number(value));
    };
    const quint32 mixed =
        state.environmentFlag
            ? state.environmentCode << 16
            : mixedState(state.timestampSeconds, state.dynosaurSeed,
                         state.environmentCode);
    QString wire = QStringLiteral("0");
    if (state.extendedBundleSeed != 0)
    {
        QByteArray input("wmsdk:ex_bundle_wire:v1|");
        appendBig32(input, state.extendedBundleSeed);
        wire = QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256)
                .first(16)
                .toHex());
    }
    std::array<QByteArray, 25> fields{
        encodeText(QStringLiteral("1"), true),
        encodeText(QStringLiteral("1"), true),
        encodeText(QStringLiteral("1"), true),
        number(0),
        number(mixed),
        number(imageCount(state)),
        number(state.environmentCode),
        number(state.timestampSeconds),
        encodeText(state.extendedProofValue.isEmpty()
                       ? QStringLiteral("0")
                       : state.extendedProofValue),
        number(0),
        encodeText(QStringLiteral("5.3.2")),
        fnv(body),
        number(state.performanceData),
        number(0),
        fnv(query),
        number(textCount(state)),
        fnv(state.userAgent),
        encodeText(QStringLiteral("1.0.0.417")),
        encodeText(state.extendedScmVersion.isEmpty()
                       ? QStringLiteral("0")
                       : state.extendedScmVersion),
        encodeText(wire),
        number(state.dynosaurSeed),
        encodeText(state.location.isEmpty() ? QStringLiteral("0")
                                            : state.location),
        number(state.behaviorCode),
        number(0),
        fnv(state.extendedBundleProof)};
    if (state.capturedDynosaurFields)
    {
        const auto &captured = *state.capturedDynosaurFields;
        validateCapturedFields(captured);
        fields[8] = captured.extendedProofValue;
        fields[18] = captured.extendedScmVersion;
        fields[19] = captured.extendedWireValue;
        fields[21] = captured.location;
        fields[24] = captured.extendedBundleProofHash;
    }
    quint32 first = 0;
    for (const auto &field : fields)
    {
        first ^= static_cast<unsigned char>(field[1]);
    }
    fields[0] = encodeText(QString::number(first), true);
    QByteArray out;
    for (size_t i = 0; i < fields.size(); ++i)
    {
        appendField(out, static_cast<quint8>(32 + i), fields[i]);
    }
    return out;
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

QByteArray envelope(const QByteArray &payload, const KeyWords &keys)
{
    std::array<quint32, 16> state{};
    std::copy(Sigma.begin(), Sigma.end(), state.begin());
    std::copy(keys.begin(), keys.end(), state.begin() + 4);
    quint32 lowNibbles = 0;
    for (const quint32 key : keys)
    {
        lowNibbles += key & 15U;
    }
    const quint32 rounds = 5U + (lowNibbles & 15U);
    QByteArray cipher(payload);
    for (qsizetype offset = 0; offset < cipher.size(); offset += 64)
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
        for (qsizetype i = 0; i < 64 && offset + i < cipher.size(); ++i)
        {
            const auto byte = static_cast<unsigned char>(cipher[offset + i]);
            cipher[offset + i] = static_cast<char>(
                byte ^
                ((work[static_cast<size_t>(i >> 2)] >> (8 * (i & 3))) & 255U));
        }
        ++state[12];
    }
    QByteArray keyBytes;
    for (const quint32 key : keys)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            keyBytes.append(static_cast<char>((key >> shift) & 255U));
        }
    }
    quint64 sum = 0;
    for (const unsigned char byte : keyBytes)
    {
        sum += byte;
    }
    for (const unsigned char byte : cipher)
    {
        sum += byte;
    }
    const qsizetype insert =
        static_cast<qsizetype>(sum % static_cast<quint64>(cipher.size() + 1));
    QByteArray out(1, char{75});
    out.append(cipher.first(insert));
    out.append(keyBytes);
    out.append(cipher.sliced(insert));
    return out;
}

QString encodeEnvelope(const QByteArray &payload, const KeyWords &keys)
{
    QByteArray result = envelope(payload, keys).toBase64();
    for (char &letter : result)
    {
        if (letter != '=')
        {
            const auto *found =
                std::find(std::begin(StandardAlphabet),
                          std::end(StandardAlphabet) - 1, letter);
            letter = Alphabet[found - StandardAlphabet];
        }
    }
    return QString::fromLatin1(result);
}

bool jsWhitespace(ushort code)
{
    return (code >= 9 && code <= 13) || code == 0x20 || code == 0xa0 ||
           code == 0x1680 || (code >= 0x2000 && code <= 0x200a) ||
           code == 0x2028 || code == 0x2029 || code == 0x202f ||
           code == 0x205f || code == 0x3000 || code == 0xfeff;
}

int hexDigit(char code)
{
    if (code >= '0' && code <= '9')
    {
        return code - '0';
    }
    if (code >= 'a' && code <= 'f')
    {
        return code - 'a' + 10;
    }
    if (code >= 'A' && code <= 'F')
    {
        return code - 'A' + 10;
    }
    return -1;
}

QString decodeKey(const QString &key)
{
    const QByteArray encoded = key.toUtf8();
    QByteArray decoded;
    for (qsizetype i = 0; i < encoded.size(); ++i)
    {
        if (encoded[i] == '%')
        {
            if (i + 2 >= encoded.size() || hexDigit(encoded[i + 1]) < 0 ||
                hexDigit(encoded[i + 2]) < 0)
            {
                throw std::invalid_argument("Malformed query key encoding");
            }
            decoded.append(static_cast<char>((hexDigit(encoded[i + 1]) << 4) |
                                             hexDigit(encoded[i + 2])));
            i += 2;
        }
        else
        {
            decoded.append(encoded[i]);
        }
    }
    QStringDecoder decoder(QStringDecoder::Utf8,
                           QStringConverter::Flag::Stateless);
    const QString result = decoder(decoded);
    if (decoder.hasError())
    {
        throw std::invalid_argument("Malformed UTF8 query key encoding");
    }
    return result;
}

QString signatureQuery(const QString &query)
{
    QStringList parts;
    for (const auto &part : query.split(u'&'))
    {
        if (!std::all_of(part.begin(), part.end(), [](QChar c) {
                return jsWhitespace(c.unicode());
            }))
        {
            parts.push_back(part);
        }
    }
    for (qsizetype i = parts.size(); i > 0; --i)
    {
        const auto &part = parts[i - 1];
        const qsizetype split = part.indexOf(u'=');
        const QString key = split < 0 ? part : part.first(split);
        if (decodeKey(key) == QStringLiteral("X-Tts-Oec-Bsid"))
        {
            parts.removeAt(i - 1);
            return parts.join(u'&');
        }
    }
    return query;
}

KeyWords randomKeys()
{
    KeyWords keys{};
    QRandomGenerator::system()->fillRange(keys.data(), keys.size());
    return keys;
}
}

SigningResult signExactQuery(
    const QString &query, const QString &body, const SigningState &state,
    const std::optional<DeterministicKeys> &fixtureKeys)
{
    for (const QString *text :
         {&query, &body, &state.userAgent, &state.msToken, &state.location,
          &state.extendedBundleProof, &state.extendedScmVersion,
          &state.extendedProofValue})
    {
        requireWellFormed(*text);
    }
    if (state.userAgent.isEmpty())
    {
        throw std::invalid_argument("Explicit matching User-Agent is required");
    }
    if (query.startsWith(u'?'))
    {
        throw std::invalid_argument(
            "Expected query without leading question mark");
    }
    const quint64 text = textCount(state);
    const quint64 images = imageCount(state);
    if (text > std::numeric_limits<quint32>::max() ||
        images > std::numeric_limits<quint32>::max() ||
        (state.combinedCanvas &&
         text + images > std::numeric_limits<quint32>::max()))
    {
        throw std::invalid_argument(
            "Canvas totals exceed supported SDK field domain");
    }
    const DeterministicKeys keys =
        fixtureKeys ? *fixtureKeys
                    : DeterministicKeys{randomKeys(), randomKeys()};
    SigningResult result;
    result.dynosaurPayload =
        dynosaurPayload(signatureQuery(query), body, state);
    result.dynosaur = encodeEnvelope(result.dynosaurPayload, keys.dynosaur);
    const QString joined = query + QStringLiteral("&X-Dynosaur=") +
                           result.dynosaur + QStringLiteral("&msToken=") +
                           state.msToken;
    result.gnarlyPayload = gnarlyPayload(signatureQuery(joined), body, state);
    result.gnarly = encodeEnvelope(result.gnarlyPayload, keys.gnarly);
    result.signedQuery =
        joined + QStringLiteral("&X-Bogus=1&X-Gnarly=") + result.gnarly;
    return result;
}
}
