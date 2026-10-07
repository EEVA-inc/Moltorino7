#include "providers/tiktok/TikTokTicketGuard.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chatterino::tiktok::ticketguard {
namespace {
template <typename T, auto Free>
using Owned = std::unique_ptr<T, decltype(Free)>;
using Key = Owned<EVP_PKEY, EVP_PKEY_free>;

[[noreturn]] void invalid(const char *message)
{
    throw std::invalid_argument(message);
}

void require(bool condition, const char *message)
{
    if (!condition)
    {
        invalid(message);
    }
}

void cryptoRequire(bool condition)
{
    if (!condition)
    {
        throw std::runtime_error("Ticket Guard cryptographic operation failed");
    }
}

bool validUtf16(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i)
    {
        if (text[i].isHighSurrogate())
        {
            if (++i == text.size() || !text[i].isLowSurrogate())
            {
                return false;
            }
        }
        else if (text[i].isLowSurrogate())
        {
            return false;
        }
    }
    return true;
}

QString boundedText(const QJsonValue &value, qsizetype maxBytes,
                    bool required = false)
{
    if (value.isUndefined() && !required)
    {
        return {};
    }
    require(value.isString(), "Expected a string in Ticket Guard state");
    const auto text = value.toString();
    require(validUtf16(text) && text.toUtf8().size() <= maxBytes,
            "Invalid or oversized Ticket Guard text");
    require(!required || !text.isEmpty(),
            "Required Ticket Guard value is empty");
    return text;
}

QJsonObject parseObject(const QByteArray &bytes, qsizetype maxBytes)
{
    require(!bytes.isEmpty() && bytes.size() <= maxBytes,
            "Invalid Ticket Guard JSON size");
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(bytes);
    require(!decoder.hasError() && validUtf16(decoded),
            "Invalid UTF-8 in Ticket Guard JSON");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "Expected a Ticket Guard JSON object");
    return document.object();
}

QByteArray decodeBase64(const QByteArray &encoded, qsizetype maxBytes)
{
    require(encoded.size() <= maxBytes, "Oversized Ticket Guard Base64 value");
    const auto result = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    require(bool(result) && result.decoded.toBase64() == encoded,
            "Invalid Ticket Guard Base64 encoding");
    return result.decoded;
}

QString decryptTicket(const QString &encoded)
{
    require(validUtf16(encoded), "Invalid encrypted ticket text");
    const auto bytes = decodeBase64(encoded.toUtf8(), 8192);
    require(
        bytes.size() >= 28 && bytes.size() - 28 <= TicketGuard::MaxTicketBytes,
        "Invalid encrypted ticket size");
    std::array<unsigned char, 16> key{};
    static constexpr char Password[] = "tt-ticket-guard-iv";
    static constexpr unsigned char Salt[] = "secure-salt";
    cryptoRequire(PKCS5_PBKDF2_HMAC(Password, int(sizeof(Password) - 1), Salt,
                                    int(sizeof(Salt) - 1), 1000, EVP_sha256(),
                                    int(key.size()), key.data()) == 1);
    Owned<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context(EVP_CIPHER_CTX_new(),
                                                       EVP_CIPHER_CTX_free);
    cryptoRequire(bool(context));
    const auto *input =
        reinterpret_cast<const unsigned char *>(bytes.constData());
    const int ciphertextSize = int(bytes.size() - 28);
    std::array<unsigned char, 16> tag{};
    std::copy_n(input + bytes.size() - 16, tag.size(), tag.begin());
    QByteArray plaintext(ciphertextSize + 16, '\0');
    auto *output = reinterpret_cast<unsigned char *>(plaintext.data());
    int written = 0;
    int finalWritten = 0;
    cryptoRequire(EVP_DecryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr,
                                     nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12,
                                      nullptr) == 1 &&
                  EVP_DecryptInit_ex(context.get(), nullptr, nullptr,
                                     key.data(), input) == 1 &&
                  EVP_DecryptUpdate(context.get(), output, &written, input + 12,
                                    ciphertextSize) == 1 &&
                  EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16,
                                      tag.data()) == 1);
    const bool authenticated =
        EVP_DecryptFinal_ex(context.get(), output + written, &finalWritten) ==
        1;
    OPENSSL_cleanse(key.data(), key.size());
    if (!authenticated)
    {
        OPENSSL_cleanse(plaintext.data(), size_t(plaintext.size()));
        invalid("Encrypted ticket authentication failed");
    }
    plaintext.resize(written + finalWritten);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(plaintext);
    OPENSSL_cleanse(plaintext.data(), size_t(plaintext.size()));
    require(!decoder.hasError() && validUtf16(decoded), "Invalid ticket UTF-8");
    return decoded;
}

