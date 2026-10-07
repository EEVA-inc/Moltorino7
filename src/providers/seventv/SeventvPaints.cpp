#include "providers/seventv/SeventvPaints.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/paints/LinearGradientPaint.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/paints/RadialGradientPaint.hpp"
#include "providers/seventv/paints/UrlPaint.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/PostToThread.hpp"
#include "util/Variant.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace {
using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr std::chrono::minutes SEVENTV_PAINT_FRAME_CACHE_LIFETIME{4};
constexpr int SEVENTV_PAINT_REQUEST_TIMEOUT_MS = 15000;
constexpr std::chrono::milliseconds SEVENTV_PAINT_REQUEST_WATCHDOG{17000};

const QUrl SEVENTV_GQL_URL(QStringLiteral("https://7tv.io/v3/gql"));

const QString SEVENTV_PAINT_QUERY = QStringLiteral(R"(
query MoltorinoPaintByID($list: [Id!]!) {
  cosmetics(list: $list) {
    paints {
      id
      name
      color
      function
      repeat
      angle
      shape
      image_url
      stops { at color }
      shadows { x_offset y_offset radius color }
    }
  }
}
)");

QColor rgbaToQColor(const uint32_t color)
{
    auto red = (int)((color >> 24) & 0xFF);
    auto green = (int)((color >> 16) & 0xFF);
    auto blue = (int)((color >> 8) & 0xFF);
    auto alpha = (int)(color & 0xFF);

    return {red, green, blue, alpha};
}

std::optional<QColor> parsePaintColor(const QJsonValue &color)
{
    if (color.isNull())
    {
        return std::nullopt;
    }

    return rgbaToQColor(static_cast<uint32_t>(color.toInteger()));
}

QGradientStops parsePaintStops(const QJsonArray &stops)
{
    QGradientStops parsedStops;
    double lastStop = -1;

    for (const auto &stop : stops)
    {
        const auto stopObject = stop.toObject();

        const auto rgbaColor =
            static_cast<uint32_t>(stopObject["color"].toInteger());
        auto position = stopObject["at"].toDouble();

        if (position <= lastStop)
        {
            position = lastStop + 0.0000001;
        }

        lastStop = position;
        parsedStops.append(QGradientStop(position, rgbaToQColor(rgbaColor)));
    }

    return parsedStops;
}

std::vector<PaintDropShadow> parseDropShadows(const QJsonArray &dropShadows)
{
    std::vector<PaintDropShadow> parsedDropShadows;

    for (const auto &shadow : dropShadows)
    {
        const auto shadowObject = shadow.toObject();

        const auto xOffset = shadowObject["x_offset"].toDouble();
        const auto yOffset = shadowObject["y_offset"].toDouble();
        const auto radius = shadowObject["radius"].toDouble();
        const auto rgbaColor =
            static_cast<uint32_t>(shadowObject["color"].toInteger());

        parsedDropShadows.emplace_back(xOffset, yOffset, radius,
                                       rgbaToQColor(rgbaColor));
    }

    return parsedDropShadows;
}

std::optional<std::shared_ptr<Paint>> parsePaint(const QJsonObject &paintJson)
{
    const QString name = paintJson["name"].toString();
    const QString id = paintJson["id"].toString();

    const auto color = parsePaintColor(paintJson["color"]);
    const bool repeat = paintJson["repeat"].toBool();
    const float angle = (float)paintJson["angle"].toDouble();

    const QGradientStops stops = parsePaintStops(paintJson["stops"].toArray());

    const auto shadows = parseDropShadows(paintJson["shadows"].toArray());

    const QString function = paintJson["function"].toString();
    if (function == "LINEAR_GRADIENT" || function == "linear-gradient")
    {
        return std::make_shared<LinearGradientPaint>(name, id, color, stops,
                                                     repeat, angle, shadows);
    }

    if (function == "RADIAL_GRADIENT" || function == "radial-gradient")
    {
        return std::make_shared<RadialGradientPaint>(name, id, stops, repeat,
                                                     shadows);
    }

    if (function == "URL" || function == "url")
    {
        const QString url = paintJson["image_url"].toString().trimmed();
        if (url.isEmpty())
        {
            return std::nullopt;
        }
        const ImagePtr image = Image::fromUrl({url}, 1);
        if (image == nullptr)
        {
            return std::nullopt;
        }
        image->setFrameCacheLifetime(SEVENTV_PAINT_FRAME_CACHE_LIFETIME);

        return std::make_shared<UrlPaint>(name, id, image, shadows);
    }

    return std::nullopt;
}

}

