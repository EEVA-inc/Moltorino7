#include "providers/twitch/TwitchUserLogs.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "util/PostToThread.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace chatterino::twitch_user_logs {
namespace {

const auto API_BASE = QStringLiteral("https://logs.zonian.dev");
constexpr qsizetype MAX_INDEX_BYTES = 1024 * 1024;
constexpr qsizetype MAX_PAGE_CHUNK_BYTES = 16 * 1024 * 1024;
constexpr qsizetype MAX_SEARCH_PAGE_BYTES = 4 * 1024 * 1024;
constexpr qsizetype MAX_MESSAGE_LOOKUP_BYTES = 128 * 1024;
constexpr qsizetype MAX_ANCHOR_CHARACTERS = 128;
constexpr int REQUEST_TIMEOUT_MS = 45000;
constexpr int PERIOD_CACHE_TTL_MS = 5 * 60 * 1000;
constexpr int PAGE_CACHE_TTL_MS = 90 * 1000;
constexpr int PERIOD_CACHE_ENTRIES = 128;
constexpr int PAGE_CACHE_KIB = 12 * 1024;

struct CachedPeriods {
    QVector<Period> periods;
    qint64 expiresAt{};
    uint64_t serial{};
};

struct CachedPage {
    Page page;
    qint64 expiresAt{};
    uint64_t serial{};
};

struct LogCache {
    QMutex mutex;
    QCache<QString, CachedPeriods> periods{PERIOD_CACHE_ENTRIES};
    QCache<QString, CachedPage> pages{PAGE_CACHE_KIB};
    std::atomic<uint64_t> nextSerial{1};
};

LogCache &logCache()
{
    static LogCache cache;
    return cache;
}

QString normalizedLogin(const QString &login)
{
    return login.trimmed().toLower();
}

QString identityKey(const QString &channel, const QString &user)
{
    return normalizedLogin(channel) + u'\n' + normalizedLogin(user);
}

QString pageKey(const QString &channel, const QString &user,
                const Period &period)
{
    return identityKey(channel, user) + u'\n' + period.key();
}

void addPageWindow(QUrlQuery &query, qsizetype offset = 0)
{
    query.addQueryItem(QStringLiteral("limit"),
                       QString::number(detail::LOG_PAGE_SIZE));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
}

void addMessageLookup(QUrlQuery &query, qsizetype sourceOffset)
{
    query.addQueryItem(QStringLiteral("jsonBasic"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(sourceOffset));
}

void addSearchWindow(QUrlQuery &query, qsizetype offset)
{
    query.addQueryItem(QStringLiteral("reverse"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("limit"),
                       QString::number(detail::SEARCH_PAGE_SIZE + 1));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
}

qsizetype pageCostKiB(const Page &page)
{
    qsizetype bytes = sizeof(Page) + page.messages.size() * sizeof(Message);
    for (const auto &message : page.messages)
    {
        constexpr qsizetype STRING_ALLOCATION_OVERHEAD = 16;
        bytes +=
            (message.text.size() + message.siteAnchor.size()) *
                qsizetype{sizeof(QChar)} +
            (message.text.isEmpty() ? 0 : STRING_ALLOCATION_OVERHEAD) +
            (message.siteAnchor.isEmpty() ? 0 : STRING_ALLOCATION_OVERHEAD);
    }
    return std::max<qsizetype>(1, (bytes + 1023) / 1024);
}

std::optional<QVector<Period>> cachedPeriods(const QString &key)
{
    auto &cache = logCache();
    const QMutexLocker lock(&cache.mutex);
    auto *entry = cache.periods.object(key);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    if (entry->expiresAt <= QDateTime::currentMSecsSinceEpoch())
    {
        cache.periods.remove(key);
        return std::nullopt;
    }
    return entry->periods;
}

std::optional<Page> cachedPage(const QString &key)
{
    auto &cache = logCache();
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

void cachePeriods(const QString &key, QVector<Period> periods)
{
    auto &cache = logCache();
    const auto serial = cache.nextSerial.fetch_add(1);
    {
        const QMutexLocker lock(&cache.mutex);
        cache.periods.insert(
            key, new CachedPeriods{
                     std::move(periods),
                     QDateTime::currentMSecsSinceEpoch() + PERIOD_CACHE_TTL_MS,
                     serial});
    }
    if (auto *application = QCoreApplication::instance())
    {
        QTimer::singleShot(PERIOD_CACHE_TTL_MS, application, [key, serial] {
            auto &current = logCache();
            const QMutexLocker lock(&current.mutex);
            auto *entry = current.periods.object(key);
            if (entry != nullptr && entry->serial == serial)
            {
                current.periods.remove(key);
            }
        });
    }
}

void cachePage(const QString &key, Page page)
{
    const auto cost = pageCostKiB(page);
    if (cost > PAGE_CACHE_KIB)
    {
        return;
    }

    auto &cache = logCache();
    const auto serial = cache.nextSerial.fetch_add(1);
    {
        const QMutexLocker lock(&cache.mutex);
        cache.pages.insert(key,
                           new CachedPage{std::move(page),
                                          QDateTime::currentMSecsSinceEpoch() +
                                              PAGE_CACHE_TTL_MS,
                                          serial},
                           static_cast<int>(cost));
    }
    if (auto *application = QCoreApplication::instance())
    {
        QTimer::singleShot(PAGE_CACHE_TTL_MS, application, [key, serial] {
            auto &current = logCache();
            const QMutexLocker lock(&current.mutex);
            auto *entry = current.pages.object(key);
            if (entry != nullptr && entry->serial == serial)
            {
                current.pages.remove(key);
            }
        });
    }
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

QDateTime parseTimestamp(const QString &value)
{
    auto timestamp = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(value, Qt::ISODate);
    }
    return timestamp;
}

std::optional<int> jsonInteger(const QJsonValue &value)
{
    bool ok = false;
    int number = -1;
    if (value.isString())
    {
        number = value.toString().toInt(&ok);
    }
    else if (value.isDouble())
    {
        const auto raw = value.toDouble();
        if (raw >= std::numeric_limits<int>::min() &&
            raw <= std::numeric_limits<int>::max())
        {
            number = static_cast<int>(raw);
            ok = raw == number;
        }
    }
    return ok ? std::optional<int>(number) : std::nullopt;
}

QString requestError()
{
    return QStringLiteral("Logs couldn't be loaded. Try again.");
}

template <typename Parser, typename Success>
void loadParsed(const QUrl &url, qsizetype maximumBytes, Parser parser,
                Success onSuccess, ErrorCallback onError)
{
    using Parsed = std::invoke_result_t<Parser, const QByteArray &, QString *>;
    using Value = typename Parsed::value_type;
    auto notFoundSuccess = onSuccess;
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
                    onError(error.isEmpty()
                                ? QStringLiteral(
                                      "The log service returned invalid data.")
                                : error);
                    return;
                }
                onSuccess(std::move(*parsed));
            });
        })
        .onError([onSuccess = std::move(notFoundSuccess),
                  onError =
                      std::move(onError)](const NetworkResult &result) mutable {
            if (result.status() == 404)
            {
                runInGuiThread([onSuccess = std::move(onSuccess)]() mutable {
                    onSuccess(Value{});
                });
                return;
            }
            const auto error = requestError();
            runInGuiThread([onError = std::move(onError), error] {
                onError(error);
            });
        })
        .execute();
}

