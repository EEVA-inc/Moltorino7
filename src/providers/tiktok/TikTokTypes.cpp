#include "providers/tiktok/TikTokTypes.hpp"

#include <QRegularExpression>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

std::optional<QString> normalizeTikTokHandle(QStringView source)
{
    auto value = source.trimmed().toString();
    if (value.startsWith(u"tiktok:", Qt::CaseInsensitive))
    {
        value.remove(0, 7);
    }
    if (value.startsWith(u"https://", Qt::CaseInsensitive))
    {
        const QUrl url(value, QUrl::StrictMode);
        const auto host = url.host().toLower();
        if (!url.isValid() ||
            (host != u"www.tiktok.com" && host != u"tiktok.com") ||
            !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() ||
            url.hasFragment())
        {
            return std::nullopt;
        }
        value = url.path();
        if (value.endsWith(u'/'))
        {
            value.chop(1);
        }
        if (value.endsWith(u"/live"))
        {
            value.chop(5);
        }
        if (!value.startsWith(u"/@"))
        {
            return std::nullopt;
        }
        value.remove(0, 1);
    }
    if (value.startsWith(u'@'))
    {
        value.remove(0, 1);
    }
    static const QRegularExpression handle(u"^[A-Za-z0-9_.]{1,24}$"_s);
    if (!handle.match(value).hasMatch() || value.endsWith(u'.'))
    {
        return std::nullopt;
    }
    return value.toLower();
}

QUrl tikTokProfileUrl(QStringView handle)
{
    const auto normalized = normalizeTikTokHandle(handle);
    return normalized ? QUrl(u"https://www.tiktok.com/@"_s + *normalized)
                      : QUrl{};
}

bool isTikTokImageUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != u"https" ||
        !url.userInfo().isEmpty() || url.port() != -1 ||
        url.toEncoded().size() > 4096)
    {
        return false;
    }
    const auto host = url.host().toLower();
    for (auto suffix :
         {u"tiktokcdn.com", u"tiktokcdn-us.com", u"tiktokcdn-eu.com",
          u"byteoversea.com", u"ibytedtos.com", u"muscdn.com", u"byteimg.com"})
    {
        if (host == suffix || host.endsWith(u'.' + QString::fromUtf16(suffix)))
        {
            return true;
        }
    }
    return false;
}

}
