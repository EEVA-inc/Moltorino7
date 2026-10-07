#pragma once

#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/serialize.hpp>
#include <QString>

#include <cstdint>
#include <tuple>
#include <utility>

namespace chatterino {

enum class HiddenUserPlatform : std::uint8_t {
    Twitch,
    Kick,
    YouTube,
    TikTok,
};

QString hiddenUserPlatformName(HiddenUserPlatform platform);
QString hiddenUserPlatformKey(HiddenUserPlatform platform);
HiddenUserPlatform hiddenUserPlatformFromKey(const QString &key);

class HiddenUser
{
public:
    HiddenUser() = default;
    HiddenUser(HiddenUserPlatform platform, QString userID, QString login,
               QString displayName);

    bool operator==(const HiddenUser &other) const;

    HiddenUserPlatform platform() const;
    const QString &userID() const;
    const QString &login() const;
    const QString &displayName() const;

    QString bestName() const;
    QString displayLabel() const;
    bool matchesIdentity(HiddenUserPlatform platform, const QString &userID,
                         const QString &login,
                         const QString &displayName = {}) const;

private:
    HiddenUserPlatform platform_ = HiddenUserPlatform::Twitch;
    QString userID_;
    QString login_;
    QString displayName_;
};

}

namespace pajlada {

template <>
struct Serialize<chatterino::HiddenUser> {
    static rapidjson::Value get(const chatterino::HiddenUser &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value ret(rapidjson::kObjectType);
        chatterino::rj::set(ret, "platform",
                            chatterino::hiddenUserPlatformKey(value.platform()),
                            a);
        chatterino::rj::set(ret, "userID", value.userID(), a);
        chatterino::rj::set(ret, "login", value.login(), a);
        chatterino::rj::set(ret, "displayName", value.displayName(), a);
        return ret;
    }
};

template <>
struct Deserialize<chatterino::HiddenUser> {
    static chatterino::HiddenUser get(const rapidjson::Value &value,
                                      bool *error = nullptr)
    {
        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error)
            return {};
        }

        QString platform;
        QString userID;
        QString login;
        QString displayName;
        chatterino::rj::getSafe(value, "platform", platform);
        chatterino::rj::getSafe(value, "userID", userID);
        chatterino::rj::getSafe(value, "login", login);
        chatterino::rj::getSafe(value, "displayName", displayName);
        return {
            chatterino::hiddenUserPlatformFromKey(platform),
            std::move(userID),
            std::move(login),
            std::move(displayName),
        };
    }
};

}