namespace chatterino {

bool seventv::detail::shouldStartPaintRequest(
    SeventvPaintLoadStatus status, bool retry, bool queued, bool stalled)
{
    switch (status)
    {
        case SeventvPaintLoadStatus::Unknown:
            return true;
        case SeventvPaintLoadStatus::Ready:
            return false;
        case SeventvPaintLoadStatus::NotFound:
        case SeventvPaintLoadStatus::Failed:
            return retry;
        case SeventvPaintLoadStatus::Loading:
            return retry && !queued && stalled;
    }
    return false;
}

SeventvPaints::SeventvPaints() = default;

std::shared_ptr<Paint> SeventvPaints::getPaint(const QString &userName,
                                               bool kick) const
{
    std::shared_lock lock(this->mutex_);

    if (kick)
    {
        const auto it = this->kickPaintMap_.find(userName.toLower());
        if (it != this->kickPaintMap_.end())
        {
            return it->second;
        }
    }
    else
    {
        const auto it = this->twitchPaintMap_.find(userName.toLower());
        if (it != this->twitchPaintMap_.end())
        {
            return it->second;
        }
    }
    return nullptr;
}

std::shared_ptr<Paint> SeventvPaints::getPaintByID(const QString &paintID) const
{
    const auto normalized = normalizePaintID(paintID);
    if (normalized.isEmpty())
    {
        return nullptr;
    }

    std::shared_lock lock(this->mutex_);
    const auto it = this->knownPaints_.find(normalized);
    return it == this->knownPaints_.end() ? nullptr : it->second;
}

SeventvPaintLoadStatus SeventvPaints::getPaintLoadStatus(
    const QString &paintID) const
{
    const auto normalized = normalizePaintID(paintID);
    if (normalized.isEmpty())
    {
        return SeventvPaintLoadStatus::Unknown;
    }

    std::shared_lock lock(this->mutex_);
    if (this->knownPaints_.contains(normalized))
    {
        return SeventvPaintLoadStatus::Ready;
    }
    const auto it = this->paintLoadStates_.find(normalized);
    return it == this->paintLoadStates_.end()
               ? SeventvPaintLoadStatus::Unknown
               : it->second.status;
}

QString SeventvPaints::normalizePaintID(QString paintID)
{
    paintID = paintID.trimmed();
    const QUrl url(paintID);
    if (url.isValid() && !url.scheme().isEmpty())
    {
        const auto parts = url.path().split(u'/', Qt::SkipEmptyParts);
        if (!parts.isEmpty())
        {
            paintID = parts.last();
        }
    }
    else
    {
        paintID.replace(u'\\', u'/');
        if (const auto slash = paintID.lastIndexOf(u'/'); slash >= 0)
        {
            paintID = paintID.sliced(slash + 1);
        }
        if (const auto suffix = paintID.indexOf(QRegularExpression("[?#]"));
            suffix >= 0)
        {
            paintID.truncate(suffix);
        }
    }

    paintID = paintID.trimmed();
    static const QRegularExpression legacyID(
        QStringLiteral("^[0-9a-fA-F]{24}$"));
    return legacyID.match(paintID).hasMatch() ? paintID.toLower()
                                              : paintID.toUpper();
}

bool SeventvPaints::isValidPaintID(const QString &paintID)
{
    static const QRegularExpression validID(
        QStringLiteral("^(?:[0-9a-f]{24}|[0-9A-HJKMNP-TV-Z]{26})$"));
    return validID.match(normalizePaintID(paintID)).hasMatch();
}

void SeventvPaints::addPaint(const QJsonObject &paintJson)
{
    const auto paintID = normalizePaintID(paintJson["id"].toString());
    if (!isValidPaintID(paintID))
    {
        return;
    }

    std::unique_lock lock(this->mutex_);

    if (this->knownPaints_.contains(paintID))
    {
        return;
    }

    auto normalizedPaint = paintJson;
    normalizedPaint.insert("id", paintID);
    auto paint = parsePaint(normalizedPaint);
    if (!paint)
    {
        return;
    }

    DebugCount::increase(DebugObject::SeventvPaints);
    this->knownPaints_[paintID] = *paint;
    auto &state = this->paintLoadStates_[paintID];
    state.status = SeventvPaintLoadStatus::Ready;
    state.requestID = 0;
}

void SeventvPaints::loadPaintByID(const QString &paintID, bool retry)
{
    this->loadPaintsByIDInternal({paintID}, retry, retry);
}

void SeventvPaints::loadPaintsByID(const QStringList &paintIDs, bool retry)
{
    this->loadPaintsByIDInternal(paintIDs, retry, false);
}

void SeventvPaints::loadPaintsByIDInternal(const QStringList &paintIDs,
                                           bool retry, bool prioritize)
{
    std::vector<PaintBatchItem> pending;
    QSet<QString> pendingSet;
    QSet<std::uint64_t> stalledRequests;
    std::vector<std::shared_ptr<Paint>> cachedPaints;
    const auto now = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(this->mutex_);
        for (const auto &paintID : paintIDs)
        {
            const auto normalized = normalizePaintID(paintID);
            if (!isValidPaintID(normalized) || pendingSet.contains(normalized))
            {
                continue;
            }
            pendingSet.insert(normalized);
            if (const auto known = this->knownPaints_.find(normalized);
                known != this->knownPaints_.end())
            {
                auto &state = this->paintLoadStates_[normalized];
                state.status = SeventvPaintLoadStatus::Ready;
                state.requestID = 0;
                if (retry)
                {
                    cachedPaints.push_back(known->second);
                }
                continue;
            }

            auto &state = this->paintLoadStates_[normalized];
            const bool queued = state.status == SeventvPaintLoadStatus::Loading &&
                                state.requestID == 0;
            const bool stalled =
                state.status == SeventvPaintLoadStatus::Loading &&
                state.requestID != 0 &&
                state.startedAt != std::chrono::steady_clock::time_point{} &&
                now - state.startedAt >= SEVENTV_PAINT_REQUEST_WATCHDOG;
            if (!seventv::detail::shouldStartPaintRequest(
                    state.status, retry, queued, stalled))
            {
                if (prioritize && queued)
                {
                    this->pendingPaintBatches_.push_front(
                        {.paints = {{normalized, state.generation}},
                         .retry = state.retry});
                }
                continue;
            }

            if (stalled)
            {
                stalledRequests.insert(state.requestID);
            }

            state.status = SeventvPaintLoadStatus::Loading;
            state.generation = ++this->nextPaintGeneration_;
            state.requestID = 0;
            state.startedAt = {};
            state.retry = retry;
            pending.push_back({normalized, state.generation});
        }
    }

