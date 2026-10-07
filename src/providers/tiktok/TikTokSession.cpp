#include "providers/tiktok/TikTokSession.hpp"

#include <boost/beast/zlib/inflate_stream.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookieJar>
#include <QtEndian>

#include <algorithm>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;
namespace {
constexpr qsizetype MAX_SESSION_BYTES = 64 * 1024;
constexpr qsizetype MAX_ENCODED_BYTES = 32 * 1024;
constexpr qsizetype MAX_COOKIES = 128;

class SessionJar : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::allCookies;
    using QNetworkCookieJar::setAllCookies;
};

bool validCookie(const QNetworkCookie &cookie)
{
    auto domain = cookie.domain();
    if (domain.startsWith(u'.'))
    {
        domain.remove(0, 1);
    }
    if (domain != u"tiktok.com" && !domain.endsWith(u".tiktok.com"))
    {
        return false;
    }
    if (cookie.name().isEmpty() || cookie.name().size() > 128 ||
        cookie.value().size() > 8192 || cookie.path().size() > 1024 ||
        !cookie.path().startsWith(u'/'))
    {
        return false;
    }
    for (char c : cookie.name())
    {
        if (static_cast<unsigned char>(c) <= 32 || c == ';' || c == '=' ||
            static_cast<unsigned char>(c) >= 127)
        {
            return false;
        }
    }
    for (char c : cookie.value())
    {
        if (static_cast<unsigned char>(c) < 32 || c == ';' || c == 127)
        {
            return false;
        }
    }
    return true;
}

bool usableCookie(const QNetworkCookie &cookie)
{
    return validCookie(cookie) &&
           (cookie.isSessionCookie() ||
            cookie.expirationDate() > QDateTime::currentDateTimeUtc());
}

bool validMsToken(QStringView token)
{
    return token.size() <= 8192 &&
           std::all_of(token.begin(), token.end(), [](QChar c) {
               return c.unicode() >= 33 && c.unicode() <= 126;
           });
}
}

bool isTikTokAccountUrl(const QUrl &url)
{
    static const QStringList hosts{
        u"www.tiktok.com"_s,           u"webcast.tiktok.com"_s,
        u"webcast.us.tiktok.com"_s,    u"web-sg.tiktok.com"_s,
        u"web-va.tiktok.com"_s,        u"us.tiktok.com"_s,
        u"login-no1a.www.tiktok.com"_s};
    return url.isValid() && url.scheme() == u"https" &&
           url.userInfo().isEmpty() && url.port(443) == 443 &&
           hosts.contains(url.host());
}

QByteArray TikTokSession::cookieHeader(const QUrl &url) const
{
    if (!isTikTokAccountUrl(url))
    {
        return {};
    }
    SessionJar jar;
    jar.setAllCookies(this->cookies);
    QByteArray result;
    for (const auto &cookie : jar.cookiesForUrl(url))
    {
        if (!usableCookie(cookie))
        {
            continue;
        }
        if (!result.isEmpty())
        {
            result.append("; ");
        }
        result.append(cookie.toRawForm(QNetworkCookie::NameAndValueOnly));
    }
    return result;
}

QByteArray TikTokSession::cookieValue(const QByteArray &name,
                                      const QUrl &url) const
{
    if (!isTikTokAccountUrl(url))
    {
        return {};
    }
    SessionJar jar;
    jar.setAllCookies(this->cookies);
    for (const auto &cookie : jar.cookiesForUrl(url))
    {
        if (cookie.name() == name && usableCookie(cookie))
        {
            return cookie.value();
        }
    }
    return {};
}

bool TikTokSession::hasCredentials() const
{
    return !this->cookieValue("sessionid", QUrl(u"https://www.tiktok.com/"_s))
                .isEmpty();
}

void TikTokSession::updateCookies(const QList<QNetworkCookie> &values,
                                  const QUrl &url)
{
    if (!isTikTokAccountUrl(url))
    {
        return;
    }
    SessionJar jar;
    jar.setAllCookies(this->cookies);
    for (auto cookie : values)
    {
        if (cookie.domain().isEmpty())
        {
            cookie.setDomain(url.host());
        }
        if (cookie.path().isEmpty())
        {
            const auto path = url.path();
            const auto slash = path.lastIndexOf(u'/');
            cookie.setPath(slash > 0 ? path.left(slash) : u"/"_s);
        }
        if (validCookie(cookie))
        {
            jar.setCookiesFromUrl({cookie}, url);
        }
    }
    auto updated = jar.allCookies();
    if (updated.size() <= MAX_COOKIES)
    {
        this->cookies = std::move(updated);
    }
}

