#pragma once

#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <optional>

namespace chatterino::tiktok {

enum class LinkKind {
    Post,
    Profile,
    Short,
};

struct LinkTarget {
    LinkKind kind;
    QUrl url;
    QString cacheKey;
};

struct Preview {
    QString author;
    QString title;
    QString thumbnailUrl;
};

std::optional<LinkTarget> parseLink(const QUrl &url);

QUrl makeOEmbedUrl(const QUrl &tiktokUrl);

std::optional<Preview> parseOEmbed(const QJsonObject &root);

QString makeTooltip(const QString &displayUrl, const Preview &preview);

QString makeUnavailableTooltip(const QString &displayUrl);

}