Key readKey(const QByteArray &pem, bool privateKey)
{
    require(!pem.isEmpty() && pem.size() <= TicketGuard::MaxPemBytes,
            "Invalid Ticket Guard PEM size");
    const auto trimmed = pem.trimmed();
    const QByteArray begin = privateKey ? "-----BEGIN PRIVATE KEY-----"
                                        : "-----BEGIN PUBLIC KEY-----";
    const QByteArray end =
        privateKey ? "-----END PRIVATE KEY-----" : "-----END PUBLIC KEY-----";
    require(trimmed.startsWith(begin) && trimmed.endsWith(end) &&
                trimmed.count("-----BEGIN ") == 1 &&
                trimmed.count("-----END ") == 1,
            "Expected a single PKCS8 private key or SPKI public key");
    Owned<BIO, BIO_free> bio(
        BIO_new_mem_buf(trimmed.constData(), int(trimmed.size())), BIO_free);
    cryptoRequire(bool(bio));
    Key key(nullptr, EVP_PKEY_free);
    if (privateKey)
    {
        Owned<PKCS8_PRIV_KEY_INFO, PKCS8_PRIV_KEY_INFO_free> info(
            PEM_read_bio_PKCS8_PRIV_KEY_INFO(bio.get(), nullptr, nullptr,
                                             nullptr),
            PKCS8_PRIV_KEY_INFO_free);
        require(bool(info), "Invalid PKCS8 private key");
        key.reset(EVP_PKCS82PKEY(info.get()));
    }
    else
    {
        key.reset(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
    }
    require(bool(key) && EVP_PKEY_is_a(key.get(), "EC") == 1,
            "Expected an EC key");
    std::array<char, 64> group{};
    size_t groupLength = 0;
    require(EVP_PKEY_get_utf8_string_param(
                key.get(), OSSL_PKEY_PARAM_GROUP_NAME, group.data(),
                group.size(), &groupLength) == 1,
            "Missing EC group");
    const QByteArray curve(group.data(), qsizetype(groupLength));
    require(curve == "prime256v1" || curve == "P-256" || curve == "secp256r1",
            "Expected the P-256 curve");
    Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> validation(
        EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    cryptoRequire(bool(validation));
    require(
        EVP_PKEY_public_check(validation.get()) == 1 &&
            (!privateKey || (EVP_PKEY_private_check(validation.get()) == 1 &&
                             EVP_PKEY_pairwise_check(validation.get()) == 1)),
        "Invalid EC key material");
    return key;
}

QByteArray pointBytes(EVP_PKEY *key)
{
    BIGNUM *rawX = nullptr;
    BIGNUM *rawY = nullptr;
    const bool okX =
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_X, &rawX) == 1;
    const bool okY =
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_Y, &rawY) == 1;
    Owned<BIGNUM, BN_free> x(rawX, BN_free);
    Owned<BIGNUM, BN_free> y(rawY, BN_free);
    cryptoRequire(okX && okY && x && y);
    QByteArray result(65, '\0');
    result[0] = 4;
    auto *out = reinterpret_cast<unsigned char *>(result.data());
    cryptoRequire(BN_bn2binpad(x.get(), out + 1, 32) == 32 &&
                  BN_bn2binpad(y.get(), out + 33, 32) == 32);
    return result;
}

bool hasProvider(EndpointRole role)
{
    return role == EndpointRole::Provider ||
           role == EndpointRole::ProviderAndConsumer;
}

bool hasConsumer(EndpointRole role)
{
    return role == EndpointRole::Consumer ||
           role == EndpointRole::ProviderAndConsumer;
}