    for (const auto &paint : pending)
    {
        postToThread(
            [this, paintID = paint.paintID] {
                this->paintLoadStatusChanged.invoke(
                    paintID, SeventvPaintLoadStatus::Loading);
            },
            &this->lifetimeGuard_);
    }
    if (!cachedPaints.empty())
    {
        postToThread(
            [paints = std::move(cachedPaints)] {
                for (const auto &paint : paints)
                {
                    paint->ensureLoaded(true);
                }
            },
            &this->lifetimeGuard_);
    }

    const std::size_t batchSize = retry ? 10 : 50;
    for (std::size_t offset = 0; offset < pending.size(); offset += batchSize)
    {
        const auto end = std::min(pending.size(), offset + batchSize);
        std::vector<PaintBatchItem> batch(pending.begin() + offset,
                                          pending.begin() + end);
        this->enqueuePaintBatch(std::move(batch), retry, prioritize);
    }

    for (const auto requestID : stalledRequests)
    {
        this->handlePaintRequestWatchdog(requestID);
    }

    if (prioritize && pending.empty())
    {
        postToThread(
            [this] {
                this->pumpPaintRequests();
            },
            &this->lifetimeGuard_);
    }
}

void SeventvPaints::enqueuePaintBatch(std::vector<PaintBatchItem> paints,
                                      bool retry, bool prioritize)
{
    if (paints.empty())
    {
        return;
    }
    {
        std::unique_lock lock(this->mutex_);
        PaintBatchRequest request{.paints = std::move(paints), .retry = retry};
        if (prioritize)
        {
            this->pendingPaintBatches_.push_front(std::move(request));
        }
        else
        {
            this->pendingPaintBatches_.push_back(std::move(request));
        }
    }
    postToThread(
        [this] {
            this->pumpPaintRequests();
        },
        &this->lifetimeGuard_);
}

