// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkResolver.hpp"

#include "common/Env.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/TikTokLinkResolver.hpp"
#include "singletons/Settings.hpp"

#include <QCache>
#include <QStringBuilder>

#include <chrono>
#include <optional>

namespace chatterino {

namespace {
using Clock = std::chrono::steady_clock;

constexpr qsizetype TIKTOK_CACHE_LIMIT = 32;
constexpr auto TIKTOK_CACHE_LIFETIME = std::chrono::minutes(30);
constexpr int TIKTOK_TIMEOUT_MS = 8000;
constexpr int TIKTOK_MAX_REDIRECTS = 4;
constexpr qsizetype TIKTOK_MAX_RESPONSE_SIZE = 64 * 1024;

struct CachedTikTokPreview {
    tiktok::Preview preview;
    QString resolvedUrl;
    Clock::time_point expiresAt;
};

QCache<QString, CachedTikTokPreview> &tiktokCache()
{
    static QCache<QString, CachedTikTokPreview> cache(TIKTOK_CACHE_LIMIT);
    return cache;
}

std::optional<CachedTikTokPreview> cachedTikTokPreview(const QString &key)
{
    auto &cache = tiktokCache();
    auto *preview = cache.object(key);
    if (!preview)
    {
        return std::nullopt;
    }
    if (preview->expiresAt <= Clock::now())
    {
        cache.remove(key);
        return std::nullopt;
    }
    return *preview;
}

void cacheTikTokPreview(const QString &key, const CachedTikTokPreview &preview)
{
    tiktokCache().insert(key, new CachedTikTokPreview(preview));
}

void applyTikTokPreview(LinkInfo *info, const CachedTikTokPreview &cached)
{
    const bool showResolved =
        getSettings()->unshortLinks && !cached.resolvedUrl.isEmpty();
    const auto displayUrl =
        showResolved ? cached.resolvedUrl : info->originalUrl();

    info->setTooltip(tiktok::makeTooltip(displayUrl, cached.preview));
    info->setThumbnailSizeRange(180, 300);
    if (!cached.preview.thumbnailUrl.isEmpty())
    {
        info->setThumbnail(Image::fromUrl({cached.preview.thumbnailUrl}));
    }
    if (showResolved)
    {
        info->setResolvedUrl(cached.resolvedUrl);
    }
    info->setState(LinkInfo::State::Resolved);
}

void failTikTokPreview(LinkInfo *info, const QString &reason)
{
    const auto logUrl = QUrl(info->originalUrl()).adjusted(
        QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
    qCDebug(chatterinoHTTP)
        << "TikTok link preview failed for" << logUrl << reason;
    info->setTooltip(tiktok::makeUnavailableTooltip(info->originalUrl()));
    info->setState(LinkInfo::State::Errored);
}

void requestTikTokOEmbed(LinkInfo *info, const tiktok::LinkTarget &target,
                         QString aliasCacheKey = {})
{
    NetworkRequest(tiktok::makeOEmbedUrl(target.url))
        .caller(info)
        .timeout(TIKTOK_TIMEOUT_MS)
        .followRedirects(false)
        .maximumResponseSize(TIKTOK_MAX_RESPONSE_SIZE)
        .header("Accept", "application/json")
        .onSuccess([info, target, aliasCacheKey = std::move(aliasCacheKey)](
                       const NetworkResult &result) {
            const auto preview = tiktok::parseOEmbed(result.parseJson());
            if (!preview)
            {
                failTikTokPreview(info, QStringLiteral("invalid response"));
                return;
            }

            const CachedTikTokPreview cached{
                .preview = *preview,
                .resolvedUrl = target.url.toString(),
                .expiresAt = Clock::now() + TIKTOK_CACHE_LIFETIME,
            };
            cacheTikTokPreview(target.cacheKey, cached);
            if (!aliasCacheKey.isEmpty() && aliasCacheKey != target.cacheKey)
            {
                cacheTikTokPreview(aliasCacheKey, cached);
            }
            applyTikTokPreview(info, cached);
        })
        .onError([info](const NetworkResult &result) {
            failTikTokPreview(info, result.formatError());
        })
        .execute();
}

void resolveShortTikTokLink(LinkInfo *info,
                            const tiktok::LinkTarget &shortTarget)
{
    NetworkRequest(shortTarget.url, NetworkRequestType::Head)
        .caller(info)
        .timeout(TIKTOK_TIMEOUT_MS)
        .followRedirects(true)
        .maximumRedirectsAllowed(TIKTOK_MAX_REDIRECTS)
        .onSuccess([info, shortTarget](const NetworkResult &result) {
            const auto target = tiktok::parseLink(result.url());
            if (!target || target->kind == tiktok::LinkKind::Short)
            {
                failTikTokPreview(info, QStringLiteral("invalid redirect"));
                return;
            }

            if (const auto cached = cachedTikTokPreview(target->cacheKey))
            {
                cacheTikTokPreview(shortTarget.cacheKey, *cached);
                applyTikTokPreview(info, *cached);
                return;
            }

            requestTikTokOEmbed(info, *target, shortTarget.cacheKey);
        })
        .onError([info](const NetworkResult &result) {
            failTikTokPreview(info, result.formatError());
        })
        .execute();
}

void resolveGenericLink(LinkInfo *info)
{
    using State = LinkInfo::State;
    NetworkRequest(Env::get().linkResolverUrl.arg(QString::fromUtf8(
                       QUrl::toPercentEncoding(info->originalUrl(), {}, "/:"))))
        .caller(info)
        .timeout(30000)
        .maximumResponseSize(1024 * 1024)
        .onSuccess([info](const NetworkResult &result) {
            const auto root = result.parseJson();
            QString response;
            QString url;
            ImagePtr thumbnail = nullptr;
            if (root["status"].toInt() == 200)
            {
                response = root["tooltip"].toString();

                if (root.contains("thumbnail"))
                {
                    info->setThumbnail(
                        Image::fromUrl({root["thumbnail"].toString()}));
                }
                if (getSettings()->unshortLinks && root.contains("link"))
                {
                    info->setResolvedUrl(root["link"].toString());
                }
            }
            else
            {
                response = root["message"].toString();
            }

            info->setTooltip(QUrl::fromPercentEncoding(response.toUtf8()));
            info->setState(State::Resolved);
        })
        .onError([info](const auto &result) {
            info->setTooltip(u"No link info found (" % result.formatError() %
                             u')');
            info->setState(State::Errored);
        })
        .execute();
}

}

void LinkResolver::resolve(LinkInfo *info)
{
    using State = LinkInfo::State;

    assert(info);

    if (info->state() != State::Created)
    {

        return;
    }

    if (!getSettings()->linkInfoTooltip)
    {
        return;
    }

    info->setTooltip("Loading...");
    info->setState(State::Loading);

    const auto originalUrl = info->originalUrl();
    if (!originalUrl.contains(QStringLiteral("tiktok.com"),
                              Qt::CaseInsensitive))
    {
        resolveGenericLink(info);
        return;
    }

    const auto tiktokTarget =
        tiktok::parseLink(QUrl::fromUserInput(originalUrl));
    if (!tiktokTarget)
    {
        resolveGenericLink(info);
        return;
    }

    if (const auto cached = cachedTikTokPreview(tiktokTarget->cacheKey))
    {
        applyTikTokPreview(info, *cached);
        return;
    }

    if (tiktokTarget->kind == tiktok::LinkKind::Short)
    {
        resolveShortTikTokLink(info, *tiktokTarget);
        return;
    }

    requestTikTokOEmbed(info, *tiktokTarget);
}

}
