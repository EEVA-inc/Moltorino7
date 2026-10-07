#include "providers/twitch/TwitchRoles.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "util/PostToThread.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <utility>

namespace chatterino::twitch_roles {
namespace {

const auto API_BASE = QStringLiteral("https://roles.tv/api");
constexpr qsizetype MAX_SUMMARY_BYTES = 64 * 1024;
constexpr qsizetype MAX_PAGE_BYTES = 512 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 20000;
constexpr int SUMMARY_CACHE_TTL_MS = 10 * 60 * 1000;
constexpr int PAGE_CACHE_TTL_MS = 5 * 60 * 1000;
constexpr int SUMMARY_CACHE_ENTRIES = 128;
constexpr int PAGE_CACHE_KIB = 4 * 1024;
constexpr int MAX_CURSOR_LENGTH = 4096;
constexpr int MAX_ID_LENGTH = 64;
constexpr int MAX_LOGIN_LENGTH = 64;
constexpr int MAX_DISPLAY_NAME_LENGTH = 128;
constexpr qint64 MAX_REASONABLE_TOTAL = 100'000'000;

struct CachedSummary {
    Summary summary;
    qint64 expiresAt{};
};

struct CachedPage {
    Page page;
    qint64 expiresAt{};
};

struct RolesCache {
    QMutex mutex;
    QCache<QString, CachedSummary> summaries{SUMMARY_CACHE_ENTRIES};
    QCache<QString, CachedPage> pages{PAGE_CACHE_KIB};
};

RolesCache &rolesCache()
{
    static RolesCache cache;
    return cache;
}

int roleIndex(Role role)
{
    switch (role)
    {
        case Role::Moderator:
            return 0;
        case Role::Vip:
            return 1;
        case Role::Founder:
            return 2;
        case Role::Artist:
            return 3;
    }
    return 0;
}

QString perspectivePath(Perspective perspective)
{
    return perspective == Perspective::User ? QStringLiteral("user")
                                            : QStringLiteral("channel");
}

QString normalizedUserID(const QString &userID)
{
    const auto normalized = userID.trimmed();
    if (normalized.isEmpty() || normalized.size() > MAX_ID_LENGTH ||
        !std::ranges::all_of(normalized, [](QChar character) {
            return character.isDigit();
        }))
    {
        return {};
    }
    return normalized;
}

QString summaryKey(Perspective perspective, const QString &userID)
{
    return perspectivePath(perspective) + u'\n' + userID;
}

QString pageKey(Perspective perspective, Role role, const QString &userID,
                const QString &cursor)
{
    return summaryKey(perspective, userID) + u'\n' + rolePath(role) + u'\n' +
           cursor;
}

QString boundedString(const QJsonValue &value, int maximumLength)
{
    auto text = value.toString().trimmed();
    text.truncate(maximumLength);
    return text;
}

std::optional<qint64> nonNegativeInteger(const QJsonValue &value,
                                         qint64 maximum =
                                             MAX_REASONABLE_TOTAL)
{
    if (!value.isDouble())
    {
        return std::nullopt;
    }
    const auto raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 0 || raw > maximum ||
        std::floor(raw) != raw)
    {
        return std::nullopt;
    }
    return static_cast<qint64>(raw);
}

QString invalidDataError()
{
    return QStringLiteral("Roles.tv returned invalid data.");
}

QString requestError()
{
    return QStringLiteral("Roles couldn't be loaded. Try again.");
}

qsizetype pageCostKiB(const Page &page)
{
    qsizetype bytes = sizeof(Page) + page.entries.size() * sizeof(Entry) +
                      page.nextCursor.size() * sizeof(QChar);
    for (const auto &entry : page.entries)
    {
        bytes += (entry.userID.size() + entry.login.size() +
                  entry.displayName.size()) *
                 qsizetype{sizeof(QChar)};
        constexpr qsizetype STRING_OVERHEAD = 16;
        bytes += 3 * STRING_OVERHEAD;
    }
    return std::max<qsizetype>(1, (bytes + 1023) / 1024);
}

std::optional<Summary> cachedSummary(const QString &key)
{
    auto &cache = rolesCache();
    const QMutexLocker lock(&cache.mutex);
    auto *entry = cache.summaries.object(key);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    if (entry->expiresAt <= QDateTime::currentMSecsSinceEpoch())
    {
        cache.summaries.remove(key);
        return std::nullopt;
    }
    return entry->summary;
}

std::optional<Page> cachedPage(const QString &key)
{
    auto &cache = rolesCache();
    const QMutexLocker lock(&cache.mutex);
    auto *entry = cache.pages.object(key);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    if (entry->expiresAt <= QDateTime::currentMSecsSinceEpoch())
    {
        cache.pages.remove(key);
        return std::nullopt;
    }
    return entry->page;
}

void cacheSummary(const QString &key, Summary summary)
{
    auto &cache = rolesCache();
    const QMutexLocker lock(&cache.mutex);
    cache.summaries.insert(
        key, new CachedSummary{std::move(summary),
                               QDateTime::currentMSecsSinceEpoch() +
                                   SUMMARY_CACHE_TTL_MS});
}

void cachePage(const QString &key, Page page)
{
    const auto cost = pageCostKiB(page);
    if (cost > PAGE_CACHE_KIB)
    {
        return;
    }
    auto &cache = rolesCache();
    const QMutexLocker lock(&cache.mutex);
    cache.pages.insert(
        key,
        new CachedPage{std::move(page),
                       QDateTime::currentMSecsSinceEpoch() + PAGE_CACHE_TTL_MS},
        static_cast<int>(cost));
}

template <typename Value, typename Callback>
void deliverCached(Value value, Callback callback)
{
    if (auto *application = QCoreApplication::instance())
    {
        QTimer::singleShot(0, application,
                           [value = std::move(value),
                            callback = std::move(callback)]() mutable {
                               callback(std::move(value));
                           });
        return;
    }
    callback(std::move(value));
}

template <typename Parser, typename Success, typename NotFound>
void loadParsed(const QUrl &url, qsizetype maximumBytes, Parser parser,
                Success onSuccess, NotFound onNotFound, ErrorCallback onError)
{
    NetworkRequest(url)
        .timeout(REQUEST_TIMEOUT_MS)
        .maximumResponseSize(maximumBytes)
        .concurrent()
        .onSuccess([parser = std::move(parser),
                    onSuccess = std::move(onSuccess),
                    onError](const NetworkResult &result) mutable {
            QString error;
            auto parsed = parser(result.getData(), &error);
            runInGuiThread([parsed = std::move(parsed),
                            error = std::move(error),
                            onSuccess = std::move(onSuccess),
                            onError]() mutable {
                if (!parsed)
                {
                    onError(error.isEmpty() ? invalidDataError() : error);
                    return;
                }
                onSuccess(std::move(*parsed));
            });
        })
        .onError([onNotFound = std::move(onNotFound),
                  onError = std::move(onError)](
                     const NetworkResult &result) mutable {
            if (result.status() == 404)
            {
                runInGuiThread(
                    [onNotFound = std::move(onNotFound)]() mutable {
                        onNotFound();
                    });
                return;
            }
            runInGuiThread([onError = std::move(onError)] {
                onError(requestError());
            });
        })
        .execute();
}

}