void SeventvPaints::pumpPaintRequests()
{
    constexpr std::size_t MAX_CONCURRENT_REQUESTS = 2;
    std::vector<PaintBatchRequest> ready;
    {
        std::unique_lock lock(this->mutex_);
        while (this->activePaintRequests_.size() < MAX_CONCURRENT_REQUESTS &&
               !this->pendingPaintBatches_.empty())
        {
            auto request = std::move(this->pendingPaintBatches_.front());
            this->pendingPaintBatches_.pop_front();

            std::erase_if(request.paints, [this](const auto &paint) {
                const auto state =
                    this->paintLoadStates_.find(paint.paintID);
                return state == this->paintLoadStates_.end() ||
                       state->second.status !=
                           SeventvPaintLoadStatus::Loading ||
                       state->second.generation != paint.generation ||
                       state->second.requestID != 0;
            });
            if (request.paints.empty())
            {
                continue;
            }

            request.requestID = ++this->nextPaintRequestID_;
            const auto now = std::chrono::steady_clock::now();
            for (const auto &paint : request.paints)
            {
                auto &state = this->paintLoadStates_.at(paint.paintID);
                state.requestID = request.requestID;
                state.startedAt = now;
            }
            this->activePaintRequests_.insert_or_assign(request.requestID,
                                                        request);
            ready.push_back(std::move(request));
        }
    }
    for (auto &request : ready)
    {
        this->requestPaintBatch(std::move(request));
    }
}

void SeventvPaints::requestPaintBatch(PaintBatchRequest request)
{
    QJsonArray list;
    for (const auto &paint : request.paints)
    {
        list.append(paint.paintID);
    }
    const QJsonObject payload{
        {QStringLiteral("query"), SEVENTV_PAINT_QUERY},
        {QStringLiteral("variables"),
         QJsonObject{{QStringLiteral("list"), list}}},
    };
    const auto sharedRequest =
        std::make_shared<PaintBatchRequest>(std::move(request));

    QTimer::singleShot(
        SEVENTV_PAINT_REQUEST_WATCHDOG, &this->lifetimeGuard_,
        [this, requestID = sharedRequest->requestID] {
            this->handlePaintRequestWatchdog(requestID);
        });

    NetworkRequest(SEVENTV_GQL_URL, NetworkRequestType::Post)
        .caller(&this->lifetimeGuard_)
        .timeout(SEVENTV_PAINT_REQUEST_TIMEOUT_MS)
        .maximumResponseSize(2 * 1024 * 1024)
        .json(payload)
        .onSuccess([this, sharedRequest](const NetworkResult &result) {
            const auto root = result.parseJson();
            if (!root.value(QStringLiteral("errors")).toArray().isEmpty())
            {
                qCWarning(chatterinoSeventv)
                    << "7TV rejected a paint batch lookup";
                QStringList failed;
                for (const auto &paint : sharedRequest->paints)
                {
                    if (this->finishPaintLoad(
                            paint, sharedRequest->requestID,
                            SeventvPaintLoadStatus::Failed))
                    {
                        failed.push_back(paint.paintID);
                    }
                }
                if (!sharedRequest->retry && !failed.isEmpty())
                {
                    this->schedulePaintRetry(failed);
                }
                this->finishPaintBatchRequest(sharedRequest->requestID);
                return;
            }

            const auto paints = root.value(QStringLiteral("data"))
                                    .toObject()
                                    .value(QStringLiteral("cosmetics"))
                                    .toObject()
                                    .value(QStringLiteral("paints"))
                                    .toArray();
            for (const auto &paint : paints)
            {
                if (paint.isObject())
                {
                    this->addPaint(paint.toObject());
                }
            }

            QStringList missing;
            bool paintsChanged = false;
            for (const auto &paint : sharedRequest->paints)
            {
                const bool ready =
                    this->getPaintByID(paint.paintID) != nullptr;
                paintsChanged = paintsChanged || ready;
                auto status = SeventvPaintLoadStatus::Failed;
                if (ready)
                {
                    status = SeventvPaintLoadStatus::Ready;
                }
                else if (sharedRequest->retry)
                {
                    status = SeventvPaintLoadStatus::NotFound;
                }
                const bool applied = this->finishPaintLoad(
                    paint, sharedRequest->requestID, status);
                if (applied && !ready && !sharedRequest->retry)
                {
                    missing.push_back(paint.paintID);
                }
            }
            if (!missing.isEmpty())
            {
                this->schedulePaintRetry(missing);
            }
            this->finishPaintBatchRequest(sharedRequest->requestID,
                                          paintsChanged);
        })
        .onError([this, sharedRequest](const NetworkResult &result) {
            qCWarning(chatterinoSeventv)
                << "Failed to load a 7TV paint batch" << result.formatError();
            QStringList failed;
            for (const auto &paint : sharedRequest->paints)
            {
                if (this->finishPaintLoad(paint, sharedRequest->requestID,
                                          SeventvPaintLoadStatus::Failed))
                {
                    failed.push_back(paint.paintID);
                }
            }
            if (!sharedRequest->retry && !failed.isEmpty())
            {
                this->schedulePaintRetry(failed);
            }
            this->finishPaintBatchRequest(sharedRequest->requestID);
        })
        .execute();
}

