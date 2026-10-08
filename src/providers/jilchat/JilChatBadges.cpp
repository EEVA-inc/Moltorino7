#include "providers/jilchat/JilChatBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Settings.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <mutex>

namespace chatterino {
namespace {

constexpr auto BADGES_URL = "https://api.jil.chat/v1/badges";
constexpr qsizetype MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
constexpr auto REFRESH_INTERVAL = 30 * 1000;

bool validImageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    const auto host = url.host().toLower();
    return value.size() <= 2048 && url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           (host == QStringLiteral("jil.chat") ||
            host.endsWith(QStringLiteral(".jil.chat"))) &&
           url.userInfo().isEmpty() && url.port(-1) == -1 && !url.hasFragment();
}

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

std::shared_ptr<const Emote> makeBadge(const QJsonObject &object)
{
    const auto id = object.value("id").toString();
    const auto slug = object.value("slug").toString();
    const auto name = object.value("name").toString();
    const auto imageUrl = object.value("image_url").toString();
    if (id.isEmpty() || id.size() > 64 || slug.size() > 64 || name.isEmpty() ||
        name.size() > 256 || !validImageUrl(imageUrl))
    {
        return nullptr;
    }

    // JilChat serves one high-resolution image per badge. Autoscaling keeps
    // all of its pixels and only sets the scale, so the badge is drawn at the
    // regular badge size and stays sharp on HiDPI and in the tooltip.
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("jilchat:") +
                          (slug.isEmpty() ? id : slug)},
        .images = ImageSet{Image::fromAutoscaledUrl(Url{imageUrl}, 18)},
        .tooltip = Tooltip{name.toHtmlEscaped()},
        .homePage = Url{},
        .id = EmoteId{id},
    });
}

}  // namespace

void JilChatBadges::initialize()
{
    this->refreshTimer_.setInterval(REFRESH_INTERVAL);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->requestBadges();
    });
    this->refreshTimer_.start();

    // Nothing is requested from JilChat while its badges are turned off.
    getSettings()->showBadgesJilChat.connect(
        [this](bool enabled) {
            if (enabled)
            {
                QTimer::singleShot(3500, this, [this] {
                    this->requestBadges();
                });
            }
        },
        this->signalHolder_);
}

std::vector<std::shared_ptr<const Emote>> JilChatBadges::getBadges(
    const UserId &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto found = this->users_.find(userID.string);
    if (found == this->users_.end())
    {
        return {};
    }
    return found->second;
}

bool JilChatBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isArray())
    {
        return false;
    }

    boost::unordered_flat_map<QString,
                              std::vector<std::shared_ptr<const Emote>>>
        users;
    size_t total = 0;
    for (const auto &value : document.array())
    {
        const auto object = value.toObject();
        const auto badge = makeBadge(object);
        if (!badge)
        {
            continue;
        }
        for (const auto &user : object.value("users").toArray())
        {
            const auto id = user.toObject().value("twitch_id").toString();
            if (!validUserId(id))
            {
                continue;
            }
            if (++total > 200000)
            {
                return false;
            }
            auto &badges = users[id];
            if (badges.size() < 4)
            {
                badges.push_back(badge);
            }
        }
    }

    std::unique_lock lock(this->mutex_);
    this->users_ = std::move(users);
    return true;
}

void JilChatBadges::requestBadges()
{
    if (this->requestInFlight_ || !getSettings()->showBadgesJilChat)
    {
        return;
    }
    this->requestInFlight_ = true;
    NetworkRequest(BADGES_URL)
        .header("Accept", "application/json")
        .maximumResponseSize(MAX_PAYLOAD_SIZE)
        .timeout(8000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto payload = result.getData();
            if (payload == this->lastPayload_)
            {
                return;
            }
            if (!this->applyPayload(payload))
            {
                qCWarning(chatterinoApp)
                    << "[JilChat] Ignoring an invalid badge list.";
                return;
            }
            this->lastPayload_ = payload;
            this->badgesUpdated.invoke();
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[JilChat] Badge request failed:" << result.formatError();
        })
        .finally([this] {
            this->requestInFlight_ = false;
        })
        .execute();
}

}  // namespace chatterino
