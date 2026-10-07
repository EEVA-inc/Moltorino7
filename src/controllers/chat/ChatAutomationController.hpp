#pragma once

#include "common/Aliases.hpp"
#include "util/RapidjsonHelpers.hpp"

#include <pajlada/serialize.hpp>
#include <pajlada/settings.hpp>
#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QString>
#include <QStringList>

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

class QDateTime;
class QTimer;

namespace chatterino {

class CommandController;
class Paths;
class Settings;
class TwitchAccount;
class TwitchChannel;
struct Message;

enum class ChatAutomationMatch : uint8_t {
    Command,
    Exact,
    StartsWith,
    ContainsWords,
    ContainsText,
    RegularExpression,
    AnyMessage,
};

enum class ChatAutomationAction : uint8_t {
    SendMessage,
    RunCommand,
    ReplyToMessage,
};

enum class ChatAutomationAccess : uint8_t {
    Everyone,
    Moderators,
    Broadcaster,
    Subscribers,
    VIPs,
    NamedUsers,
};

enum class ChatAutomationArguments : uint8_t {
    Optional,
    Required,
    None,
};

enum class ChatAutomationStream : uint8_t {
    Always,
    Live,
    Offline,
};

enum class ChatAutomationChannelRole : uint8_t {
    Any,
    Moderator,
    Broadcaster,
    VIP,
};

enum class ChatAutomationResponseOrder : uint8_t {
    Random,
    NoRepeat,
    InOrder,
};

struct ChatAutomationChannelRoles {
    bool moderator = false;
    bool broadcaster = false;
    bool vip = false;
};

struct ChatAutomation {
    QString id;
    QString name;
    QString trigger;
    QStringList aliases;
    QString response;
    QStringList channels;
    QStringList excludedChannels;
    QStringList users;
    QStringList ignoredUsers;
    QString timeZone;
    QString timeFormat;
    QString dateFormat;
    QString requiredText;
    QString excludedText;
    ChatAutomationMatch match = ChatAutomationMatch::Command;
    ChatAutomationAction action = ChatAutomationAction::SendMessage;
    ChatAutomationAccess access = ChatAutomationAccess::Everyone;
    ChatAutomationArguments arguments = ChatAutomationArguments::Optional;
    ChatAutomationStream stream = ChatAutomationStream::Always;
    ChatAutomationChannelRole channelRole = ChatAutomationChannelRole::Any;
    ChatAutomationResponseOrder responseOrder =
        ChatAutomationResponseOrder::Random;
    int minimumMessageLength = 0;
    int maximumMessageLength = 0;
    int cooldownSeconds = 10;
    int userCooldownSeconds = 0;
    bool enabled = true;
    bool allOpenTwitchChannels = false;
    bool respondToSelf = true;
    bool onlySelf = false;
    bool useBotBadge = false;
    bool caseSensitive = false;
    bool randomResponse = false;
    bool use12HourTime = false;
};

struct ChatAutomationMatchResult {
    bool matched = false;
    QString trigger;
    QString arguments;
    QStringList captures;
    std::unordered_map<QString, QString> namedCaptures;
    QString error;
};

struct ChatAutomationPreview {
    ChatAutomationMatchResult match;
    QString response;
};

class ChatAutomationController final
{
public:
    ChatAutomationController(const Paths &paths, CommandController *commands);
    ~ChatAutomationController();

    ChatAutomationController(const ChatAutomationController &) = delete;
    ChatAutomationController &operator=(const ChatAutomationController &) =
        delete;

    [[nodiscard]] bool enabled() const;
    void setEnabled(bool enabled);

    [[nodiscard]] const std::vector<ChatAutomation> &rules() const;
    void setRules(std::vector<ChatAutomation> rules);
    [[nodiscard]] bool save();

    void updateOpenChannels(
        const void *owner,
        std::vector<std::shared_ptr<TwitchChannel>> channels);
    void removeOpenChannels(const void *owner);