struct CompletePageRequest {
    QUrl firstUrl;
    Page page;
    qsizetype offset{};
    qsizetype retainedKiB{};
    PageCallback onSuccess;
    ErrorCallback onError;
    ContinueCallback shouldContinue;
};

void loadNextPageChunk(const std::shared_ptr<CompletePageRequest> &request)
{
    if (request->shouldContinue && !request->shouldContinue())
    {
        return;
    }
    auto url = request->firstUrl;
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("limit"));
    query.removeAllQueryItems(QStringLiteral("offset"));
    addPageWindow(query, request->offset);
    url.setQuery(query);

    loadParsed(
        url, MAX_PAGE_CHUNK_BYTES, detail::parseTextPage,
        [request](Page chunk) mutable {
            if (request->shouldContinue && !request->shouldContinue())
            {
                return;
            }
            const auto sourceCount = chunk.sourceRecordCount;
            if (sourceCount > detail::LOG_PAGE_SIZE)
            {
                auto callback = std::move(request->onError);
                callback(QStringLiteral(
                    "The log service returned an invalid page."));
                return;
            }
            request->retainedKiB += pageCostKiB(chunk);
            if (request->retainedKiB > 64 * 1024 ||
                request->page.sourceRecordCount + sourceCount > 100000)
            {
                auto callback = std::move(request->onError);
                callback(QStringLiteral("This period is too large. Choose a single day."));
                return;
            }
            request->page.messages.reserve(request->page.messages.size() +
                                           chunk.messages.size());
            for (auto &message : chunk.messages)
            {
                message.sourceOffset += request->offset;
                request->page.messages.push_back(std::move(message));
            }
            request->page.availableMessageCount += chunk.availableMessageCount;
            request->page.sourceRecordCount += sourceCount;

            if (sourceCount < detail::LOG_PAGE_SIZE)
            {
                std::stable_sort(
                    request->page.messages.begin(),
                    request->page.messages.end(),
                    [](const auto &left, const auto &right) {
                        if (left.timestamp != right.timestamp)
                        {
                            return left.timestamp > right.timestamp;
                        }
                        return left.sourceOffset > right.sourceOffset;
                    });
                auto callback = std::move(request->onSuccess);
                callback(std::move(request->page));
                return;
            }

            request->offset += sourceCount;
            loadNextPageChunk(request);
        },
        [request](QString error) mutable {
            auto callback = std::move(request->onError);
            callback(std::move(error));
        });
}

