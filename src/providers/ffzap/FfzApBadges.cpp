#include "providers/ffzap/FfzApBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Paths.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <mutex>

namespace chatterino {

namespace {

constexpr auto BADGES_URL = "https://api.ffzap.com/v1/supporters";
constexpr auto CACHE_FILE = "ffzap-badges.json";
constexpr auto META_FILE = "ffzap-badges.meta.json";
constexpr qsizetype MAX_PAYLOAD_SIZE = 256 * 1024;
constexpr qsizetype MAX_METADATA_SIZE = 4096;
constexpr qsizetype MAX_BADGES = 5000;
constexpr QSize BADGE_SIZE{18, 18};

const boost::unordered_flat_map<QString, QString> &specialBadgeNames()
{
    static const boost::unordered_flat_map<QString, QString> names{
        {QStringLiteral("26964566"), QStringLiteral("FFZ:AP Developer")},
        {QStringLiteral("11819690"), QStringLiteral("FFZ:AP Helper")},
        {QStringLiteral("36442149"), QStringLiteral("FFZ:AP Helper")},
        {QStringLiteral("29519423"), QStringLiteral("FFZ:AP Helper")},
        {QStringLiteral("22025290"), QStringLiteral("FFZ:AP Helper")},
        {QStringLiteral("4867723"), QStringLiteral("FFZ:AP Helper")},
    };
    return names;
}

bool jsonBool(const QJsonValue &value)
{
    if (value.isBool())
    {
        return value.toBool();
    }
    if (value.isDouble())
    {
        return value.toInt() != 0;
    }
    const auto text = value.toString().trimmed();
    return text == QStringLiteral("1") ||
           text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

QString cachePath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromLatin1(CACHE_FILE));
}

QString metadataPath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromLatin1(META_FILE));
}

EmotePtr makeBadgeEmote(const QString &id, const QString &tooltip)
{
    const auto baseUrl =
        QStringLiteral("https://api.ffzap.com/v1/user/badge/%1").arg(id);
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("ffzap:") + tooltip},
        .images =
            ImageSet{
                Image::fromUrl(Url{baseUrl + QStringLiteral("/1")}, 1.0,
                               BADGE_SIZE),
                Image::fromUrl(Url{baseUrl + QStringLiteral("/2")}, 0.5,
                               BADGE_SIZE * 2),
                Image::fromUrl(Url{baseUrl + QStringLiteral("/3")}, 0.25,
                               BADGE_SIZE * 4),
            },
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{baseUrl + QStringLiteral("/1")},
    });
}

}

void FfzApBadges::initialize()
{
    this->loadCache();
    QTimer::singleShot(3500, this, [this] {
        this->requestBadges();
    });
}

std::optional<FfzApBadges::Badge> FfzApBadges::getBadge(
    const UserId &userID) const
{
    {
        std::shared_lock lock(this->mutex_);
        const auto found = this->badges_.find(userID.string);
        if (found == this->badges_.end())
        {
            return std::nullopt;
        }
        if (found->second.emote != nullptr)
        {
            return Badge{.emote = found->second.emote,
                         .color = found->second.color};
        }
    }

    std::unique_lock lock(this->mutex_);
    const auto found = this->badges_.find(userID.string);
    if (found == this->badges_.end())
    {
        return std::nullopt;
    }
    auto &entry = found->second;
    if (entry.emote == nullptr)
    {
        entry.emote = makeBadgeEmote(userID.string, entry.tooltip);
    }
    return Badge{.emote = entry.emote, .color = entry.color};
}

void FfzApBadges::loadCache()
{
    QFile metadata(metadataPath());
    if (metadata.open(QIODevice::ReadOnly))
    {
        this->etag_ = QJsonDocument::fromJson(
                          metadata.read(MAX_METADATA_SIZE + 1))
                          .object()
                          .value(QStringLiteral("etag"))
                          .toString();
    }

    QFile cache(cachePath());
    if (cache.open(QIODevice::ReadOnly))
    {
        this->loaded_ = this->applyPayload(cache.read(MAX_PAYLOAD_SIZE + 1));
    }
}

void FfzApBadges::requestBadges(bool useEtag)
{
    if (this->requestInFlight_ || this->requestAttempts_ >= 3)
    {
        return;
    }
    this->requestInFlight_ = true;
    ++this->requestAttempts_;

    auto request = NetworkRequest(BADGES_URL)
                       .header("Accept", "application/json")
                       .maximumResponseSize(MAX_PAYLOAD_SIZE)
                       .timeout(8000)
                       .caller(this);
    if (useEtag && !this->etag_.isEmpty())
    {
        request = std::move(request).header("If-None-Match", this->etag_);
    }
    std::move(request)
        .onSuccess([this](const NetworkResult &result) {
            if (result.status() == 304)
            {
                if (!this->loaded_)
                {
                    this->etag_.clear();
                    this->retryWithoutEtag_ = true;
                }
                return;
            }
            if (!this->applyPayload(result.getData()))
            {
                qCWarning(chatterinoApp)
                    << "[FFZ:AP] Ignoring an invalid badge list.";
                this->scheduleRetry();
                return;
            }
            this->loaded_ = true;
            this->etag_ = QString::fromUtf8(result.etag());
            this->badgesUpdated.invoke();
            if (this->saveCache(result.getData()))
            {
                this->saveMetadata();
            }
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[FFZ:AP] Badge request failed:" << result.formatError();
            this->scheduleRetry();
        })
        .finally([this] {
            this->requestInFlight_ = false;
            const auto retryWithoutEtag = this->retryWithoutEtag_;
            this->retryWithoutEtag_ = false;
            if (retryWithoutEtag)
            {
                QTimer::singleShot(0, this, [this] {
                    this->requestBadges(false);
                });
            }
        })
        .execute();
}

bool FfzApBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray() ||
        document.array().size() > MAX_BADGES)
    {
        return false;
    }

    boost::unordered_flat_map<QString, BadgeEntry> badges;
    badges.reserve(static_cast<size_t>(document.array().size()));
    for (const auto &value : document.array())
    {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString().trimmed();
        bool validID = !id.isEmpty() && id.size() <= 20;
        for (const auto character : id)
        {
            validID = validID && character.isDigit();
        }
        if (!validID)
        {
            continue;
        }

        const auto named = specialBadgeNames().find(id);
        const auto tooltip = named == specialBadgeNames().end()
                                 ? QStringLiteral("FFZ:AP Supporter")
                                 : named->second;
        const auto color =
            jsonBool(object.value(QStringLiteral("badge_is_colored")))
                ? QColor(object.value(QStringLiteral("badge_color")).toString())
                : QColor{};
        badges.insert_or_assign(
            id, BadgeEntry{.tooltip = tooltip,
                           .emote = nullptr,
                           .color = color});
    }
    if (badges.empty())
    {
        return false;
    }

    std::unique_lock lock(this->mutex_);
    this->badges_ = std::move(badges);
    return true;
}

bool FfzApBadges::saveCache(const QByteArray &payload) const
{
    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(payload) != payload.size())
    {
        return false;
    }
    return file.commit();
}

void FfzApBadges::saveMetadata() const
{
    QSaveFile file(metadataPath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("etag"), this->etag_}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

void FfzApBadges::scheduleRetry()
{
    if (this->loaded_ || this->requestAttempts_ >= 3)
    {
        return;
    }
    const auto delay = this->requestAttempts_ == 1 ? 30'000 : 120'000;
    QTimer::singleShot(delay, this, [this] {
        this->requestBadges();
    });
}

}
