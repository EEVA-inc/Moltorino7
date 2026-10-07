#include "providers/links/TikTokLinkResolver.hpp"

#include <QJsonValue>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QStringList>
#include <QUrlQuery>

namespace chatterino::tiktok {
namespace {

constexpr qsizetype MAX_AUTHOR_LENGTH = 100;
constexpr qsizetype MAX_TITLE_LENGTH = 300;
constexpr qsizetype MAX_THUMBNAIL_URL_LENGTH = 4096;

bool isCanonicalHost(const QString &host)
{
    return host == QStringLiteral("tiktok.com") ||
           host == QStringLiteral("www.tiktok.com") ||
           host == QStringLiteral("m.tiktok.com");
}

bool isShortHost(const QString &host)
{
    return host == QStringLiteral("vm.tiktok.com") ||
           host == QStringLiteral("vt.tiktok.com");
}

QString cleanText(QString text, qsizetype maximumLength)
{
    static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
    text.replace(whitespace, QStringLiteral(" "));
    text = text.trimmed();
    if (text.size() > maximumLength)
    {
        text = text.left(maximumLength - 1) + QChar(0x2026);
    }
    return text;
}

QUrl canonicalUrl(const QString &path)
{
    QUrl url(QStringLiteral("https://www.tiktok.com"));
    url.setPath(path);
    return url;
}

}

std::optional<LinkTarget> parseLink(const QUrl &input)
{
    static const QRegularExpression postPath(
        QStringLiteral(
            R"(^/@([A-Za-z0-9._]{0,64})/(video|photo)/(\d{1,32})/?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression profilePath(
        QStringLiteral(R"(^/@([A-Za-z0-9._]{1,64})/?$)"));
    static const QRegularExpression shortPath(
        QStringLiteral(R"(^/t/[^/]{1,128}/?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mobileShortPath(
        QStringLiteral(R"(^/[^/]{1,128}/?$)"));

    if (!input.isValid())
    {
        return std::nullopt;
    }

    const auto scheme = input.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
    {
        return std::nullopt;
    }

    const auto host = input.host().toLower();
    const auto path = input.path();

    if ((isShortHost(host) && mobileShortPath.match(path).hasMatch()) ||
        (isCanonicalHost(host) && shortPath.match(path).hasMatch()))
    {
        QUrl clean = input;
        clean.setScheme(QStringLiteral("https"));
        clean.setUserInfo(QString{});
        clean.setPort(-1);
        clean.setQuery(QString{});
        clean.setFragment(QString{});
        return LinkTarget{
            .kind = LinkKind::Short,
            .url = clean,
            .cacheKey = QStringLiteral("short:%1%2").arg(host, path),
        };
    }

    if (!isCanonicalHost(host))
    {
        return std::nullopt;
    }

    const auto post = postPath.match(path);
    if (post.hasMatch())
    {
        const auto postType = post.captured(2).toLower();
        const auto postID = post.captured(3);
        const auto cleanPath = QStringLiteral("/@%1/%2/%3")
                                   .arg(post.captured(1), postType, postID);
        return LinkTarget{
            .kind = LinkKind::Post,
            .url = canonicalUrl(cleanPath),
            .cacheKey = QStringLiteral("post:%1").arg(postID),
        };
    }

    const auto profile = profilePath.match(path);
    if (profile.hasMatch())
    {
        const auto username = profile.captured(1);
        return LinkTarget{
            .kind = LinkKind::Profile,
            .url = canonicalUrl(QStringLiteral("/@%1").arg(username)),
            .cacheKey = QStringLiteral("profile:%1").arg(username.toLower()),
        };
    }

    return std::nullopt;
}

QUrl makeOEmbedUrl(const QUrl &tiktokUrl)
{
    QUrl endpoint(QStringLiteral("https://www.tiktok.com/oembed"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("url"), tiktokUrl.toString());
    endpoint.setQuery(query);
    return endpoint;
}

std::optional<Preview> parseOEmbed(const QJsonObject &root)
{
    if (root.value(QStringLiteral("provider_name"))
            .toString()
            .compare(QStringLiteral("TikTok"), Qt::CaseInsensitive) != 0)
    {
        return std::nullopt;
    }

    Preview preview{
        .author =
            cleanText(root.value(QStringLiteral("author_name")).toString(),
                      MAX_AUTHOR_LENGTH),
        .title = cleanText(root.value(QStringLiteral("title")).toString(),
                           MAX_TITLE_LENGTH),
    };

    if (preview.author.isEmpty() && preview.title.isEmpty())
    {
        return std::nullopt;
    }

    const QUrl thumbnail(
        root.value(QStringLiteral("thumbnail_url")).toString());
    if (thumbnail.toString().size() <= MAX_THUMBNAIL_URL_LENGTH &&
        thumbnail.isValid() && thumbnail.scheme() == QStringLiteral("https") &&
        !thumbnail.host().isEmpty())
    {
        preview.thumbnailUrl = thumbnail.toString();
    }

    return preview;
}

QString makeTooltip(const QString &displayUrl, const Preview &preview)
{
    QStringList lines{displayUrl.toHtmlEscaped()};
    if (preview.author.isEmpty())
    {
        lines.append(QStringLiteral("TikTok"));
    }
    else
    {
        lines.append(QStringLiteral("TikTok \u00b7 %1")
                         .arg(preview.author.toHtmlEscaped()));
    }
    if (!preview.title.isEmpty())
    {
        lines.append(preview.title.toHtmlEscaped());
    }
    return lines.join(QStringLiteral("<br>"));
}

QString makeUnavailableTooltip(const QString &displayUrl)
{
    return displayUrl.toHtmlEscaped() %
           QStringLiteral("<br>TikTok preview unavailable");
}

}
