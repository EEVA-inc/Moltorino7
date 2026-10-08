#include "providers/moltorino/MoltorinoSupporterBadges.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSize>
#include <QTimer>
#include <QUrlQuery>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace chatterino {

namespace {

using namespace literals;

constexpr auto ENDPOINT = "https://api.moltorino.com/v2/badges";
constexpr auto LEGACY_ENDPOINT = "https://api.moltorino.com/badges";
constexpr auto CACHE_FILE = "moltorino-supporter-badges.json";
constexpr int PASSIVE_REFRESH_THROTTLE_MS = 60000;
constexpr qsizetype MAX_PAYLOAD_BYTES = 8 * 1024 * 1024;
constexpr int MAX_CATEGORIES = 64;
constexpr int MAX_ASSIGNMENTS = 250000;
constexpr int MAX_VANITY_LAYOUTS = 250000;
constexpr int MAX_VANITY_KEYS = 64;
constexpr QSize BADGE_BASE_SIZE(18, 18);

struct ParsedPayload {
    bool isV2 = false;
    QString generation;
    int version = -1;
    std::unordered_map<QString, std::vector<MoltorinoSupporterBadge>>
        userBadges;
    std::unordered_map<QString, MoltorinoSupporterBadge> categoryBadges;
    std::vector<QString> categoryOrder;
    std::unordered_set<QString> decorationsDisabledUsers;
    std::unordered_map<QString, MoltorinoVanityLayout> vanityLayouts;
    size_t categoryCount = 0;
    size_t assignmentCount = 0;
};

bool isValidVanitySlotKey(const QString &value)
{
    static const QSet<QString> slots{
        QStringLiteral("ta"), QStringLiteral("ts"), QStringLiteral("tv"),
        QStringLiteral("tp"), QStringLiteral("c"),  QStringLiteral("ff"),
        QStringLiteral("fa"), QStringLiteral("bt"), QStringLiteral("m"),
        QStringLiteral("7"),  QStringLiteral("hs"), QStringLiteral("hc"),
        QStringLiteral("bl"), QStringLiteral("jc"),
    };
    return slots.contains(value);
}

bool isValidVanityKey(const QString &value)
{
    if (isValidVanitySlotKey(value))
    {
        return true;
    }
    static const QRegularExpression expression(
        QStringLiteral("^t:[a-z0-9_-]{1,48}$"));
    return expression.match(value).hasMatch();
}

std::vector<QString> parseVanityKeys(const QJsonValue &value)
{
    std::vector<QString> keys;
    if (!value.isArray())
    {
        return keys;
    }

    std::unordered_set<QString> seen;
    for (const auto &item : value.toArray())
    {
        if (keys.size() >= MAX_VANITY_KEYS)
        {
            break;
        }
        const auto key = item.toString().trimmed().toLower();
        if (!isValidVanityKey(key) || !seen.insert(key).second)
        {
            continue;
        }
        keys.push_back(key);
    }
    return keys;
}

bool isValidUserId(const QString &value)
{
    if (value.isEmpty() || value.size() > 32)
    {
        return false;
    }

    return std::ranges::all_of(value, [](const QChar ch) {
        return ch.isDigit();
    });
}

QString versionedImageUrl(const QString &url, int version)
{
    if (url.isEmpty() || version < 0 || url.startsWith(u":/"_s))
    {
        return url;
    }

    const auto fragmentIndex = url.indexOf(u'#');
    auto base = fragmentIndex < 0 ? url : url.left(fragmentIndex);
    const auto fragment = fragmentIndex < 0 ? QString{} : url.mid(fragmentIndex);

    base += base.contains(u'?') ? u'&' : u'?';
    base += u"mbv="_s + QString::number(version);

    return base + fragment;
}

ImagePtr badgeImage(const QString &url, int version, qreal scale,
                    QSize expectedSize)
{
    if (url.isEmpty())
    {
        return Image::getEmpty();
    }

    return Image::fromUrl(Url{versionedImageUrl(url, version)}, scale,
                          expectedSize);
}

ImageSet badgeImageSet(const QString &image1, const QString &image2,
                       const QString &image3, const QString &image4, int version)
{
    const auto badge1x = badgeImage(image1, version, 1.0, BADGE_BASE_SIZE);
    const auto badge2x = badgeImage(image2, version, 0.5, BADGE_BASE_SIZE * 2);
    const auto badge3x = badgeImage(image3, version, 0.25, BADGE_BASE_SIZE * 4);
    const auto badge4x = badgeImage(image4, version, 0.125, BADGE_BASE_SIZE * 8);

    for (const auto &image : {badge1x, badge2x, badge3x, badge4x})
    {
        if (!image->isEmpty())
        {
            image->setFrameCacheLifetime(std::chrono::minutes(4));
        }
    }
    return ImageSet{badge1x, badge2x, badge3x, badge4x};
}

EmotePtr makeBadgeEmote(const QJsonObject &category, int version)
{
    const auto id = category.value("id").toString().trimmed().toLower();
    if (id.isEmpty() || id.size() > 128)
    {
        return nullptr;
    }

    const auto images = category.value("images").toObject();
    auto image1 = images.value("1x").toString().trimmed();
    auto image2 = images.value("2x").toString().trimmed();
    auto image3 = images.value("3x").toString().trimmed();
    auto image4 = images.value("4x").toString().trimmed();
    if (image1.isEmpty())
    {
        image1 = category.value("image1").toString().trimmed();
    }
    if (image2.isEmpty())
    {
        image2 = category.value("image2").toString().trimmed();
    }
    if (image3.isEmpty())
    {
        image3 = category.value("image3").toString().trimmed();
    }

    if (image4.isEmpty())
    {
        image4 = category.value("image4").toString().trimmed();
    }

    auto imageSet = badgeImageSet(image1, image2, image3, image4, version);
    if (imageSet.getImage1()->isEmpty())
    {
        return nullptr;
    }

    auto tooltip = category.value("tooltip").toString().trimmed();
    if (tooltip.isEmpty())
    {
        tooltip = id;
    }

    auto emote = Emote{
        .name = EmoteName{u"moltorino:"_s + id},
        .images = std::move(imageSet),
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

void addBadgeAssignment(ParsedPayload &parsed, const QString &userId,
                        const MoltorinoSupporterBadge &badge)
{
    auto &badges = parsed.userBadges[userId];
    if (std::ranges::any_of(badges, [&](const auto &assigned) {
            return assigned.categoryId == badge.categoryId;
        }))
    {
        return;
    }

    badges.push_back(badge);
}

void addUserPolicy(ParsedPayload &parsed, const QString &userId,
                   const QJsonObject &user)
{
    if (!isValidUserId(userId))
    {
        return;
    }

    const auto decorations = user.value("decorations");
    if (decorations.isBool() && !decorations.toBool())
    {
        parsed.decorationsDisabledUsers.insert(userId);
    }
}

bool parsePayload(const QByteArray &payload,
                  ParsedPayload &parsed)
{
    if (payload.size() > MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    const auto root = document.object();
    parsed.isV2 = root.value("schemaVersion").toInt() == 2;
    if (parsed.isV2)
    {
        parsed.generation = root.value("generation").toString().trimmed();
        if (parsed.generation.isEmpty() || parsed.generation.size() > 128)
        {
            return false;
        }
    }
    bool ok = false;
    const auto version = root.value(parsed.isV2 ? "bundleVersion" : "version")
                             .toVariant()
                             .toInt(&ok);
    if (!ok || version < 0)
    {
        return false;
    }

    parsed.version = version;

    auto categories = root.value("badges").toArray();
    if (categories.isEmpty() && root.contains("categories"))
    {
        categories = root.value("categories").toArray();
    }

    parsed.userBadges.reserve(std::min<int>(categories.size() * 64, 4096));

    for (const auto &categoryValue : categories)
    {
        if (parsed.categoryCount >= MAX_CATEGORIES ||
            parsed.assignmentCount >= MAX_ASSIGNMENTS)
        {
            break;
        }

        const auto category = categoryValue.toObject();
        const auto categoryId =
            category.value("id").toString().trimmed().toLower();
        if (categoryId.isEmpty() || categoryId.size() > 128)
        {
            continue;
        }

        auto emote = makeBadgeEmote(category, parsed.isV2 ? -1 : version);
        if (!emote)
        {
            continue;
        }

        auto displayName = category.value("name").toString().trimmed();
        if (displayName.isEmpty())
        {
            displayName = category.value("tooltip").toString().trimmed();
        }
        if (displayName.isEmpty())
        {
            displayName = categoryId;
        }
        auto description = category.value("description").toString().trimmed();
        const auto listedValue = category.value("listed");
        const auto listedInVanity =
            vanity::detail::resolveMoltorinoBadgeVanityListing(
                categoryId, listedValue.isBool()
                                ? std::optional<bool>{listedValue.toBool()}
                                : std::nullopt);
        displayName = displayName.left(80);
        description = description.left(320);

        ++parsed.categoryCount;
        if (!parsed.categoryBadges.contains(categoryId))
        {
            parsed.categoryOrder.push_back(categoryId);
        }
        parsed.categoryBadges.insert_or_assign(
            categoryId,
            MoltorinoSupporterBadge{categoryId, std::move(displayName),
                                    std::move(description), emote,
                                    listedInVanity});

        const auto usersValue = category.value("users");
        if (usersValue.isArray())
        {
            const auto users = usersValue.toArray();
            for (const auto &userValue : users)
            {
                if (parsed.assignmentCount >= MAX_ASSIGNMENTS)
                {
                    break;
                }

                const auto user = userValue.toObject();
                auto userId = user.value("id").toString().trimmed();
                if (userId.isEmpty())
                {
                    userId = user.value("userId").toString().trimmed();
                }
                if (!isValidUserId(userId))
                {
                    continue;
                }

                addUserPolicy(parsed, userId, user);
                ++parsed.assignmentCount;

                addBadgeAssignment(parsed, userId,
                                   parsed.categoryBadges.at(categoryId));
            }
            continue;
        }

        const auto users = usersValue.toObject();
        for (auto it = users.constBegin(); it != users.constEnd(); ++it)
        {
            if (parsed.assignmentCount >= MAX_ASSIGNMENTS)
            {
                break;
            }

            const auto userId = it.key().trimmed();
            if (!isValidUserId(userId))
            {
                continue;
            }

            addUserPolicy(parsed, userId, it.value().toObject());
            ++parsed.assignmentCount;

            addBadgeAssignment(parsed, userId,
                               parsed.categoryBadges.at(categoryId));
        }
    }

    if (parsed.isV2)
    {
        auto defaultOrder = parseVanityKeys(
            root.value("layout").toObject().value("defaultOrder"));
        const auto users = root.value("users").toObject();
        for (auto it = users.constBegin(); it != users.constEnd(); ++it)
        {
            if (parsed.vanityLayouts.size() >= MAX_VANITY_LAYOUTS ||
                parsed.assignmentCount >= MAX_ASSIGNMENTS)
            {
                break;
            }
            const auto userId = it.key().trimmed();
            const auto user = it.value().toObject();
            if (!isValidUserId(userId) || user.isEmpty())
            {
                continue;
            }

            addUserPolicy(parsed, userId, user);
            for (const auto &badgeValue : user.value("badges").toArray())
            {
                if (parsed.assignmentCount >= MAX_ASSIGNMENTS)
                {
                    break;
                }
                const auto badgeId = badgeValue.toString().trimmed().toLower();
                const auto category = parsed.categoryBadges.find(badgeId);
                if (category == parsed.categoryBadges.end())
                {
                    continue;
                }
                addBadgeAssignment(parsed, userId, category->second);
                ++parsed.assignmentCount;
            }

            const bool hasLayout = user.contains("order") ||
                                   user.contains("hidden") ||
                                   user.contains("activeBadge");
            if (hasLayout)
            {
                MoltorinoVanityLayout layout;
                layout.order = user.contains("order")
                                   ? parseVanityKeys(user.value("order"))
                                   : defaultOrder;
                for (auto key : parseVanityKeys(user.value("hidden")))
                {
                    layout.hidden.insert(std::move(key));
                }
                layout.moltorinoBadge =
                    user.value("activeBadge").toString().trimmed().toLower();
                layout.moltorinoBadgeSelectionExplicit =
                    user.contains("activeBadge");
                parsed.vanityLayouts.insert_or_assign(
                    userId, vanity::detail::normalizeLayout(std::move(layout)));
            }
        }
        return true;
    }

    for (const auto &userValue : root.value("users").toArray())
    {
        const auto user = userValue.toObject();
        auto userId = user.value("id").toString().trimmed();
        if (userId.isEmpty())
        {
            userId = user.value("userId").toString().trimmed();
        }

        addUserPolicy(parsed, userId, user);
    }

    for (const auto &layoutValue : root.value("layouts").toArray())
    {
        if (parsed.vanityLayouts.size() >= MAX_VANITY_LAYOUTS)
        {
            break;
        }
        const auto layoutObject = layoutValue.toObject();
        const auto userId = layoutObject.value("u").toString().trimmed();
        if (!isValidUserId(userId))
        {
            continue;
        }

        MoltorinoVanityLayout layout;
        layout.order = parseVanityKeys(layoutObject.value("o"));
        for (auto key : parseVanityKeys(layoutObject.value("h")))
        {
            layout.hidden.insert(std::move(key));
        }
        layout.moltorinoBadge =
            layoutObject.value("m").toString().trimmed().toLower();
        layout.moltorinoBadgeSelectionExplicit =
            layoutObject.contains("x")
                ? layoutObject.value("x").toBool(false)
                : !layout.moltorinoBadge.isEmpty();
        parsed.vanityLayouts.insert_or_assign(
            userId, vanity::detail::normalizeLayout(std::move(layout)));
    }

    return true;
}

QString cachePath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromUtf8(CACHE_FILE));
}

}  // namespace

namespace vanity::detail {

QString badgeOrderSlot(const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    if (normalized.startsWith(QStringLiteral("t:")))
    {
        return TwitchBadge::vanitySlotKeyForSet(normalized.mid(2));
    }
    return normalized;
}

MoltorinoVanityLayout normalizeLayout(MoltorinoVanityLayout layout)
{
    const auto isTwitchSlot = [](const QString &key) {
        return key == QStringLiteral("ta") || key == QStringLiteral("ts") ||
               key == QStringLiteral("tv") || key == QStringLiteral("tp");
    };
    const bool hasLegacyTwitchKeys =
        std::ranges::any_of(layout.order, [](const auto &key) {
            return key.trimmed().startsWith(QStringLiteral("t:"),
                                            Qt::CaseInsensitive);
        });

    std::vector<QString> order;
    order.reserve(layout.order.size());
    std::unordered_set<QString> present;
    const auto append = [&](const QString &rawKey) {
        const auto key = badgeOrderSlot(rawKey);
        if (isValidVanitySlotKey(key) && present.insert(key).second)
        {
            order.push_back(key);
        }
    };

    if (hasLegacyTwitchKeys)
    {
        std::unordered_set<QString> twitchSlots;
        for (const auto &rawKey : layout.order)
        {
            const auto slot = badgeOrderSlot(rawKey);
            if (isTwitchSlot(slot))
            {
                twitchSlots.insert(slot);
            }
        }
        for (const auto &slot : {QStringLiteral("ta"), QStringLiteral("ts"),
                                 QStringLiteral("tv"), QStringLiteral("tp")})
        {
            if (twitchSlots.contains(slot))
            {
                append(slot);
            }
        }
        for (const auto &rawKey : layout.order)
        {
            if (!isTwitchSlot(badgeOrderSlot(rawKey)))
            {
                append(rawKey);
            }
        }
    }
    else
    {
        for (const auto &rawKey : layout.order)
        {
            append(rawKey);
        }
    }
    for (const auto &key :
         {QStringLiteral("ta"), QStringLiteral("ts"), QStringLiteral("tv"),
          QStringLiteral("tp"), QStringLiteral("c"), QStringLiteral("ff"),
          QStringLiteral("bt"), QStringLiteral("m"), QStringLiteral("7"),
          QStringLiteral("hc"), QStringLiteral("hs")})
    {
        append(key);
    }
    if (present.insert(QStringLiteral("fa")).second)
    {
        const auto ffz = std::ranges::find(order, QStringLiteral("ff"));
        order.insert(ffz == order.end() ? order.end() : std::next(ffz),
                     QStringLiteral("fa"));
    }
    if (present.insert(QStringLiteral("bl")).second)
    {
        order.insert(std::ranges::find(order, QStringLiteral("m")),
                     QStringLiteral("bl"));
    }
    if (present.insert(QStringLiteral("jc")).second)
    {
        order.push_back(QStringLiteral("jc"));
    }
    layout.order = std::move(order);

    std::unordered_set<QString> hidden;
    for (const auto &rawKey : layout.hidden)
    {
        const auto key = rawKey.trimmed().toLower();
        if (isValidVanityKey(key))
        {
            hidden.insert(key.startsWith(QStringLiteral("t:"))
                              ? badgeOrderSlot(key)
                              : key);
        }
    }
    layout.hidden = std::move(hidden);
    layout.moltorinoBadge = layout.moltorinoBadge.trimmed().toLower();
    return layout;
}

MoltorinoVanityLayout defaultLayoutPreservingBadge(
    const MoltorinoVanityLayout &layout)
{
    auto reset = normalizeLayout({});
    reset.moltorinoBadge = layout.moltorinoBadge.trimmed().toLower();
    reset.moltorinoBadgeSelectionExplicit =
        layout.moltorinoBadgeSelectionExplicit;
    return reset;
}

bool isBadgeVisible(const MoltorinoVanityLayout &layout, const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    const auto slot = badgeOrderSlot(normalized);
    return !layout.hidden.contains(normalized) &&
           (slot == normalized || !layout.hidden.contains(slot));
}

void setBadgeVisible(MoltorinoVanityLayout &layout, const QString &key,
                     bool visible)
{
    const auto normalized = key.trimmed().toLower();
    if (!isValidVanityKey(normalized))
    {
        return;
    }
    if (visible)
    {
        layout.hidden.erase(normalized);
        layout.hidden.erase(badgeOrderSlot(normalized));
    }
    else
    {
        layout.hidden.erase(normalized);
        layout.hidden.insert(badgeOrderSlot(normalized));
    }
}

void appendMissingBadgeKeys(MoltorinoVanityLayout &layout,
                            const std::vector<QString> &keys)
{
    std::unordered_set<QString> present(layout.order.begin(),
                                        layout.order.end());
    const auto twitchRank = [](const QString &key) {
        if (key == QStringLiteral("ta"))
        {
            return 0;
        }
        if (key == QStringLiteral("ts"))
        {
            return 1;
        }
        if (key == QStringLiteral("tv"))
        {
            return 2;
        }
        if (key == QStringLiteral("tp"))
        {
            return 3;
        }
        return -1;
    };
    for (const auto &rawKey : keys)
    {
        const auto key = badgeOrderSlot(rawKey);
        if (isValidVanitySlotKey(key) && present.insert(key).second)
        {
            const auto rank = twitchRank(key);
            if (rank < 0)
            {
                layout.order.push_back(key);
                continue;
            }

            auto insertion = layout.order.begin();
            for (auto it = layout.order.begin(); it != layout.order.end(); ++it)
            {
                const auto existingRank = twitchRank(*it);
                if (existingRank < 0)
                {
                    continue;
                }
                if (existingRank > rank)
                {
                    insertion = it;
                    break;
                }
                insertion = std::next(it);
            }
            layout.order.insert(insertion, key);
        }
    }
}

void replaceActiveBadgeOrder(MoltorinoVanityLayout &layout,
                             const std::vector<QString> &keys)
{
    std::vector<QString> activeOrder;
    activeOrder.reserve(keys.size());
    std::unordered_set<QString> activeSlots;
    for (const auto &rawKey : keys)
    {
        const auto key = badgeOrderSlot(rawKey);
        if (isValidVanitySlotKey(key) && activeSlots.insert(key).second)
        {
            activeOrder.push_back(key);
        }
    }
    appendMissingBadgeKeys(layout, activeOrder);

    std::vector<QString> order;
    order.reserve(layout.order.size() + activeOrder.size());
    std::unordered_set<QString> present;
    size_t activeIndex = 0;
    for (const auto &rawKey : layout.order)
    {
        const auto key = badgeOrderSlot(rawKey);
        if (!isValidVanitySlotKey(key) || !present.insert(key).second)
        {
            continue;
        }
        if (activeSlots.contains(key))
        {
            order.push_back(activeOrder.at(activeIndex++));
        }
        else
        {
            order.push_back(key);
        }
    }
    layout.order = std::move(order);
}

bool shouldSaveProfile(const MoltorinoVanityLayout &original,
                       const MoltorinoVanityLayout &current,
                       bool forceOnlineSync)
{
    return forceOnlineSync ||
           normalizeLayout(original) != normalizeLayout(current);
}

MoltorinoVanityLayout withoutDeviceOnlyKeys(MoltorinoVanityLayout layout)
{
    std::erase(layout.order, QStringLiteral("jc"));
    layout.hidden.erase(QStringLiteral("jc"));
    return layout;
}

bool shouldClearLocalLayout(const MoltorinoVanityLayout &local,
                            const std::optional<MoltorinoVanityLayout> &remote)
{
    if (remote && *remote == local)
    {
        return true;
    }
    // The server may not store the JilChat slot. A layout that moves or hides
    // it then only exists on this device and has to stay.
    if (local.hidden.contains(QStringLiteral("jc")) ||
        (!local.order.empty() && local.order.back() != QStringLiteral("jc")))
    {
        return false;
    }
    if (remote)
    {
        return *remote == local;
    }
    return local == normalizeLayout({});
}

bool resolveMoltorinoBadgeVanityListing(
    const QString &categoryId, std::optional<bool> serverListing)
{
    if (serverListing.has_value())
    {
        return *serverListing;
    }

    return categoryId.compare(QStringLiteral("leadmod"),
                              Qt::CaseInsensitive) != 0;
}

bool shouldShowMoltorinoBadge(bool listedInVanity, bool owned)
{
    return listedInVanity || owned;
}

}

MoltorinoSupporterBadges::MoltorinoSupporterBadges(QObject *parent)
    : QObject(parent)
{
}

void MoltorinoSupporterBadges::initialize()
{
    assertInGuiThread();
    if (this->initialized_ || isAppAboutToQuit())
    {
        return;
    }

    this->initialized_ = true;
    this->loadLocalVanityLayouts();
    this->loadCache();
    this->refreshNow();

    auto *timer = new QTimer(this);
    timer->setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(timer, &QTimer::timeout, this,
                     &MoltorinoSupporterBadges::refreshPassive);
    QObject::connect(QCoreApplication::instance(),
                     &QCoreApplication::aboutToQuit, timer, &QTimer::stop);
    timer->start(std::chrono::minutes(5));
}

void MoltorinoSupporterBadges::refreshNow()
{
    if (!isGuiThread())
    {
        postToThread(
            [this] {
                this->refreshNow();
            },
            this);
        return;
    }

    this->refreshInternal(true, std::nullopt);
}

void MoltorinoSupporterBadges::refreshPassive()
{
    if (!isGuiThread())
    {
        postToThread(
            [this] {
                this->refreshPassive();
            },
            this);
        return;
    }

    this->refreshInternal(false, std::nullopt);
}

void MoltorinoSupporterBadges::refreshIfNewer(int version)
{
    if (!isGuiThread())
    {
        postToThread(
            [this, version] {
                this->refreshIfNewer(version);
            },
            this);
        return;
    }

    if (!this->usingV2_ && version <= this->version_)
    {
        return;
    }

    this->refreshInternal(!this->usingV2_, this->usingV2_
                                               ? std::nullopt
                                               : std::optional<int>{version});
}

void MoltorinoSupporterBadges::refreshV2IfNewer(const QString &generation,
                                                int version)
{
    if (!isGuiThread())
    {
        postToThread(
            [this, generation, version] {
                this->refreshV2IfNewer(generation, version);
            },
            this);
        return;
    }

    const auto normalizedGeneration = generation.trimmed();
    if (normalizedGeneration.isEmpty() || version < 0)
    {
        this->refreshNow();
        return;
    }
    if (this->usingV2_ && normalizedGeneration == this->generation_ &&
        version <= this->version_)
    {
        return;
    }
    this->refreshInternal(true, version, normalizedGeneration);
}

std::vector<MoltorinoSupporterBadge> MoltorinoSupporterBadges::getBadges(
    const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->userBadges_.find(userId);
    if (it == this->userBadges_.end())
    {
        return {};
    }

    auto badges = it->second;
    QString preferred;
    bool selectionExplicit = false;
    if (const auto localLayout = this->localVanityLayouts_.find(userId);
        localLayout != this->localVanityLayouts_.end())
    {
        preferred = localLayout->second.moltorinoBadge;
        selectionExplicit =
            localLayout->second.moltorinoBadgeSelectionExplicit;
    }
    else if (const auto remoteLayout = this->vanityLayouts_.find(userId);
             remoteLayout != this->vanityLayouts_.end())
    {
        preferred = remoteLayout->second.moltorinoBadge;
        selectionExplicit =
            remoteLayout->second.moltorinoBadgeSelectionExplicit;
    }
    if (selectionExplicit && preferred.isEmpty())
    {
        return {};
    }
    if (!preferred.isEmpty())
    {
        const auto selected = std::ranges::find(
            badges, preferred, &MoltorinoSupporterBadge::categoryId);
        if (selected != badges.end() && selected != badges.begin())
        {
            std::rotate(badges.begin(), selected, std::next(selected));
        }
    }
    return badges;
}

std::vector<MoltorinoSupporterBadge>
    MoltorinoSupporterBadges::getAssignedBadges(const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->userBadges_.find(userId);
    return it == this->userBadges_.end()
               ? std::vector<MoltorinoSupporterBadge>{}
               : it->second;
}

std::vector<MoltorinoSupporterBadge>
    MoltorinoSupporterBadges::getBadgeCatalog() const
{
    std::shared_lock lock(this->mutex_);
    std::vector<MoltorinoSupporterBadge> badges;
    badges.reserve(this->categoryOrder_.size());
    for (const auto &categoryId : this->categoryOrder_)
    {
        const auto badge = this->categoryBadges_.find(categoryId);
        if (badge != this->categoryBadges_.end())
        {
            badges.push_back(badge->second);
        }
    }
    return badges;
}

std::vector<MoltorinoSupporterBadge>
    MoltorinoSupporterBadges::getBadgesByCategoryIds(
        const std::vector<QString> &categoryIds) const
{
    std::shared_lock lock(this->mutex_);
    std::vector<MoltorinoSupporterBadge> badges;
    badges.reserve(categoryIds.size());
    std::unordered_set<QString> seen;
    for (const auto &rawId : categoryIds)
    {
        const auto categoryId = rawId.trimmed().toLower();
        if (categoryId.isEmpty() || !seen.insert(categoryId).second)
        {
            continue;
        }
        const auto badge = this->categoryBadges_.find(categoryId);
        if (badge != this->categoryBadges_.end())
        {
            badges.push_back(badge->second);
        }
    }
    return badges;
}

std::optional<MoltorinoVanityLayout> MoltorinoSupporterBadges::getVanityLayout(
    const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto local = this->localVanityLayouts_.find(userId);
    if (local != this->localVanityLayouts_.end())
    {
        return local->second;
    }
    const auto it = this->vanityLayouts_.find(userId);
    if (it == this->vanityLayouts_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<MoltorinoVanityLayout>
    MoltorinoSupporterBadges::getLocalVanityLayout(const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->localVanityLayouts_.find(userId);
    if (it == this->localVanityLayouts_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void MoltorinoSupporterBadges::setLocalVanityLayout(
    const QString &userId, MoltorinoVanityLayout layout)
{
    if (!isValidUserId(userId))
    {
        return;
    }
    layout = vanity::detail::normalizeLayout(std::move(layout));
    {
        std::unique_lock lock(this->mutex_);
        const auto existing = this->localVanityLayouts_.find(userId);
        if (existing != this->localVanityLayouts_.end() &&
            existing->second == layout)
        {
            return;
        }
        this->localVanityLayouts_.insert_or_assign(
            userId, std::move(layout));
    }
    this->saveLocalVanityLayouts();
}

bool MoltorinoSupporterBadges::decorationsEnabledForUser(const QString &userId,
                                                         bool isSelf) const
{
    if (isSelf || !isValidUserId(userId))
    {
        return true;
    }

    std::shared_lock lock(this->mutex_);
    return !this->decorationsDisabledUsers_.contains(userId);
}

void MoltorinoSupporterBadges::refreshInternal(
    bool force, std::optional<int> minimumVersion, QString expectedGeneration,
    bool bypassCache)
{
    assertInGuiThread();

    if (isAppAboutToQuit())
    {
        return;
    }

    if (minimumVersion &&
        (expectedGeneration.isEmpty() ||
         expectedGeneration == this->generation_) &&
        *minimumVersion <= this->version_)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastFetchAttempt_.isValid() &&
        this->lastFetchAttempt_.msecsTo(now) < PASSIVE_REFRESH_THROTTLE_MS)
    {
        return;
    }

    if (this->requestInFlight_)
    {
        this->pendingRefresh_ = true;
        this->pendingForce_ = this->pendingForce_ || force;
        this->pendingBypassCache_ = this->pendingBypassCache_ || bypassCache;
        if (!expectedGeneration.isEmpty() &&
            expectedGeneration != this->pendingGeneration_)
        {
            this->pendingGeneration_ = std::move(expectedGeneration);
            this->pendingMinimumVersion_ = minimumVersion;
        }
        else if (minimumVersion &&
                 (!this->pendingMinimumVersion_ ||
                  *minimumVersion > *this->pendingMinimumVersion_))
        {
            this->pendingMinimumVersion_ = minimumVersion;
            if (!expectedGeneration.isEmpty())
            {
                this->pendingGeneration_ = std::move(expectedGeneration);
            }
        }
        return;
    }

    this->requestInFlight_ = true;
    this->lastFetchAttempt_ = now;

    QUrl endpoint(QString::fromLatin1(ENDPOINT));
    QUrlQuery query;
    if (minimumVersion)
    {
        query.addQueryItem(QStringLiteral("version"),
                           QString::number(*minimumVersion));
        if (!expectedGeneration.isEmpty())
        {
            query.addQueryItem(QStringLiteral("generation"),
                               expectedGeneration);
        }
    }
    if (bypassCache)
    {
        query.addQueryItem(
            QStringLiteral("refresh"),
            QString::number(QDateTime::currentMSecsSinceEpoch()));
    }
    if (!query.isEmpty())
    {
        endpoint.setQuery(query);
    }

    NetworkRequest(endpoint)
        .header("Accept", "application/json")
        .timeout(5000)
        .maximumResponseSize(MAX_PAYLOAD_BYTES)
        .caller(this)
        .onSuccess([this, minimumVersion, expectedGeneration,
                    bypassCache](const NetworkResult &result) {
            if (minimumVersion &&
                (expectedGeneration.isEmpty() ||
                 expectedGeneration == this->generation_) &&
                *minimumVersion <= this->version_)
            {
                return;
            }

            bool expectationMismatch = false;
            if (this->applyPayload(result.getData(), false, minimumVersion,
                                   expectedGeneration, &expectationMismatch))
            {
                this->saveCache(result.getData());
            }
            else if (expectationMismatch && !bypassCache)
            {
                this->pendingRefresh_ = true;
                this->pendingForce_ = true;
                this->pendingBypassCache_ = true;
                this->pendingMinimumVersion_.reset();
                this->pendingGeneration_.clear();
            }
            else if (!this->usingV2_)
            {
                this->requestLegacyFallback();
            }
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoApp) << "[Moltorino] Failed to load badge V2:"
                                     << result.formatError();
            if (!this->usingV2_)
            {
                this->requestLegacyFallback();
            }
        })
        .finally([this] {
            if (!this->legacyFallbackInFlight_)
            {
                this->finishRequest();
            }
        })
        .execute();
}

void MoltorinoSupporterBadges::requestLegacyFallback()
{
    assertInGuiThread();
    if (isAppAboutToQuit() || this->usingV2_ || this->legacyFallbackInFlight_)
    {
        return;
    }
    this->legacyFallbackInFlight_ = true;
    NetworkRequest(LEGACY_ENDPOINT)
        .header("Accept", "application/json")
        .timeout(5000)
        .maximumResponseSize(MAX_PAYLOAD_BYTES)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            if (this->applyPayload(result.getData(), false))
            {
                this->saveCache(result.getData());
            }
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Moltorino] Badge V1 fallback also failed:"
                << result.formatError();
        })
        .finally([this] {
            this->legacyFallbackInFlight_ = false;
            this->finishRequest();
        })
        .execute();
}