qint64 Summary::count(Role role) const
{
    return this->counts.at(static_cast<size_t>(roleIndex(role)));
}

QString rolePath(Role role)
{
    switch (role)
    {
        case Role::Moderator:
            return QStringLiteral("moderators");
        case Role::Vip:
            return QStringLiteral("vips");
        case Role::Founder:
            return QStringLiteral("founders");
        case Role::Artist:
            return QStringLiteral("artists");
    }
    return {};
}

void loadSummary(Perspective perspective, const QString &userID,
                 SummaryCallback onSuccess, ErrorCallback onError)
{
    const auto normalized = normalizedUserID(userID);
    if (normalized.isEmpty())
    {
        onError(QStringLiteral("This Twitch user is not ready yet."));
        return;
    }
    const auto key = summaryKey(perspective, normalized);
    if (auto summary = cachedSummary(key))
    {
        deliverCached(std::move(*summary), std::move(onSuccess));
        return;
    }

    auto success = [key,
                    onSuccess = std::move(onSuccess)](Summary summary) mutable {
        cacheSummary(key, summary);
        onSuccess(std::move(summary));
    };
    auto notFoundSuccess = success;
    loadParsed(
        detail::summaryUrl(perspective, normalized), MAX_SUMMARY_BYTES,
        detail::parseSummary, std::move(success),
        [onSuccess = std::move(notFoundSuccess)]() mutable {
            onSuccess(Summary{});
        },
        std::move(onError));
}