QByteArray exactPath(const QByteArray &url)
{
    require(!url.isEmpty() && url.size() <= TicketGuard::MaxUrlBytes,
            "Invalid Ticket Guard URL size");
    for (char value : url)
    {
        const auto c = static_cast<unsigned char>(value);
        require(c >= 0x21 && c <= 0x7e && c != '\\',
                "Expected an already serialized ASCII URL");
    }
    const auto parsed = QUrl::fromEncoded(url, QUrl::StrictMode);
    require(parsed.isValid() && parsed.scheme() == u"https" &&
                !parsed.host().isEmpty() && parsed.userInfo().isEmpty() &&
                url.startsWith("https://"),
            "Expected an absolute HTTPS URL");
    const auto authorityEnd = url.indexOf('/', 8);
    const auto query = url.indexOf('?');
    const auto fragment = url.indexOf('#');
    qsizetype stop = url.size();
    if (query >= 0)
    {
        stop = std::min(stop, query);
    }
    if (fragment >= 0)
    {
        stop = std::min(stop, fragment);
    }
    const auto path = authorityEnd < 0 || authorityEnd >= stop
                          ? QByteArray("/")
                          : url.mid(authorityEnd, stop - authorityEnd);
    for (const auto &segment : path.split('/'))
    {
        const auto decoded = QByteArray::fromPercentEncoding(segment);
        require(decoded != "." && decoded != "..",
                "URL dot segments are not supported");
    }
    return path;
}

QByteArray jsonValue(const QJsonValue &value)
{
    const auto bytes =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2);
}

struct SignData {
    QString ticket;
    QJsonValue tsSign = QJsonValue(QJsonValue::Undefined);
};

SignData parseSignData(const QJsonObject &object)
{
    SignData result;
    result.ticket =
        boundedText(object.value(u"ticket"_s), TicketGuard::MaxTicketBytes);
    const auto encrypted = boundedText(object.value(u"encrypt_ticket"_s), 8192);
    if (result.ticket.isEmpty() && !encrypted.isEmpty())
    {
        result.ticket = decryptTicket(encrypted);
    }
    result.tsSign = object.value(u"ts_sign"_s);
    if (!result.tsSign.isUndefined() && !result.tsSign.isNull())
    {
        boundedText(result.tsSign, TicketGuard::MaxTsSignBytes);
    }
    boundedText(object.value(u"client_cert"_s), TicketGuard::MaxPemBytes);
    return result;
}
}

struct TicketGuard::KeyMaterial {
    Key privateKey{nullptr, EVP_PKEY_free};
    QByteArray publicPoint;
};

TicketGuard TicketGuard::importStorage(const QByteArray &exportedJson,
                                       const QString &scene,
                                       const QString &storageNamespace)
{
    for (const auto &component : {scene, storageNamespace})
    {
        require(component.size() <= 64,
                "Oversized Ticket Guard storage selector");
        for (const auto c : component)
        {
            require((c.isLetterOrNumber() && c.unicode() < 128) || c == u'_' ||
                        c == u'-' || c == u'.',
                    "Invalid Ticket Guard storage selector");
        }
    }
    require(!scene.isEmpty(), "Ticket Guard scene is empty");
    const auto outer = parseObject(exportedJson, MaxExportBytes);
    auto prefix = u"security-sdk/"_s;
    if (!storageNamespace.isEmpty())
    {
        prefix += storageNamespace + u'/';
    }
    const auto unwrap = [&](const QString &name, bool required = false) {
        const auto value = outer.value(prefix + name);
        if (value.isUndefined() && !required)
        {
            return QByteArray{};
        }
        const auto text = boundedText(value, MaxExportBytes, true);
        const auto wrapped = parseObject(text.toUtf8(), MaxExportBytes);
        return boundedText(wrapped.value(u"data"_s), MaxExportBytes, required)
            .toUtf8();
    };
    const auto keyObject =
        parseObject(unwrap(u"s_sdk_crypt_sdk"_s, true), MaxExportBytes);
    auto privateKey = readKey(
        boundedText(keyObject.value(u"ec_privateKey"_s), MaxPemBytes, true)
            .toUtf8(),
        true);
    auto publicKey = readKey(
        boundedText(keyObject.value(u"ec_publicKey"_s), MaxPemBytes, true)
            .toUtf8(),
        false);
    require(EVP_PKEY_eq(privateKey.get(), publicKey.get()) == 1,
            "Ticket Guard key pair does not match");
    TicketGuard result;
    result.hasStorageNamespace_ = !storageNamespace.isEmpty();
    result.keys_ = std::make_shared<KeyMaterial>();
    result.keys_->publicPoint = pointBytes(publicKey.get());
    result.keys_->privateKey = std::move(privateKey);
    result.csr_ = boundedText(keyObject.value(u"ec_csr"_s), MaxPemBytes);
    const auto certificate = unwrap(u"s_sdk_cert_key"_s);
    require(certificate.size() <= MaxPemBytes,
            "Oversized Ticket Guard certificate");
    result.certificate_ = QString::fromUtf8(certificate);
    const auto signBytes = unwrap(u"s_sdk_sign_data_key/"_s + scene);
    if (!signBytes.isEmpty())
    {
        const auto sign = parseSignData(parseObject(signBytes, MaxExportBytes));
        result.ticket_ = sign.ticket;
        result.tsSign_ = sign.tsSign;
    }
    return result;
}