void MoltorinoSupporterBadges::finishRequest()
{
    assertInGuiThread();

    this->requestInFlight_ = false;
    if (!this->pendingRefresh_)
    {
        return;
    }

    const auto force = this->pendingForce_;
    const auto minimumVersion = this->pendingMinimumVersion_;
    const auto bypassCache = this->pendingBypassCache_;
    auto generation = std::move(this->pendingGeneration_);
    this->pendingRefresh_ = false;
    this->pendingForce_ = false;
    this->pendingBypassCache_ = false;
    this->pendingMinimumVersion_.reset();
    this->pendingGeneration_.clear();

    QTimer::singleShot(0, this,
                       [this, force, minimumVersion, bypassCache,
                        generation = std::move(generation)]() mutable {
                           this->refreshInternal(force, minimumVersion,
                                                 std::move(generation),
                                                 bypassCache);
                       });
}

void MoltorinoSupporterBadges::loadCache()
{
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }

    this->applyPayload(file.read(MAX_PAYLOAD_BYTES + 1), true);
}

void MoltorinoSupporterBadges::loadLocalVanityLayouts()
{
    const auto payload = getSettings()->localVanityLayouts.getValue().toUtf8();
    if (payload.isEmpty())
    {
        return;
    }
    QJsonParseError error;
    const auto root = QJsonDocument::fromJson(payload, &error).object();
    if (error.error != QJsonParseError::NoError || root.isEmpty())
    {
        return;
    }

    std::unordered_map<QString, MoltorinoVanityLayout> layouts;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        const auto userId = it.key().trimmed();
        const auto object = it.value().toObject();
        if (!isValidUserId(userId) || object.isEmpty())
        {
            continue;
        }
        MoltorinoVanityLayout layout;
        layout.order = parseVanityKeys(object.value("o"));
        for (auto key : parseVanityKeys(object.value("h")))
        {
            layout.hidden.insert(std::move(key));
        }
        layout.moltorinoBadge =
            object.value("m").toString().trimmed().toLower();
        layout.moltorinoBadgeSelectionExplicit =
            object.contains("x") ? object.value("x").toBool(false)
                                 : !layout.moltorinoBadge.isEmpty();
        layouts.insert_or_assign(
            userId, vanity::detail::normalizeLayout(std::move(layout)));
    }

    std::unique_lock lock(this->mutex_);
    this->localVanityLayouts_ = std::move(layouts);
}