void loadPage(Perspective perspective, Role role, const QString &userID,
              const QString &cursor, PageCallback onSuccess,
              ErrorCallback onError)
{
    const auto normalized = normalizedUserID(userID);
    if (normalized.isEmpty() || cursor.size() > MAX_CURSOR_LENGTH)
    {
        onError(QStringLiteral("This role page is not available."));
        return;
    }
    const auto key = pageKey(perspective, role, normalized, cursor);
    if (auto page = cachedPage(key))
    {
        deliverCached(std::move(*page), std::move(onSuccess));
        return;
    }

    auto success = [key, cursor, onSuccess = std::move(onSuccess),
                    onError](Page page) mutable {
        if (detail::repeatsCursor(cursor, page.nextCursor))
        {
            onError(invalidDataError());
            return;
        }
        cachePage(key, page);
        onSuccess(std::move(page));
    };
    auto notFoundSuccess = success;
    loadParsed(
        detail::pageUrl(perspective, role, normalized, cursor), MAX_PAGE_BYTES,
        detail::parsePage, std::move(success),
        [onSuccess = std::move(notFoundSuccess)]() mutable {
            onSuccess(Page{});
        },
        std::move(onError));
}

namespace detail {

QUrl summaryUrl(Perspective perspective, const QString &userID)
{
    const auto normalized = normalizedUserID(userID);
    if (normalized.isEmpty())
    {
        return {};
    }
    QUrl url(API_BASE);
    url.setPath(QStringLiteral("/api/%1/id/%2")
                    .arg(perspectivePath(perspective), normalized));
    return url;
}

QUrl pageUrl(Perspective perspective, Role role, const QString &userID,
             const QString &cursor)
{
    const auto normalized = normalizedUserID(userID);
    if (normalized.isEmpty() || cursor.size() > MAX_CURSOR_LENGTH)
    {
        return {};
    }
    QUrl url(API_BASE);
    url.setPath(QStringLiteral("/api/stats/%1/%2/id/%3")
                    .arg(perspectivePath(perspective), rolePath(role),
                         normalized));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("per_page"),
                       QString::number(PAGE_SIZE));
    if (!cursor.isEmpty())
    {
        query.addQueryItem(QStringLiteral("after"), cursor);
    }
    url.setQuery(query);
    return url;
}

QUrl websiteUrl(Perspective perspective, const QString &login)
{
    const auto normalized = login.trimmed().toLower();
    if (normalized.isEmpty() || normalized.size() > MAX_LOGIN_LENGTH)
    {
        return {};
    }
    QUrl url(QStringLiteral("https://roles.tv"));
    url.setPath((perspective == Perspective::User ? QStringLiteral("/u/")
                                                  : QStringLiteral("/c/")) +
                normalized);
    return url;
}

bool repeatsCursor(const QString &requestedCursor, const QString &nextCursor)
{
    return !nextCursor.isEmpty() && nextCursor == requestedCursor;
}

void sortNewestFirst(QVector<Entry> &entries)
{
    std::ranges::sort(entries, [](const Entry &left, const Entry &right) {
        if (left.grantedDate.isValid() != right.grantedDate.isValid())
        {
            return left.grantedDate.isValid();
        }
        if (left.grantedDate != right.grantedDate)
        {
            return left.grantedDate > right.grantedDate;
        }
        const auto displayOrder =
            left.displayName.compare(right.displayName, Qt::CaseInsensitive);
        if (displayOrder != 0)
        {
            return displayOrder < 0;
        }
        return left.userID < right.userID;
    });
}