QByteArray TicketGuard::publicPointBase64() const
{
    require(bool(keys_), "Ticket Guard state has not been initialized");
    return keys_->publicPoint.toBase64();
}

Headers TicketGuard::headersForRequest(const QByteArray &absoluteUrl,
                                       qint64 timestampSeconds,
                                       EndpointRole role) const
{
    require(keys_ && timestampSeconds >= 0 &&
                timestampSeconds <= 9007199254740991LL,
            "Invalid Ticket Guard state or timestamp");
    const auto path = exactPath(absoluteUrl);
    const auto tsText = tsSign_.toString();
    const bool publicMode = hasProvider(role) || tsText.isEmpty() ||
                            tsText.startsWith(u"ts.1") ||
                            certificate_.contains(u"pub.");
    Headers result;
    if (publicMode)
    {
        result.insert("tt-ticket-guard-public-key", publicPointBase64());
        result.insert("tt-ticket-guard-web-version", "1");
    }
    else if (!certificate_.isEmpty())
    {
        result.insert("tt-ticket-guard-client-cert",
                      certificate_.toUtf8().toBase64());
    }
    else
    {
        result.insert("tt-ticket-guard-client-csr", csr_.toUtf8().toBase64());
    }
    result.insert("tt-ticket-guard-version", "2");
    result.insert("tt-ticket-guard-iteration-version", "0");
    if (ticket_.isEmpty() || !hasConsumer(role))
    {
        return result;
    }
    const auto input = QByteArray("ticket=") + ticket_.toUtf8() +
                       "&path=" + path +
                       "&timestamp=" + QByteArray::number(timestampSeconds);
    Owned<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(),
                                               EVP_MD_CTX_free);
    cryptoRequire(bool(context));
    cryptoRequire(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(),
                                     nullptr, keys_->privateKey.get()) == 1);
    size_t signatureSize = 0;
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(input.constData());
    cryptoRequire(EVP_DigestSign(context.get(), nullptr, &signatureSize, bytes,
                                 size_t(input.size())) == 1 &&
                  signatureSize <= 80);
    QByteArray signature(qsizetype(signatureSize), '\0');
    cryptoRequire(
        EVP_DigestSign(context.get(),
                       reinterpret_cast<unsigned char *>(signature.data()),
                       &signatureSize, bytes, size_t(input.size())) == 1);
    signature.resize(qsizetype(signatureSize));
    QByteArray packed("{");
    if (!tsSign_.isUndefined())
    {
        packed += "\"ts_sign\":" + jsonValue(tsSign_) + ',';
    }
    packed += "\"req_content\":\"ticket,path,timestamp\",\"req_sign\":" +
              jsonValue(QString::fromLatin1(signature.toBase64())) +
              ",\"timestamp\":" + QByteArray::number(timestampSeconds) + '}';
    result.insert("tt-ticket-guard-client-data", packed.toBase64());
    return result;
}

bool TicketGuard::applyServerData(const QByteArray &base64Header,
                                  EndpointRole role)
{
    require(bool(keys_), "Ticket Guard state has not been initialized");
    if (!hasProvider(role) || base64Header.isEmpty())
    {
        return false;
    }
    const auto response = parseObject(
        decodeBase64(base64Header, MaxServerHeaderBytes), MaxExportBytes);
    const auto values = response.value(u"tickets"_s);
    require(values.isArray(), "Expected a Ticket Guard ticket array");
    const auto tickets = values.toArray();
    if (tickets.isEmpty())
    {
        return false;
    }
    require(tickets.first().isObject(), "Invalid Ticket Guard ticket entry");
    const auto entry = tickets.first().toObject();
    const auto plainTicket =
        boundedText(entry.value(u"ticket"_s), MaxTicketBytes);
    if (plainTicket.isEmpty())
    {
        return false;
    }
    const auto next = parseSignData(entry);
    const auto nextCertificate =
        boundedText(entry.value(u"client_cert"_s), MaxPemBytes);
    ticket_ = next.ticket;
    tsSign_ = next.tsSign;
    if (hasStorageNamespace_ && !nextCertificate.isEmpty())
    {
        certificate_ = nextCertificate;
    }
    return true;
}
}