void MoltorinoSupporterBadges::saveLocalVanityLayouts() const
{
    QJsonObject root;
    {
        std::shared_lock lock(this->mutex_);
        for (const auto &[userId, local] : this->localVanityLayouts_)
        {
            QJsonArray order;
            for (const auto &key : local.order)
            {
                order.append(key);
            }
            QStringList hiddenKeys;
            for (const auto &key : local.hidden)
            {
                hiddenKeys.push_back(key);
            }
            hiddenKeys.sort(Qt::CaseInsensitive);
            QJsonArray hidden;
            for (const auto &key : hiddenKeys)
            {
                hidden.append(key);
            }
            root.insert(
                userId,
                QJsonObject{{"o", order},
                            {"h", hidden},
                            {"m", local.moltorinoBadge},
                            {"x", local.moltorinoBadgeSelectionExplicit}});
        }
    }
    getSettings()->localVanityLayouts =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MoltorinoSupporterBadges::saveCache(const QByteArray &payload) const
{
    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }

    file.write(payload);
    file.commit();
}

bool MoltorinoSupporterBadges::applyPayload(const QByteArray &payload,
                                            bool fromCache,
                                            std::optional<int> minimumVersion,
                                            const QString &expectedGeneration,
                                            bool *expectationMismatch)
{
    if (expectationMismatch != nullptr)
    {
        *expectationMismatch = false;
    }
    ParsedPayload parsed;
    if (!parsePayload(payload, parsed))
    {
        if (!fromCache)
        {
            qCWarning(chatterinoApp)
                << "[Moltorino] Ignoring malformed supporter badge payload.";
        }
        return false;
    }

    if (!expectedGeneration.isEmpty() &&
        (!parsed.isV2 || parsed.generation != expectedGeneration))
    {
        if (expectationMismatch != nullptr)
        {
            *expectationMismatch = true;
        }
        if (!fromCache)
        {
            qCWarning(chatterinoApp)
                << "[Moltorino] Badge generation changed while waiting for"
                << expectedGeneration << "and returned" << parsed.generation;
        }
        return false;
    }

    if (minimumVersion && parsed.version < *minimumVersion)
    {
        if (expectationMismatch != nullptr)
        {
            *expectationMismatch = true;
        }
        if (!fromCache)
        {
            qCWarning(chatterinoApp)
                << "[Moltorino] Ignoring stale badge payload version"
                << parsed.version << "while waiting for" << *minimumVersion;
        }
        return false;
    }

    if (!parsed.isV2 && this->usingV2_)
    {
        return false;
    }
    if (parsed.isV2 && this->usingV2_ &&
        parsed.generation == this->generation_ &&
        parsed.version < this->version_)
    {
        return false;
    }
    if (!parsed.isV2 && parsed.version < this->version_)
    {
        return false;
    }

    const auto userCount = parsed.userBadges.size();
    const auto layoutCount = parsed.vanityLayouts.size();
    bool removedLocalLayout = false;
    {
        std::unique_lock lock(this->mutex_);
        for (auto it = this->localVanityLayouts_.begin();
             it != this->localVanityLayouts_.end();)
        {
            const auto remote = parsed.vanityLayouts.find(it->first);
            const auto remoteLayout =
                remote == parsed.vanityLayouts.end()
                    ? std::optional<MoltorinoVanityLayout>{}
                    : std::optional<MoltorinoVanityLayout>{remote->second};
            if (vanity::detail::shouldClearLocalLayout(it->second,
                                                       remoteLayout))
            {
                it = this->localVanityLayouts_.erase(it);
                removedLocalLayout = true;
            }
            else
            {
                ++it;
            }
        }
        this->version_ = parsed.version;
        this->generation_ = parsed.generation;
        this->usingV2_ = parsed.isV2;
        this->userBadges_ = std::move(parsed.userBadges);
        this->categoryBadges_ = std::move(parsed.categoryBadges);
        this->categoryOrder_ = std::move(parsed.categoryOrder);
        this->decorationsDisabledUsers_ =
            std::move(parsed.decorationsDisabledUsers);
        this->vanityLayouts_ = std::move(parsed.vanityLayouts);
    }
    if (removedLocalLayout)
    {
        this->saveLocalVanityLayouts();
    }

    qCDebug(chatterinoApp) << "[Moltorino] Loaded supporter badges:"
                           << userCount << "users across"
                           << parsed.categoryCount << "categories from"
                           << parsed.assignmentCount << "assignments in"
                           << (fromCache ? "cache" : "network") << "version"
                           << this->version_ << "with" << layoutCount
                           << "vanity layouts";

    this->badgesUpdated.invoke();

    return true;
}

}  // namespace chatterino
