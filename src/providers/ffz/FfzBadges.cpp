// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/ffz/FfzBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/ffz/FfzUtil.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <limits>

namespace chatterino {

namespace {

std::optional<uint64_t> canonicalUserID(QStringView value)
{
    if (value.isEmpty() || value.size() > 20 ||
        (value.size() > 1 && value.front() == u'0'))
    {
        return std::nullopt;
    }
    uint64_t id = 0;
    for (const auto character : value)
    {
        if (character < u'0' || character > u'9')
        {
            return std::nullopt;
        }
        const auto digit = static_cast<uint64_t>(character.unicode() - u'0');
        if (id > (std::numeric_limits<uint64_t>::max() - digit) / 10)
        {
            return std::nullopt;
        }
        id = id * 10 + digit;
    }
    return id;
}

void insertBadgeID(QVarLengthArray<int, 2> &badges, int badgeID)
{
    const auto position = std::ranges::lower_bound(badges, badgeID);
    if (position == badges.end() || *position != badgeID)
    {
        badges.insert(position, badgeID);
    }
}

}

std::vector<FfzBadges::Badge> FfzBadges::getUserBadges(const UserId &id)
{
    std::vector<Badge> badges;

    std::shared_lock lock(this->mutex_);

    const auto append = [&](const auto &assignments, const auto &key) {
        const auto it = assignments.find(key);
        if (it == assignments.end())
        {
            return;
        }
        for (const auto badgeID : it->second)
        {
            if (auto badge = this->getBadge(badgeID); badge)
            {
                badges.emplace_back(*badge);
            }
        }
    };
    if (const auto numericID = canonicalUserID(id.string))
    {
        append(this->userBadges, *numericID);
    }
    else
    {
        append(this->otherUserBadges, id.string);
    }

    return badges;
}

std::optional<FfzBadges::Badge> FfzBadges::getBadge(const int badgeID) const
{
    this->tgBadges.guard();
    auto it = this->badges.find(badgeID);
    if (it != this->badges.end())
    {
        return it->second;
    }

    return std::nullopt;
}

void FfzBadges::load()
{
    static QUrl url("https://api.frankerfacez.com/v1/badges/ids");

    NetworkRequest(url)
        .onSuccess([this](auto result) {
            std::unique_lock lock(this->mutex_);

            auto jsonRoot = result.parseJson();
            this->tgBadges.guard();
            for (const auto &jsonBadge_ : jsonRoot.value("badges").toArray())
            {
                auto jsonBadge = jsonBadge_.toObject();
                auto jsonUrls = jsonBadge.value("urls").toObject();
                QSize baseSize(jsonBadge["width"].toInt(18),
                               jsonBadge["height"].toInt(18));

                auto emote = Emote{
                    .name =
                        EmoteName{
                            u"frankerfacez:" %
                                jsonBadge.value("name").toString(),
                        },
                    .images =
                        ImageSet{
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("1").toString()),
                                1.0, baseSize),
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("2").toString()),
                                0.5, baseSize * 2),
                            Image::fromUrl(
                                parseFfzUrl(jsonUrls.value("4").toString()),
                                0.25, baseSize * 4)},
                    .tooltip = Tooltip{jsonBadge.value("title").toString()},
                    .homePage = Url{},
                };

                int badgeID = jsonBadge.value("id").toInt();

                this->badges[badgeID] = Badge{
                    .emote = std::make_shared<const Emote>(std::move(emote)),
                    .color = QColor(jsonBadge.value("color").toString()),
                };

                auto badgeIDString = QString::number(badgeID);
                for (const auto &user : jsonRoot.value("users")
                                            .toObject()
                                            .value(badgeIDString)
                                            .toArray())
                {
                    const auto userID = user.toInteger(-1);
                    if (userID > 0)
                    {
                        this->insertUserBadge(QString::number(userID),
                                              badgeID);
                    }
                }
            }
        })
        .execute();
}

void FfzBadges::registerBadge(int badgeID, Badge badge)
{
    assert(getApp()->isTest());

    std::unique_lock lock(this->mutex_);

    this->badges.emplace(badgeID, std::move(badge));
}

void FfzBadges::assignBadgeToUser(const UserId &userID, int badgeID)
{
    assert(getApp()->isTest());

    std::unique_lock lock(this->mutex_);

    this->insertUserBadge(userID.string, badgeID);
}

void FfzBadges::insertUserBadge(const QString &userID, int badgeID)
{
    if (const auto numericID = canonicalUserID(userID))
    {
        insertBadgeID(this->userBadges[*numericID], badgeID);
    }
    else
    {
        insertBadgeID(this->otherUserBadges[userID], badgeID);
    }
}

}