    [[nodiscard]] QStringList openChannelNames() const;
    [[nodiscard]] static bool botBadgeConfigured();
    [[nodiscard]] static std::unordered_map<QString, QString> timeVariables(
        const ChatAutomation &rule, const QDateTime &instant);
    [[nodiscard]] static QStringList responseChoices(
        const ChatAutomation &rule);

    [[nodiscard]] static ChatAutomationMatchResult matchMessage(
        const ChatAutomation &rule, const QString &text);
    [[nodiscard]] static QString validateRule(const ChatAutomation &rule);

    [[nodiscard]] static QString conditionFailure(
        const ChatAutomation &rule, const Message &message,
        const QString &channel, bool fromSelectedAccount, bool live,
        ChatAutomationChannelRoles accountRoles = {});
    [[nodiscard]] ChatAutomationPreview preview(const ChatAutomation &rule,
                                                const QString &text,
                                                const QString &user,
                                                const QString &channel,
                                                int responseIndex = 0,
                                                quint64 counterValue = 1) const;

    pajlada::Signals::NoArgSignal rulesChanged;
    pajlada::Signals::NoArgSignal openChannelsChanged;

private:
    struct ChannelRegistration {
        std::shared_ptr<TwitchChannel> channel;
        pajlada::Signals::ScopedConnection messageConnection;
        int owners = 0;
    };

    void load(const Paths &paths);
    void handleMessage(const QString &channelKey,
                       const std::shared_ptr<TwitchChannel> &channel,
                       const std::shared_ptr<const Message> &message);
    [[nodiscard]] static QString expandResponse(
        const ChatAutomation &rule, const ChatAutomationMatchResult &match,
        const std::shared_ptr<TwitchChannel> &channel, const Message &message,
        const TwitchAccount &selectedAccount, const Settings &settings,
        CommandController &commands, int responseIndex = -1,
        quint64 counterValue = 1);
    int nextResponseIndex(const ChatAutomation &rule, const QString &channel);
    void loadCounters(const Paths &paths);
    bool saveCounters();
    [[nodiscard]] static QString normalizeExpectedEcho(
        const QString &message, bool directBotBadgeDelivery);
    void addChannel(const std::shared_ptr<TwitchChannel> &channel);
    void removeChannel(const QString &channelKey);
    [[nodiscard]] static bool audienceAllows(const ChatAutomation &rule,
                                             const Message &message);
    [[nodiscard]] bool channelRateLimitAllows(const QString &channelKey,
                                              qint64 now);
    [[nodiscard]] bool consumeRecentOutput(const QString &channelKey,
                                           const QString &message, qint64 now);
    void rememberOutput(const QString &channelKey, const QString &message,
                        qint64 now);

    struct RecentOutput {
        QString message;
        qint64 sentAt{};
    };

    CommandController *commands_{};
    QString filePath_;
    bool enabled_ = false;
    std::vector<ChatAutomation> rules_;
    std::unordered_map<QString, std::unique_ptr<ChannelRegistration>> channels_;
    std::unordered_map<const void *, QStringList> owners_;
    std::unordered_map<QString, std::unordered_map<QString, qint64>>
        ruleLastRunByChannel_;
    struct UserCooldowns {
        std::unordered_map<QString, qint64> expires;
        qint64 nextCleanupAt{};
    };
    std::unordered_map<QString, UserCooldowns> userRuns_;
    std::unordered_map<QString, std::deque<qint64>> channelRuns_;
    std::unordered_map<QString, std::deque<RecentOutput>> recentOutputs_;
    std::unordered_map<QString, std::unordered_map<QString, int>>
        responsePositions_;
    std::unordered_map<QString, quint64> counters_;
    QString counterFilePath_;
    std::unique_ptr<QTimer> counterSaveTimer_;
    bool countersDirty_ = false;