ExpectedStr<QString> encodeTikTokSession(const TikTokSession &session)
{
    if (session.userAgent.isEmpty() || session.userAgent.size() > 1024 ||
        session.userAgent.contains(u'\r') ||
        session.userAgent.contains(u'\n') ||
        session.cookies.size() > MAX_COOKIES || !session.hasCredentials() ||
        !validMsToken(session.msToken) ||
        (session.msTokenStatus != 0 && session.msTokenStatus != 5 &&
         session.msTokenStatus != 9))
    {
        return makeUnexpected(u"TikTok did not provide a usable session."_s);
    }
    QJsonArray cookies;
    for (const auto &cookie : session.cookies)
    {
        if (!validCookie(cookie))
        {
            return makeUnexpected(
                u"TikTok returned an invalid session cookie."_s);
        }
        cookies.append(QString::fromLatin1(cookie.toRawForm().toBase64()));
    }
    auto data =
        QJsonDocument(
            QJsonObject{{u"version"_s, 1},
                        {u"userAgent"_s, session.userAgent},
                        {u"cookies"_s, cookies},
                        {u"context"_s, session.context},
                        {u"msToken"_s, session.msToken},
                        {u"msTokenStatus"_s, session.msTokenStatus},
                        {u"ticketGuardStorage"_s, session.ticketGuardStorage}})
            .toJson(QJsonDocument::Compact);
    if (data.size() > MAX_SESSION_BYTES)
    {
        return makeUnexpected(u"The TikTok session is too large to save."_s);
    }
    auto encoded = qCompress(data, 9).toBase64();
    if (encoded.size() > MAX_ENCODED_BYTES)
    {
        return makeUnexpected(u"The TikTok session is too large to save."_s);
    }
    return QString::fromLatin1(encoded);
}

ExpectedStr<TikTokSession> decodeTikTokSession(const QString &encoded)
{
    auto invalid = [] {
        return makeUnexpected(
            u"The saved TikTok session could not be read. Log in again."_s);
    };
    if (encoded.size() > MAX_ENCODED_BYTES)
    {
        return invalid();
    }
    auto compressed = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (compressed.size() < 11 ||
        qFromBigEndian<quint32>(compressed.constData()) > MAX_SESSION_BYTES)
    {
        return invalid();
    }

    const auto cmf = static_cast<unsigned char>(compressed[4]);
    const auto flags = static_cast<unsigned char>(compressed[5]);
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || (flags & 32) != 0 ||
        ((cmf << 8) | flags) % 31 != 0)
    {
        return invalid();
    }
    QByteArray decoded(MAX_SESSION_BYTES, Qt::Uninitialized);
    boost::beast::zlib::inflate_stream stream;
    boost::beast::zlib::z_params params{};
    params.next_in = compressed.constData() + 6;

    params.avail_in = static_cast<std::size_t>(compressed.size() - 6);
    params.next_out = decoded.data();
    params.avail_out = MAX_SESSION_BYTES;
    boost::system::error_code error;
    stream.write(params, boost::beast::zlib::Flush::finish, error);
    if (error != boost::beast::zlib::error::end_of_stream ||
        params.avail_in + (params.data_type & 63) / 8 != 4 ||
        params.total_out != qFromBigEndian<quint32>(compressed.constData()))
    {
        return invalid();
    }
    decoded.resize(static_cast<qsizetype>(params.total_out));
    quint32 a = 1;
    quint32 b = 0;
    for (const unsigned char byte : decoded)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    if (((b << 16) | a) !=
        qFromBigEndian<quint32>(compressed.constData() + compressed.size() - 4))
    {
        return invalid();
    }
    const auto json = QJsonDocument::fromJson(decoded).object();
    if (json.value("version").toInt() != 1 || !json.value("cookies").isArray())
    {
        return invalid();
    }
    TikTokSession session;
    session.userAgent = json.value("userAgent").toString();
    session.context = json.value("context").toObject();
    session.msToken = json.value("msToken").toString();
    session.msTokenStatus = json.value("msTokenStatus").toInt();
    session.ticketGuardStorage = json.value("ticketGuardStorage").toObject();
    const auto cookies = json.value("cookies").toArray();
    if (cookies.size() > MAX_COOKIES)
    {
        return invalid();
    }
    for (const auto &value : cookies)
    {
        const auto raw =
            QByteArray::fromBase64(value.toString().toLatin1(),
                                   QByteArray::AbortOnBase64DecodingErrors);
        const auto parsed = QNetworkCookie::parseCookies(raw);
        if (parsed.size() != 1 || !validCookie(parsed.front()))
        {
            return invalid();
        }
        session.cookies.push_back(parsed.front());
    }
    if (!encodeTikTokSession(session))
    {
        return invalid();
    }
    return session;
}

bool TikTokSession::updateMsToken(const QByteArray &token, int requestStatus)
{
    const auto value = QString::fromLatin1(token);
    if (value.isEmpty() || !validMsToken(value) ||
        requestStatus < this->msTokenStatus)
    {
        return false;
    }
    if (this->msToken == value)
    {
        return false;
    }
    this->msToken = value;
    return true;
}
}