void SeventvPaints::finishPaintBatchRequest(std::uint64_t requestID,
                                            bool paintsChanged)
{
    bool wasActive = false;
    {
        std::unique_lock lock(this->mutex_);
        wasActive = this->activePaintRequests_.erase(requestID) != 0;
    }
    if (!wasActive && !paintsChanged)
    {
        return;
    }
    postToThread(
        [this, paintsChanged, wasActive] {
            if (paintsChanged)
            {
                getApp()->getWindows()->invalidateChannelViewBuffers();
            }
            if (wasActive)
            {
                this->pumpPaintRequests();
            }
        },
        &this->lifetimeGuard_);
}

void SeventvPaints::handlePaintRequestWatchdog(std::uint64_t requestID)
{
    PaintBatchRequest request;
    QStringList ready;
    QStringList failed;
    {
        std::unique_lock lock(this->mutex_);
        const auto active = this->activePaintRequests_.find(requestID);
        if (active == this->activePaintRequests_.end())
        {
            return;
        }
        request = std::move(active->second);
        this->activePaintRequests_.erase(active);
        for (const auto &paint : request.paints)
        {
            const auto state = this->paintLoadStates_.find(paint.paintID);
            if (state == this->paintLoadStates_.end() ||
                state->second.generation != paint.generation ||
                state->second.requestID != requestID)
            {
                continue;
            }
            if (this->knownPaints_.contains(paint.paintID))
            {
                state->second.status = SeventvPaintLoadStatus::Ready;
                ready.push_back(paint.paintID);
            }
            else
            {
                state->second.status = SeventvPaintLoadStatus::Failed;
                failed.push_back(paint.paintID);
            }
            state->second.requestID = 0;
            state->second.startedAt = {};
        }
    }

    for (const auto &paintID : ready)
    {
        postToThread(
            [this, paintID] {
                this->paintLoadStatusChanged.invoke(
                    paintID, SeventvPaintLoadStatus::Ready);
            },
            &this->lifetimeGuard_);
    }
    for (const auto &paintID : failed)
    {
        postToThread(
            [this, paintID] {
                this->paintLoadStatusChanged.invoke(
                    paintID, SeventvPaintLoadStatus::Failed);
            },
            &this->lifetimeGuard_);
    }
    if (!request.retry && !failed.isEmpty())
    {
        this->schedulePaintRetry(failed);
    }
    postToThread(
        [this] {
            this->pumpPaintRequests();
        },
        &this->lifetimeGuard_);
}

void SeventvPaints::schedulePaintRetry(const QStringList &paintIDs)
{
    if (paintIDs.isEmpty())
    {
        return;
    }
    QTimer::singleShot(900, &this->lifetimeGuard_, [this, paintIDs] {
        this->loadPaintsByID(paintIDs, true);
    });
}