void loadCompletePage(const QUrl &url, PageCallback onSuccess,
                      ErrorCallback onError, ContinueCallback shouldContinue)
{
    auto request = std::make_shared<CompletePageRequest>();
    request->firstUrl = url;
    request->onSuccess = std::move(onSuccess);
    request->onError = std::move(onError);
    request->shouldContinue = std::move(shouldContinue);
    loadNextPageChunk(request);
}

}

QString Period::key() const
{
    auto value = QStringLiteral("%1-%2")
                     .arg(this->year, 4, 10, QChar('0'))
                     .arg(this->month, 2, 10, QChar('0'));
    if (this->day)
    {
        value += QStringLiteral("-%1").arg(*this->day, 2, 10, QChar('0'));
    }
    return value;
}

QString Period::label() const
{
    if (this->day)
    {
        return QLocale(QLocale::English)
            .toString(QDate(this->year, this->month, *this->day),
                      QLocale::LongFormat);
    }
    return QStringLiteral("%1 %2")
        .arg(QLocale(QLocale::English)
                 .standaloneMonthName(this->month, QLocale::LongFormat))
        .arg(this->year);
}

QString Period::compactLabel() const
{
    const auto month =
        QLocale(QLocale::English)
            .standaloneMonthName(this->month, QLocale::ShortFormat);
    if (this->day)
    {
        return QStringLiteral("%1 %2 '%3")
            .arg(month)
            .arg(*this->day)
            .arg(this->year % 100, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1 '%2").arg(month).arg(this->year % 100, 2, 10,
                                                   QChar('0'));
}

void loadAvailablePeriods(const QString &channel, const QString &user,
                          PeriodsCallback onSuccess, ErrorCallback onError)
{
    const auto key = identityKey(channel, user);
    if (auto periods = cachedPeriods(key))
    {
        deliverCached(std::move(*periods), std::move(onSuccess));
        return;
    }
    loadParsed(
        detail::availablePeriodsUrl(channel, user), MAX_INDEX_BYTES,
        detail::parseAvailablePeriods,
        [key,
         onSuccess = std::move(onSuccess)](QVector<Period> periods) mutable {
            cachePeriods(key, periods);
            onSuccess(std::move(periods));
        },
        std::move(onError));
}

void loadPeriod(const QString &channel, const QString &user,
                const Period &period, PageCallback onSuccess,
                ErrorCallback onError, ContinueCallback shouldContinue)
{
    const auto key = pageKey(channel, user, period);
    if (auto page = cachedPage(key))
    {
        deliverCached(std::move(*page), std::move(onSuccess));
        return;
    }
    loadCompletePage(
        detail::periodUrl(channel, user, period),
        [key, onSuccess = std::move(onSuccess)](Page page) mutable {
            cachePage(key, page);
            onSuccess(std::move(page));
        },
        std::move(onError), std::move(shouldContinue));
}

void search(const QString &channel, const QString &user, const QString &query,
            qsizetype offset, PageCallback onSuccess, ErrorCallback onError,
            ContinueCallback shouldContinue)
{
    if (offset < 0)
    {
        onError(QStringLiteral("The requested log page is invalid."));
        return;
    }

    loadParsed(
        detail::searchUrl(channel, user, query, offset), MAX_SEARCH_PAGE_BYTES,
        [offset](const QByteArray &data, QString *error) {
            return detail::parseSearchPage(data, offset, error);
        },
        [shouldContinue = std::move(shouldContinue),
         onSuccess = std::move(onSuccess)](Page page) mutable {
            if (shouldContinue && !shouldContinue())
            {
                return;
            }
            onSuccess(std::move(page));
        },
        std::move(onError));
}

namespace {

void resolveMessageAnchor(const QUrl &url, const Message &expected,
                          AnchorCallback onSuccess, ErrorCallback onError)
{
    if (expected.sourceOffset < 0)
    {
        onError(QStringLiteral("The exact message link is unavailable."));
        return;
    }
    auto validationError = onError;
    loadParsed(
        url, MAX_MESSAGE_LOOKUP_BYTES, detail::parseJsonMessage,
        [expected, onSuccess = std::move(onSuccess),
         onError = std::move(validationError)](Message resolved) mutable {
            const bool matches = !resolved.siteAnchor.isEmpty() &&
                                 resolved.text == expected.text &&
                                 resolved.timestamp.toSecsSinceEpoch() ==
                                     expected.timestamp.toSecsSinceEpoch();
            if (!matches)
            {
                onError(QStringLiteral(
                    "The exact message link is no longer available."));
                return;
            }
            onSuccess(std::move(resolved.siteAnchor));
        },
        std::move(onError));
}

}

void resolvePeriodMessageAnchor(const QString &channel, const QString &user,
                                const Period &period, const Message &expected,
                                AnchorCallback onSuccess, ErrorCallback onError)
{
    resolveMessageAnchor(
        detail::periodMessageUrl(channel, user, period, expected.sourceOffset),
        expected, std::move(onSuccess), std::move(onError));
}

void resolveSearchMessageAnchor(const QString &channel, const QString &user,
                                const QString &query, const Message &expected,
                                AnchorCallback onSuccess, ErrorCallback onError)
{
    resolveMessageAnchor(
        detail::searchMessageUrl(channel, user, query, expected.sourceOffset),
        expected, std::move(onSuccess), std::move(onError));
}

namespace detail {

QUrl availablePeriodsUrl(const QString &channel, const QString &user)
{
    QUrl url(API_BASE + QStringLiteral("/list"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("channel"), normalizedLogin(channel));
    query.addQueryItem(QStringLiteral("user"), normalizedLogin(user));
    url.setQuery(query);
    return url;
}

QUrl periodUrl(const QString &channel, const QString &user,
               const Period &period)
{
    QUrl url(API_BASE);
    auto path = QStringLiteral("/channel/%1/user/%2/%3/%4")
                    .arg(normalizedLogin(channel), normalizedLogin(user))
                    .arg(period.year)
                    .arg(period.month);
    if (period.day)
    {
        path += QStringLiteral("/%1").arg(*period.day);
    }
    url.setPath(path);
    QUrlQuery query;
    addPageWindow(query);
    url.setQuery(query);
    return url;
}

QUrl searchUrl(const QString &channel, const QString &user,
               const QString &queryText, qsizetype offset)
{
    QUrl url(API_BASE);
    url.setPath(QStringLiteral("/channel/%1/user/%2/search")
                    .arg(normalizedLogin(channel), normalizedLogin(user)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), queryText.trimmed());
    addSearchWindow(query, offset);
    url.setQuery(query);
    return url;
}

QUrl periodMessageUrl(const QString &channel, const QString &user,
                      const Period &period, qsizetype sourceOffset)
{
    auto url = periodUrl(channel, user, period);
    QUrlQuery query;
    addMessageLookup(query, sourceOffset);
    url.setQuery(query);
    return url;
}

QUrl searchMessageUrl(const QString &channel, const QString &user,
                      const QString &queryText, qsizetype sourceOffset)
{
    QUrl url(API_BASE);
    url.setPath(QStringLiteral("/channel/%1/user/%2/search")
                    .arg(normalizedLogin(channel), normalizedLogin(user)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), queryText.trimmed());
    query.addQueryItem(QStringLiteral("reverse"), QStringLiteral("true"));
    addMessageLookup(query, sourceOffset);
    url.setQuery(query);
    return url;
}

QUrl messageUrl(const QString &channel, const Message &message)
{
    if (channel.trimmed().isEmpty() || !message.timestamp.isValid())
    {
        return {};
    }
    QUrl url(QStringLiteral("https://tv.supa.sh/logs"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("c"), normalizedLogin(channel));
    query.addQueryItem(QStringLiteral("d"),
                       message.timestamp.toUTC().date().toString(Qt::ISODate));
    url.setQuery(query);
    if (!message.siteAnchor.isEmpty())
    {
        url.setFragment(message.siteAnchor);
    }
    return url;
}

std::optional<QVector<Period>> parseAvailablePeriods(const QByteArray &data,
                                                     QString *error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error)
        {
            *error = QStringLiteral("The log index returned invalid data.");
        }
        return std::nullopt;
    }

    const auto value = document.object().value(QStringLiteral("availableLogs"));
    if (!value.isArray())
    {
        if (error)
        {
            *error = QStringLiteral("The log index returned invalid data.");
        }
        return std::nullopt;
    }

    QVector<Period> periods;
    QSet<QString> known;
    for (const auto &entry : value.toArray())
    {
        if (!entry.isObject())
        {
            continue;
        }
        const auto object = entry.toObject();
        const auto year = jsonInteger(object.value(QStringLiteral("year")));
        const auto month = jsonInteger(object.value(QStringLiteral("month")));
        const auto dayValue = object.value(QStringLiteral("day"));
        const auto day = dayValue.isUndefined() || dayValue.isNull()
                             ? std::optional<int>{}
                             : jsonInteger(dayValue);
        if (!year || !month || *year < 2000 || *year > 2200 || *month < 1 ||
            *month > 12 ||
            (!dayValue.isUndefined() && !dayValue.isNull() && !day) ||
            (day && (*day < 1 || *day > 31)) ||
            (day && !QDate(*year, *month, *day).isValid()))
        {
            continue;
        }

        Period period{*year, *month, day};
        if (!known.contains(period.key()))
        {
            known.insert(period.key());
            periods.push_back(std::move(period));
        }
    }

    std::sort(periods.begin(), periods.end(),
              [](const auto &left, const auto &right) {
                  return left.key() > right.key();
              });
    return periods;
}

std::optional<Page> parseTextPage(const QByteArray &data, QString *error)
{
    Page page;

    page.messages.reserve(std::min(data.count('\n') + 1, LOG_PAGE_SIZE));

    qsizetype start = 0;
    qsizetype sourceOffset = 0;
    bool sawNonEmptyLine = false;
    while (start < data.size())
    {
        auto end = data.indexOf('\n', start);
        if (end < 0)
        {
            end = data.size();
        }
        auto length = end - start;
        if (length > 0 && data.at(start + length - 1) == '\r')
        {
            --length;
        }
        if (length > 0)
        {
            sawNonEmptyLine = true;
            const auto offset = sourceOffset++;
            const auto line = QByteArrayView(data).sliced(start, length);
            constexpr qsizetype TIMESTAMP_LENGTH = 19;
            constexpr qsizetype MESSAGE_PREFIX_LENGTH = 23;
            if (line.size() >= MESSAGE_PREFIX_LENGTH && line.at(0) == '[' &&
                line.at(TIMESTAMP_LENGTH + 1) == ']' &&
                line.at(TIMESTAMP_LENGTH + 2) == ' ' &&
                line.at(TIMESTAMP_LENGTH + 3) == '#')
            {
                const auto timestampText =
                    QString::fromLatin1(line.sliced(1, TIMESTAMP_LENGTH));
                const auto date = QDate::fromString(
                    timestampText.first(10), QStringLiteral("yyyy-MM-dd"));
                const auto time = QTime::fromString(timestampText.sliced(11),
                                                    QStringLiteral("HH:mm:ss"));
                const auto channelEnd =
                    line.indexOf(' ', MESSAGE_PREFIX_LENGTH);
                const auto messageStart =
                    channelEnd < 0 ? -1 : line.indexOf(": ", channelEnd + 1);
                if (date.isValid() && time.isValid() && messageStart >= 0)
                {
                    auto text =
                        QString::fromUtf8(line.sliced(messageStart + 2));
                    if (!text.isEmpty())
                    {
                        ++page.availableMessageCount;
                        text.replace(u'\r', u' ');
                        text.replace(u'\n', u' ');
                        page.messages.push_back({
                            .text = std::move(text),
                            .timestamp =
                                QDateTime(date, time, QTimeZone::utc()),
                            .sourceOffset = offset,
                        });
                    }
                }
            }
        }
        if (end == data.size())
        {
            break;
        }
        start = end + 1;
    }

    if (sawNonEmptyLine && page.availableMessageCount == 0)
    {
        if (error)
        {
            *error = QStringLiteral("The log service returned invalid data.");
        }
        return std::nullopt;
    }
    page.sourceRecordCount = sourceOffset;
    return page;
}

std::optional<Page> parseSearchPage(const QByteArray &data, qsizetype offset,
                                    QString *error)
{
    if (offset < 0)
    {
        if (error)
        {
            *error = QStringLiteral("The requested log page is invalid.");
        }
        return std::nullopt;
    }

    auto page = parseTextPage(data, error);
    if (!page)
    {
        return std::nullopt;
    }

    page->hasMoreResults = page->sourceRecordCount > SEARCH_PAGE_SIZE;
    page->sourceRecordCount =
        std::min(page->sourceRecordCount, SEARCH_PAGE_SIZE);
    page->messages.erase(
        std::remove_if(page->messages.begin(), page->messages.end(),
                       [](const Message &message) {
                           return message.sourceOffset >= SEARCH_PAGE_SIZE;
                       }),
        page->messages.end());
    page->availableMessageCount = page->messages.size();
    for (auto &message : page->messages)
    {
        message.sourceOffset += offset;
    }
    std::stable_sort(page->messages.begin(), page->messages.end(),
                     [](const auto &left, const auto &right) {
                         if (left.timestamp != right.timestamp)
                         {
                             return left.timestamp > right.timestamp;
                         }
                         return left.sourceOffset < right.sourceOffset;
                     });
    return page;
}

std::optional<Message> parseJsonMessage(const QByteArray &data, QString *error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error)
        {
            *error = QStringLiteral("The log service returned invalid data.");
        }
        return std::nullopt;
    }

    const auto value = document.object().value(QStringLiteral("messages"));
    if (!value.isArray())
    {
        if (error)
        {
            *error = QStringLiteral("The exact message link is unavailable.");
        }
        return std::nullopt;
    }

    const auto messages = value.toArray();
    if (messages.size() != 1 || !messages.at(0).isObject())
    {
        if (error)
        {
            *error = QStringLiteral("The exact message link is unavailable.");
        }
        return std::nullopt;
    }

    const auto object = messages.at(0).toObject();
    auto text = object.value(QStringLiteral("text")).toString();
    auto siteAnchor = object.value(QStringLiteral("id")).toString();
    const auto timestamp =
        parseTimestamp(object.value(QStringLiteral("timestamp")).toString());
    if (text.isEmpty() || siteAnchor.isEmpty() || !timestamp.isValid())
    {
        if (error)
        {
            *error = QStringLiteral("The exact message link is unavailable.");
        }
        return std::nullopt;
    }
    text.replace(u'\r', u' ');
    text.replace(u'\n', u' ');
    siteAnchor.truncate(MAX_ANCHOR_CHARACTERS);
    return Message{
        .text = std::move(text),
        .siteAnchor = std::move(siteAnchor),
        .timestamp = timestamp,
    };
}

}
}