    std::shared_ptr<pajlada::Settings::SettingManager> settingsManager_;
    std::unique_ptr<pajlada::Settings::Setting<bool>> enabledSetting_;
    std::unique_ptr<pajlada::Settings::Setting<std::vector<ChatAutomation>>>
        rulesSetting_;
};

}

namespace pajlada {

template <>
struct Serialize<chatterino::ChatAutomation> {
    static rapidjson::Value get(const chatterino::ChatAutomation &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value object(rapidjson::kObjectType);
        chatterino::rj::set(object, "id", value.id, a);
        chatterino::rj::set(object, "name", value.name, a);
        chatterino::rj::set(object, "trigger", value.trigger, a);
        rapidjson::Value aliases(rapidjson::kArrayType);
        for (const auto &alias : value.aliases)
        {
            chatterino::rj::add(aliases, alias, a);
        }
        chatterino::rj::addMember(object, "aliases", aliases, a);
        chatterino::rj::set(object, "response", value.response, a);
        chatterino::rj::set(object, "timeZone", value.timeZone, a);
        chatterino::rj::set(object, "timeFormat", value.timeFormat, a);
        chatterino::rj::set(object, "dateFormat", value.dateFormat, a);
        chatterino::rj::set(object, "requiredText", value.requiredText, a);
        chatterino::rj::set(object, "excludedText", value.excludedText, a);
        chatterino::rj::set(object, "minimumMessageLength",
                            value.minimumMessageLength, a);
        chatterino::rj::set(object, "maximumMessageLength",
                            value.maximumMessageLength, a);
        chatterino::rj::set(object, "responseOrder",
                            static_cast<int>(value.responseOrder), a);
        rapidjson::Value channels(rapidjson::kArrayType);
        for (const auto &channel : value.channels)
        {
            chatterino::rj::add(channels, channel, a);
        }
        chatterino::rj::addMember(object, "channels", channels, a);
        for (const auto &[key, names] :
             {std::pair{"excludedChannels", &value.excludedChannels},
              std::pair{"users", &value.users},
              std::pair{"ignoredUsers", &value.ignoredUsers}})
        {
            rapidjson::Value entries(rapidjson::kArrayType);
            for (const auto &name : *names)
            {
                chatterino::rj::add(entries, name, a);
            }
            chatterino::rj::addMember(object, key, entries, a);
        }
        chatterino::rj::set(object, "arguments",
                            static_cast<int>(value.arguments), a);
        chatterino::rj::set(object, "stream", static_cast<int>(value.stream),
                            a);
        chatterino::rj::set(object, "channelRole",
                            static_cast<int>(value.channelRole), a);
        chatterino::rj::set(object, "match", static_cast<int>(value.match), a);
        chatterino::rj::set(object, "action", static_cast<int>(value.action),
                            a);
        chatterino::rj::set(object, "access", static_cast<int>(value.access),
                            a);
        chatterino::rj::set(object, "cooldownSeconds", value.cooldownSeconds,
                            a);
        chatterino::rj::set(object, "userCooldownSeconds",
                            value.userCooldownSeconds, a);
        chatterino::rj::set(object, "enabled", value.enabled, a);
        chatterino::rj::set(object, "allOpenTwitchChannels",
                            value.allOpenTwitchChannels, a);
        chatterino::rj::set(object, "respondToSelf", value.respondToSelf, a);
        chatterino::rj::set(object, "onlySelf", value.onlySelf, a);
        chatterino::rj::set(object, "useBotBadge", value.useBotBadge, a);
        chatterino::rj::set(object, "caseSensitive", value.caseSensitive, a);
        chatterino::rj::set(object, "randomResponse", value.randomResponse, a);
        chatterino::rj::set(object, "use12HourTime", value.use12HourTime, a);
        return object;
    }
};

template <>
struct Deserialize<chatterino::ChatAutomation> {
    static chatterino::ChatAutomation get(const rapidjson::Value &value,
                                          bool *error = nullptr);
};

}