std::optional<Summary> parseSummary(const QByteArray &data, QString *error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }
    const auto object = document.object().value(QStringLiteral("data"));
    if (!object.isObject())
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }
    const auto user = object.toObject();
    const auto rolesValue = user.value(QStringLiteral("roles"));
    const auto userID = boundedString(user.value(QStringLiteral("id")),
                                      MAX_ID_LENGTH);
    const auto login = boundedString(user.value(QStringLiteral("login")),
                                     MAX_LOGIN_LENGTH);
    if (userID.isEmpty() || login.isEmpty() ||
        (!rolesValue.isObject() && !rolesValue.isNull() &&
         !rolesValue.isUndefined()))
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }

    Summary summary;
    summary.found = true;
    summary.userID = userID;
    summary.login = login;
    summary.displayName = boundedString(
        user.value(QStringLiteral("displayName")), MAX_DISPLAY_NAME_LENGTH);
    if (summary.displayName.isEmpty())
    {
        summary.displayName = login;
    }
    if (!rolesValue.isObject())
    {
        return summary;
    }
    const auto roles = rolesValue.toObject();
    for (const auto role : ROLES)
    {
        const auto count = nonNegativeInteger(roles.value(rolePath(role)));
        if (!count)
        {
            if (error)
            {
                *error = invalidDataError();
            }
            return std::nullopt;
        }
        summary.counts.at(static_cast<size_t>(roleIndex(role))) = *count;
    }
    return summary;
}

std::optional<Page> parsePage(const QByteArray &data, QString *error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }
    const auto object = document.object();
    const auto total = nonNegativeInteger(object.value(QStringLiteral("total")));
    const auto cursorValue = object.value(QStringLiteral("cursor"));
    if (!total || (!cursorValue.isNull() && !cursorValue.isString()))
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }

    Page page;
    page.total = *total;
    if (cursorValue.isString())
    {
        page.nextCursor = cursorValue.toString();
        if (page.nextCursor.size() > MAX_CURSOR_LENGTH)
        {
            if (error)
            {
                *error = invalidDataError();
            }
            return std::nullopt;
        }
    }

    const auto dataValue = object.value(QStringLiteral("data"));
    if (*total == 0 && dataValue.isUndefined())
    {
        return page;
    }
    if (!dataValue.isArray() || dataValue.toArray().size() > PAGE_SIZE)
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }

    QSet<QString> knownIDs;
    for (const auto &value : dataValue.toArray())
    {
        if (!value.isObject())
        {
            if (error)
            {
                *error = invalidDataError();
            }
            return std::nullopt;
        }
        const auto entryObject = value.toObject();
        if (entryObject.value(QStringLiteral("active")).isBool() &&
            !entryObject.value(QStringLiteral("active")).toBool())
        {
            continue;
        }
        Entry entry;
        entry.userID = boundedString(entryObject.value(QStringLiteral("id")),
                                     MAX_ID_LENGTH);
        entry.login = boundedString(
            entryObject.value(QStringLiteral("login")), MAX_LOGIN_LENGTH);
        if (entry.userID.isEmpty() || entry.login.isEmpty())
        {
            if (error)
            {
                *error = invalidDataError();
            }
            return std::nullopt;
        }
        if (knownIDs.contains(entry.userID))
        {
            continue;
        }
        knownIDs.insert(entry.userID);
        entry.displayName = boundedString(
            entryObject.value(QStringLiteral("displayName")),
            MAX_DISPLAY_NAME_LENGTH);
        if (entry.displayName.isEmpty())
        {
            entry.displayName = entry.login;
        }
        entry.grantedDate = QDate::fromString(
            entryObject.value(QStringLiteral("grantedAt"))
                .toString()
                .first(10),
            Qt::ISODate);
        entry.partner = entryObject.value(QStringLiteral("isPartner")).toBool();
        entry.affiliate =
            entryObject.value(QStringLiteral("isAffiliate")).toBool();
        page.entries.push_back(std::move(entry));
    }
    if (*total > 0 && page.entries.isEmpty())
    {
        if (error)
        {
            *error = invalidDataError();
        }
        return std::nullopt;
    }
    return page;
}

}
}