bool SeventvPaints::finishPaintLoad(const PaintBatchItem &paint,
                                    std::uint64_t requestID,
                                    SeventvPaintLoadStatus status)
{
    bool applied = false;
    {
        std::unique_lock lock(this->mutex_);
        auto &state = this->paintLoadStates_[paint.paintID];
        if (this->knownPaints_.contains(paint.paintID))
        {
            status = SeventvPaintLoadStatus::Ready;
            state.status = status;
            state.requestID = 0;
            state.startedAt = {};
            applied = true;
        }
        else if (state.generation == paint.generation &&
                 state.requestID == requestID)
        {
            state.status = status;
            state.requestID = 0;
            state.startedAt = {};
            applied = true;
        }
    }

    if (applied)
    {
        postToThread(
            [this, paintID = paint.paintID, status] {
                this->paintLoadStatusChanged.invoke(paintID, status);
            },
            &this->lifetimeGuard_);
    }
    return applied;
}

void SeventvPaints::assignPaintToUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users)
{
    const auto normalizedPaintID = normalizePaintID(paintID);
    std::vector<std::pair<QString, bool>> changedUsers;
    int64_t nAdded = 0;
    {
        std::unique_lock lock(this->mutex_);

        const auto paintIt = this->knownPaints_.find(normalizedPaintID);
        if (paintIt == this->knownPaints_.end())
        {
            return;
        }

        auto addToMap = [&](auto &map, const QString &username, bool isKick) {
            const auto normalizedUsername = username.toLower();
            auto it = map.find(normalizedUsername);
            if (it == map.end())
            {
                map.emplace(normalizedUsername, paintIt->second);
                changedUsers.emplace_back(normalizedUsername, isKick);
                nAdded++;
            }
            else if (it->second != paintIt->second)
            {
                it->second = paintIt->second;
                changedUsers.emplace_back(normalizedUsername, isKick);
            }
        };
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                           [&](const seventv::eventapi::TwitchUser &u) {
                               addToMap(this->twitchPaintMap_, u.userName,
                                        false);
                           },
                           [&](const seventv::eventapi::KickUser &u) {
                               addToMap(this->kickPaintMap_, u.userName, true);
                           },
                       },
                       user);
        }
    }

    if (nAdded > 0)
    {
        DebugCount::increase(DebugObject::SeventvPaintAssignments, nAdded);
    }

    if (!changedUsers.empty())
    {
        postToThread(
            [this, changedUsers = std::move(changedUsers)] {
                for (const auto &[username, isKick] : changedUsers)
                {
                    this->paintChanged.invoke(username, isKick);
                }
                getApp()->getWindows()->invalidateChannelViewBuffers();
            },
            &this->lifetimeGuard_);
    }
}

void SeventvPaints::clearPaintFromUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users)
{
    const auto normalizedPaintID = normalizePaintID(paintID);
    std::vector<std::pair<QString, bool>> changedUsers;
    int64_t nRemoved = 0;
    {
        std::unique_lock lock(this->mutex_);

        auto removeFromMap = [&](auto &map, const QString &username,
                                 bool isKick) {
            const auto normalizedUsername = username.toLower();
            const auto it = map.find(normalizedUsername);
            if (it != map.end() &&
                normalizePaintID(it->second->id) == normalizedPaintID)
            {
                map.erase(it);
                changedUsers.emplace_back(normalizedUsername, isKick);
                nRemoved++;
            }
        };
        for (const auto &user : users)
        {
            std::visit(
                variant::Overloaded{
                    [&](const seventv::eventapi::TwitchUser &u) {
                        removeFromMap(this->twitchPaintMap_, u.userName, false);
                    },
                    [&](const seventv::eventapi::KickUser &u) {
                        removeFromMap(this->kickPaintMap_, u.userName, true);
                    },
                },
                user);
        }
    }

    if (nRemoved > 0)
    {
        DebugCount::decrease(DebugObject::SeventvPaintAssignments, nRemoved);
        postToThread(
            [this, changedUsers = std::move(changedUsers)] {
                for (const auto &[username, isKick] : changedUsers)
                {
                    this->paintChanged.invoke(username, isKick);
                }
                getApp()->getWindows()->invalidateChannelViewBuffers();
            },
            &this->lifetimeGuard_);
    }
}

}
