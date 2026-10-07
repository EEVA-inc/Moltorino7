#include "controllers/chat/ChatAutomationController.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Message.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/Backup.hpp"
#include "util/CombinePath.hpp"
#include "util/FilesystemHelpers.hpp"

#include <pajlada/settings/backup.hpp>
#include <QCache>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QTimeZone>
#include <QUuid>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace {

using namespace chatterino;

constexpr auto MIN_CHANNEL_INTERVAL_MS = qint64{1500};
constexpr auto CHANNEL_WINDOW_MS = qint64{30000};

constexpr auto RECENT_OUTPUT_LIFETIME_MS = qint64{15000};
constexpr auto MAX_RECENT_OUTPUTS_PER_CHANNEL = size_t{20};
constexpr auto MAX_COUNTER_VALUE = quint64{1000000000000};
constexpr int MAX_TRIGGER_LENGTH = 512;
constexpr int MAX_MESSAGE_LENGTH = 4096;
constexpr int MAX_RESPONSE_LENGTH = 16384;
constexpr int MAX_TIME_FORMAT_LENGTH = 128;
constexpr int MAX_COOLDOWN_SECONDS = 24 * 60 * 60;
constexpr size_t MAX_USER_COOLDOWNS_PER_CHANNEL = 30000;

qint64 monotonicNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString channelKey(QString name)
{
    name = name.trimmed().toCaseFolded();
    if (name.startsWith(QLatin1Char('@')) || name.startsWith(QLatin1Char('#')))
    {
        name.remove(0, 1);
    }
    return name;
}

bool hasBadge(const Message &message, QStringView key)
{
    return std::ranges::any_of(message.twitchBadges, [key](const auto &badge) {
        return badge.key_ == key;
    });
}

QString ensureRuleId(QString id)
{
    if (!id.trimmed().isEmpty())
    {
        return id;
    }
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString normalizedTimeZone(QString id)
{
    id = id.trimmed();
    if (id.isEmpty())
    {
        return {};
    }
    return QTimeZone(id.toUtf8()).isValid() ? id : QString{};
}

QDateTime dateTimeForRule(const ChatAutomation &rule, const QDateTime &instant)
{
    if (rule.timeZone.isEmpty())
    {
        return instant.toLocalTime();
    }

    const QTimeZone zone(rule.timeZone.toUtf8());
    return zone.isValid() ? instant.toTimeZone(zone) : instant.toLocalTime();
}

bool isWordCharacter(QChar character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}

int findWholePhrase(const QString &text, const QString &phrase,
                    Qt::CaseSensitivity sensitivity)
{
    int offset = 0;
    while (offset <= text.size() - phrase.size())
    {
        const auto found = text.indexOf(phrase, offset, sensitivity);
        if (found < 0)
        {
            return -1;
        }

        const auto end = found + phrase.size();
        const bool leftBoundary = found == 0 ||
                                  !isWordCharacter(phrase.front()) ||
                                  !isWordCharacter(text.at(found - 1));
        const bool rightBoundary = end == text.size() ||
                                   !isWordCharacter(phrase.back()) ||
                                   !isWordCharacter(text.at(end));
        if (leftBoundary && rightBoundary)
        {
            return found;
        }
        offset = found + 1;
    }
    return -1;
}

void preserveLegacyPrefixRule(ChatAutomation &rule)
{
    if (rule.match != ChatAutomationMatch::Command)
    {
        return;
    }
    const bool containsWhitespace =
        std::ranges::any_of(rule.trigger, [](QChar character) {
            return character.isSpace();
        });
    if (containsWhitespace)
    {
        rule.match = ChatAutomationMatch::StartsWith;
    }
}

QRegularExpression automationPattern(const ChatAutomation &rule)
{
    thread_local QCache<QString, QRegularExpression> cache(128);
    const auto key = QString::number(rule.caseSensitive) + rule.trigger;
    if (const auto *cached = cache.object(key))
    {
        return *cached;
    }
    QRegularExpression expression(
        QStringLiteral(
            "(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=100)(*LIMIT_HEAP=1024)") +
            rule.trigger,
        QRegularExpression::UseUnicodePropertiesOption |
            (rule.caseSensitive ? QRegularExpression::NoPatternOption
                                : QRegularExpression::CaseInsensitiveOption));
    cache.insert(key, new QRegularExpression(expression));
    return expression;
}

void normalizeRule(ChatAutomation &rule)
{
    rule.name = rule.name.trimmed();
    rule.trigger = rule.trigger.trimmed();
    rule.response = rule.response.trimmed();
    rule.timeZone = normalizedTimeZone(std::move(rule.timeZone));
    rule.cooldownSeconds =
        std::clamp(rule.cooldownSeconds, 0, MAX_COOLDOWN_SECONDS);
    rule.userCooldownSeconds =
        std::clamp(rule.userCooldownSeconds, 0, MAX_COOLDOWN_SECONDS);
    rule.minimumMessageLength =
        std::clamp(rule.minimumMessageLength, 0, MAX_MESSAGE_LENGTH);
    rule.maximumMessageLength =
        std::clamp(rule.maximumMessageLength, 0, MAX_MESSAGE_LENGTH);
    rule.requiredText = rule.requiredText.trimmed();
    rule.excludedText = rule.excludedText.trimmed();
    if (rule.requiredText.size() > MAX_TRIGGER_LENGTH ||
        rule.excludedText.size() > MAX_TRIGGER_LENGTH)
    {
        rule.enabled = false;
        rule.requiredText = rule.requiredText.left(MAX_TRIGGER_LENGTH);
        rule.excludedText = rule.excludedText.left(MAX_TRIGGER_LENGTH);
    }
    if (rule.onlySelf)
    {
        rule.respondToSelf = true;
    }
    preserveLegacyPrefixRule(rule);
    for (auto &alias : rule.aliases)
    {
        alias = alias.trimmed();
    }
    rule.aliases.removeAll(QString{});
    rule.aliases.removeDuplicates();
    for (auto *names : {&rule.channels, &rule.excludedChannels, &rule.users,
                        &rule.ignoredUsers})
    {
        for (auto &name : *names)
        {
            name = channelKey(std::move(name));
        }
        names->removeAll(QString{});
        names->removeDuplicates();
    }
}

void normalizeRules(std::vector<ChatAutomation> &rules)
{
    std::unordered_set<QString> ids;
    for (auto &rule : rules)
    {
        rule.id = ensureRuleId(std::move(rule.id));
        if (!ids.emplace(rule.id).second)
        {
            rule.id = ensureRuleId({});
            ids.emplace(rule.id);
        }
        normalizeRule(rule);
    }
}

QString expandTemplate(const ChatAutomation &rule,
                       const ChatAutomationMatchResult &match,
                       std::unordered_map<QString, QString> context,
                       CommandController &commands, const ChannelPtr &channel,
                       const Message &message, int responseIndex = -1)
{
    auto response = rule.response;
    if (rule.randomResponse)
    {
        const auto choices = ChatAutomationController::responseChoices(rule);
        if (choices.isEmpty())
        {
            return {};
        }
        const auto index =
            responseIndex < 0
                ? QRandomGenerator::global()->bounded(
                      static_cast<int>(choices.size()))
                : std::clamp(responseIndex, 0,
                             static_cast<int>(choices.size()) - 1);
        response = choices.at(index);
    }
    context.insert_or_assign(QStringLiteral("args"), match.arguments);
    context.insert_or_assign(QStringLiteral("trigger"), match.trigger);
    const auto argumentWords = match.arguments.simplified().split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    auto target = argumentWords.value(0);
    if (target.startsWith(QLatin1Char('@')))
    {
        target.remove(0, 1);
    }
    context.insert_or_assign(QStringLiteral("target"), target);
    for (const auto &[name, capture] : match.namedCaptures)
    {
        context.insert_or_assign(QStringLiteral("match.") + name, capture);
    }
    for (auto &[name, value] : ChatAutomationController::timeVariables(
             rule, QDateTime::currentDateTimeUtc()))
    {
        context.insert_or_assign(std::move(name), std::move(value));
    }

    QStringList words{match.trigger};
    if (rule.match == ChatAutomationMatch::RegularExpression)
    {
        words.append(match.captures);
    }
    else
    {
        words.append(argumentWords);
    }

    return commands.execCustomCommand(words, Command(match.trigger, response),
                                      true, channel, &message,
                                      std::move(context));
}

}

namespace pajlada {

chatterino::ChatAutomation Deserialize<chatterino::ChatAutomation>::get(
    const rapidjson::Value &value, bool *error)
{
    using namespace chatterino;
    ChatAutomation rule;
    if (!value.IsObject())
    {
        PAJLADA_REPORT_ERROR(error);
        rule.enabled = false;
        return rule;
    }

    rj::getSafe(value, "id", rule.id);
    rj::getSafe(value, "name", rule.name);
    if (!rj::getSafe(value, "trigger", rule.trigger) ||
        !rj::getSafe(value, "response", rule.response))
    {
        PAJLADA_REPORT_ERROR(error);
        rule.enabled = false;
        return rule;
    }
    rj::getSafe(value, "timeZone", rule.timeZone);
    if (value.HasMember("aliases") && value["aliases"].IsArray())
    {
        for (const auto &entry : value["aliases"].GetArray())
        {
            QString alias;
            if (rj::getSafe(entry, alias))
            {
                rule.aliases.push_back(std::move(alias));
            }
        }
    }
    if (value.HasMember("channels") && value["channels"].IsArray())
    {
        for (const auto &entry : value["channels"].GetArray())
        {
            QString channel;
            if (rj::getSafe(entry, channel))
            {
                rule.channels.push_back(std::move(channel));
            }
        }
    }

    int match = static_cast<int>(rule.match);
    int action = static_cast<int>(rule.action);
    int access = static_cast<int>(rule.access);
    int arguments = 0;
    int stream = 0;
    int channelRole = 0;
    int responseOrder = 0;
    rj::getSafe(value, "match", match);
    rj::getSafe(value, "action", action);
    rj::getSafe(value, "access", access);
    rj::getSafe(value, "arguments", arguments);
    rj::getSafe(value, "stream", stream);
    rj::getSafe(value, "channelRole", channelRole);
    rj::getSafe(value, "responseOrder", responseOrder);
    for (auto [key, names] :
         {std::pair{"excludedChannels", &rule.excludedChannels},
          std::pair{"users", &rule.users},
          std::pair{"ignoredUsers", &rule.ignoredUsers}})
    {
        if (value.HasMember(key) && value[key].IsArray())
        {
            for (const auto &entry : value[key].GetArray())
            {
                QString name;
                if (rj::getSafe(entry, name))
                {
                    names->push_back(std::move(name));
                }
            }
        }
    }
    rj::getSafe(value, "cooldownSeconds", rule.cooldownSeconds);
    rj::getSafe(value, "userCooldownSeconds", rule.userCooldownSeconds);
    rj::getSafe(value, "enabled", rule.enabled);
    rj::getSafe(value, "allOpenTwitchChannels", rule.allOpenTwitchChannels);
    rj::getSafe(value, "respondToSelf", rule.respondToSelf);
    rj::getSafe(value, "onlySelf", rule.onlySelf);
    rj::getSafe(value, "useBotBadge", rule.useBotBadge);
    rj::getSafe(value, "caseSensitive", rule.caseSensitive);
    rj::getSafe(value, "randomResponse", rule.randomResponse);
    rj::getSafe(value, "use12HourTime", rule.use12HourTime);
    rj::getSafe(value, "timeFormat", rule.timeFormat);
    rj::getSafe(value, "dateFormat", rule.dateFormat);
    rj::getSafe(value, "requiredText", rule.requiredText);
    rj::getSafe(value, "excludedText", rule.excludedText);
    rj::getSafe(value, "minimumMessageLength", rule.minimumMessageLength);
    rj::getSafe(value, "maximumMessageLength", rule.maximumMessageLength);
    if (rule.timeFormat.size() > MAX_TIME_FORMAT_LENGTH ||
        rule.dateFormat.size() > MAX_TIME_FORMAT_LENGTH)
    {
        rule.enabled = false;
    }

    const bool validMatch =
        match >= 0 &&
        match <= static_cast<int>(ChatAutomationMatch::AnyMessage);
    const bool validAction =
        action >= 0 &&
        action <= static_cast<int>(ChatAutomationAction::ReplyToMessage);
    const bool validAccess =
        access >= 0 &&
        access <= static_cast<int>(ChatAutomationAccess::NamedUsers);
    if (!validMatch || !validAction || !validAccess ||
        arguments < 0 ||
        arguments > static_cast<int>(ChatAutomationArguments::None) ||
        stream < 0 ||
        stream > static_cast<int>(ChatAutomationStream::Offline) ||
        channelRole < 0 ||
        channelRole > static_cast<int>(ChatAutomationChannelRole::VIP) ||
        responseOrder < 0 ||
        responseOrder > static_cast<int>(ChatAutomationResponseOrder::InOrder))
    {
        rule.enabled = false;
    }
    rule.arguments = static_cast<ChatAutomationArguments>(std::clamp(
        arguments, 0, static_cast<int>(ChatAutomationArguments::None)));
    rule.stream = static_cast<ChatAutomationStream>(
        std::clamp(stream, 0, static_cast<int>(ChatAutomationStream::Offline)));
    rule.channelRole = static_cast<ChatAutomationChannelRole>(std::clamp(
        channelRole, 0, static_cast<int>(ChatAutomationChannelRole::VIP)));
    rule.responseOrder = static_cast<ChatAutomationResponseOrder>(
        std::clamp(responseOrder, 0,
                   static_cast<int>(ChatAutomationResponseOrder::InOrder)));

    if (validMatch)
    {
        rule.match = static_cast<ChatAutomationMatch>(match);
    }
    if (validAction)
    {
        rule.action = static_cast<ChatAutomationAction>(action);
    }
    if (validAccess)
    {
        rule.access = static_cast<ChatAutomationAccess>(access);
    }
    rule.id = ensureRuleId(std::move(rule.id));
    normalizeRule(rule);
    return rule;
}

}

namespace chatterino {

ChatAutomationController::ChatAutomationController(const Paths &paths,
                                                   CommandController *commands)
    : commands_(commands)
{
    this->load(paths);
    this->loadCounters(paths);
    this->counterSaveTimer_ = std::make_unique<QTimer>();
    this->counterSaveTimer_->setSingleShot(true);
    this->counterSaveTimer_->setInterval(5000);
    QObject::connect(this->counterSaveTimer_.get(), &QTimer::timeout, [this] {
        if (!this->saveCounters())
        {
            this->counterSaveTimer_->start();
        }
    });
}

ChatAutomationController::~ChatAutomationController()
{
    this->counterSaveTimer_->stop();
    this->saveCounters();
}

void ChatAutomationController::loadCounters(const Paths &paths)
{
    this->counterFilePath_ = combinePath(
        paths.settingsDirectory, QStringLiteral("chat-automation-counts.json"));
    for (int backup = 0; backup <= 3; ++backup)
    {
        QFile file(
            this->counterFilePath_ +
            (backup == 0 ? QString{} : QStringLiteral(".bkp-%1").arg(backup)));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        {
            continue;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() ||
            document.object().value("version").toInt() != 1 ||
            !document.object().value("counts").isObject())
        {
            continue;
        }
        const auto entries = document.object().value("counts").toObject();
        if (entries.size() > 10000)
        {
            continue;
        }
        std::unordered_set<QString> ids;
        for (const auto &rule : this->rules_)
        {
            ids.emplace(rule.id);
        }
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            bool valid = false;
            const auto count = it.value().toString().toULongLong(&valid);
            if (valid && ids.contains(it.key()))
            {
                this->counters_.emplace(it.key(),
                                        std::min(count, MAX_COUNTER_VALUE));
            }
        }
        return;
    }
}

bool ChatAutomationController::saveCounters()
{
    if (!this->countersDirty_)
    {
        return true;
    }
    QJsonObject counts;
    for (const auto &rule : this->rules_)
    {
        if (const auto found = this->counters_.find(rule.id);
            found != this->counters_.end())
        {
            counts.insert(rule.id, QString::number(found->second));
        }
    }
    const auto data =
        QJsonDocument(QJsonObject{{"version", 1}, {"counts", counts}})
            .toJson(QJsonDocument::Compact);
    std::error_code error;
    pajlada::Settings::Backup::saveWithBackup(
        qStringToStdPath(this->counterFilePath_),
        {.enabled = true, .numSlots = 3},
        [&](const auto &path, auto &writeError) {
            QSaveFile file(stdPathToQString(path));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(data) != data.size() || !file.commit())
            {
                writeError = std::make_error_code(std::errc::io_error);
            }
        },
        error);
    if (!error)
    {
        this->countersDirty_ = false;
        std::unordered_set<QString> ids;
        for (const auto &rule : this->rules_)
        {
            ids.emplace(rule.id);
        }
        std::erase_if(this->counters_, [&](const auto &entry) {
            return !ids.contains(entry.first);
        });
    }
    return !error;
}

void ChatAutomationController::load(const Paths &paths)
{
    this->settingsManager_ =
        std::make_shared<pajlada::Settings::SettingManager>();
    this->settingsManager_->saveMethod =
        pajlada::Settings::SettingManager::SaveMethod::SaveManually;
    const auto path = combinePath(paths.settingsDirectory,
                                  QStringLiteral("chat-automations.json"));
    this->filePath_ = path;
    this->settingsManager_->setPath(qStringToStdPath(path));
    this->settingsManager_->setBackupEnabled(true);
    this->settingsManager_->setBackupSlots(5);

    this->enabledSetting_ = std::make_unique<pajlada::Settings::Setting<bool>>(
        "/enabled", pajlada::Settings::SettingOption::CompareBeforeSet,
        this->settingsManager_);
    this->rulesSetting_ = std::make_unique<
        pajlada::Settings::Setting<std::vector<ChatAutomation>>>(
        "/rules", pajlada::Settings::SettingOption::CompareBeforeSet,
        this->settingsManager_);

    backup::loadSettingManagerWithBackups(
        backup::FileData{
            .fileName = QStringLiteral("chat-automations.json"),
            .directory = paths.settingsDirectory,
            .fileKind = QStringLiteral("Chat automations"),
            .fileDescription = QStringLiteral(
                "This file contains your personal chat automations."),
        },
        this->settingsManager_);

    this->enabled_ = this->enabledSetting_->getValue();
    this->rules_ = this->rulesSetting_->getValue();
    normalizeRules(this->rules_);
}

bool ChatAutomationController::enabled() const
{
    return this->enabled_;
}

void ChatAutomationController::setEnabled(bool enabled)
{
    if (this->enabled_ == enabled)
    {
        return;
    }
    this->enabled_ = enabled;
    this->enabledSetting_->setValue(enabled);
    this->ruleLastRunByChannel_.clear();
    this->channelRuns_.clear();
    this->userRuns_.clear();
    this->responsePositions_.clear();

    this->rulesChanged.invoke();
}

const std::vector<ChatAutomation> &ChatAutomationController::rules() const
{
    return this->rules_;
}

void ChatAutomationController::setRules(std::vector<ChatAutomation> rules)
{
    normalizeRules(rules);
    this->rules_ = std::move(rules);
    this->rulesSetting_->setValue(this->rules_);
    this->ruleLastRunByChannel_.clear();
    this->userRuns_.clear();
    this->responsePositions_.clear();

    this->rulesChanged.invoke();
}

bool ChatAutomationController::save()
{
    rapidjson::Document document(rapidjson::kObjectType);
    rj::set(document, "enabled", this->enabled_);
    rj::set(document, "rules", this->rules_);
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    std::error_code error;
    pajlada::Settings::Backup::saveWithBackup(
        qStringToStdPath(this->filePath_), {.enabled = true, .numSlots = 5},
        [&](const auto &path, auto &writeError) {
            QSaveFile file(stdPathToQString(path));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(buffer.GetString(), buffer.GetSize()) !=
                    static_cast<qint64>(buffer.GetSize()) ||
                !file.commit())
            {
                writeError = std::make_error_code(std::errc::io_error);
            }
        },
        error);
    return !error;
}

void ChatAutomationController::updateOpenChannels(
    const void *owner, std::vector<std::shared_ptr<TwitchChannel>> channels)
{
    QStringList next;
    std::unordered_map<QString, std::shared_ptr<TwitchChannel>> unique;
    for (auto &channel : channels)
    {
        if (!channel)
        {
            continue;
        }
        auto key = channelKey(channel->getName());
        if (key.isEmpty())
        {
            continue;
        }
        if (unique.emplace(key, channel).second)
        {
            next.push_back(key);
        }
    }

    const auto existing = this->owners_.find(owner);
    const auto previous =
        existing == this->owners_.end() ? QStringList{} : existing->second;
    if (previous == next)
    {
        return;
    }

    for (const auto &key : previous)
    {
        if (!next.contains(key))
        {
            this->removeChannel(key);
        }
    }
    for (const auto &key : next)
    {
        if (!previous.contains(key))
        {
            this->addChannel(unique.at(key));
        }
    }

    if (next.isEmpty())
    {
        this->owners_.erase(owner);
    }
    else
    {
        this->owners_[owner] = std::move(next);
    }
    this->openChannelsChanged.invoke();
}

void ChatAutomationController::removeOpenChannels(const void *owner)
{
    const auto existing = this->owners_.find(owner);
    if (existing == this->owners_.end())
    {
        return;
    }
    const auto channels = existing->second;
    this->owners_.erase(existing);
    for (const auto &key : channels)
    {
        this->removeChannel(key);
    }
    this->openChannelsChanged.invoke();
}

void ChatAutomationController::addChannel(
    const std::shared_ptr<TwitchChannel> &channel)
{
    auto key = channelKey(channel->getName());
    auto existing = this->channels_.find(key);
    if (existing != this->channels_.end())
    {
        existing->second->owners++;
        return;
    }

    auto registration = std::make_unique<ChannelRegistration>();
    registration->channel = channel;
    registration->owners = 1;
    registration->messageConnection = channel->messageAppended.connect(
        [this, key, weak = std::weak_ptr<TwitchChannel>(channel)](
            MessagePtr &message, std::optional<MessageFlags>) {
            if (auto locked = weak.lock())
            {
                this->handleMessage(key, locked, message);
            }
        });
    this->channels_.emplace(std::move(key), std::move(registration));
}

void ChatAutomationController::removeChannel(const QString &key)
{
    auto existing = this->channels_.find(key);
    if (existing == this->channels_.end())
    {
        return;
    }
    existing->second->owners--;
    if (existing->second->owners <= 0)
    {
        this->channels_.erase(existing);
        this->channelRuns_.erase(key);
        this->ruleLastRunByChannel_.erase(key);
        this->userRuns_.erase(key);
        this->recentOutputs_.erase(key);
        this->responsePositions_.erase(key);
    }
}

QStringList ChatAutomationController::openChannelNames() const
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(this->channels_.size()));
    for (const auto &[key, registration] : this->channels_)
    {
        names.push_back(registration->channel->getName());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool ChatAutomationController::botBadgeConfigured()
{
    const auto &settings = *getSettings();
    return !settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty() &&
           !settings.botBadgeClientID.getValue().trimmed().isEmpty() &&
           !settings.botBadgeUserID.getValue().trimmed().isEmpty();
}

std::unordered_map<QString, QString> ChatAutomationController::timeVariables(
    const ChatAutomation &rule, const QDateTime &instant)
{
    const auto now = dateTimeForRule(rule, instant);
    const auto clockPattern =
        rule.timeFormat.isEmpty()
            ? (rule.use12HourTime ? QStringLiteral("h:mm AP")
                                  : QStringLiteral("HH:mm"))
            : rule.timeFormat.left(MAX_TIME_FORMAT_LENGTH);
    const auto datePattern = rule.dateFormat.isEmpty()
                                 ? QStringLiteral("yyyy-MM-dd")
                                 : rule.dateFormat.left(MAX_TIME_FORMAT_LENGTH);
    const auto time = now.toString(clockPattern);
    const auto date = now.toString(datePattern);
    return {
        {QStringLiteral("time"), time},
        {QStringLiteral("date"), date},
        {QStringLiteral("datetime"), date + QLatin1Char(' ') + time},
        {QStringLiteral("time.zone"), now.timeZoneAbbreviation()},
        {QStringLiteral("datetime.iso"), now.toString(Qt::ISODate)},
        {QStringLiteral("timestamp"), QString::number(now.toSecsSinceEpoch())},
        {QStringLiteral("date.weekday"), now.toString(QStringLiteral("dddd"))}};
}

QStringList ChatAutomationController::responseChoices(
    const ChatAutomation &rule)
{
    auto choices = rule.response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto &choice : choices)
    {
        choice = choice.trimmed();
    }
    choices.removeAll(QString{});
    if (rule.responseOrder == ChatAutomationResponseOrder::NoRepeat)
    {
        for (auto &choice : choices)
        {
            choice = choice.simplified();
        }
        choices.removeDuplicates();
    }
    return choices;
}

int ChatAutomationController::nextResponseIndex(const ChatAutomation &rule,
                                                const QString &channel)
{
    if (!rule.randomResponse ||
        rule.responseOrder == ChatAutomationResponseOrder::Random)
    {
        return -1;
    }
    const auto count = static_cast<int>(responseChoices(rule).size());
    if (count == 0)
    {
        return -1;
    }
    auto &positions = this->responsePositions_[channel];
    const auto previous = positions.find(rule.id);
    const auto last = previous == positions.end() ? -1 : previous->second;
    int next = 0;
    if (rule.responseOrder == ChatAutomationResponseOrder::InOrder)
    {
        next = (last + 1) % count;
    }
    else if (count > 1 && last >= 0 && last < count)
    {
        next = QRandomGenerator::global()->bounded(count - 1);
        next += next >= last ? 1 : 0;
    }
    else
    {
        next = QRandomGenerator::global()->bounded(count);
    }
    positions.insert_or_assign(rule.id, next);
    return next;
}

ChatAutomationMatchResult ChatAutomationController::matchMessage(
    const ChatAutomation &rule, const QString &text)
{
    ChatAutomationMatchResult result;
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return result;
    }
    if (rule.match == ChatAutomationMatch::AnyMessage)
    {
        result.matched = true;
        result.arguments = trimmed;
        return result;
    }
    if (rule.trigger.isEmpty())
    {
        return result;
    }
    const auto sensitivity =
        rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (rule.match == ChatAutomationMatch::RegularExpression)
    {
        if (rule.trigger.size() > MAX_TRIGGER_LENGTH ||
            trimmed.size() > MAX_MESSAGE_LENGTH)
        {
            result.error =
                QStringLiteral("Keep patterns to %1 characters and messages "
                               "to %2 characters.")
                    .arg(MAX_TRIGGER_LENGTH)
                    .arg(MAX_MESSAGE_LENGTH);
            return result;
        }
        const auto pattern = automationPattern(rule);
        if (!pattern.isValid())
        {
            result.error = pattern.errorString();
            return result;
        }
        const auto match = pattern.match(trimmed);
        if (!match.isValid())
        {
            result.error = QStringLiteral("This pattern is too expensive for "
                                          "chat. Simplify the pattern.");
            return result;
        }
        if (!match.hasMatch())
        {
            return result;
        }
        result.matched = true;
        result.trigger = match.captured();
        result.arguments = trimmed.mid(match.capturedEnd()).trimmed();
        result.captures = match.capturedTexts().mid(1);
        const auto names = pattern.namedCaptureGroups();
        for (qsizetype i = 1; i < names.size(); ++i)
        {
            if (!names.at(i).isEmpty())
            {
                result.namedCaptures.emplace(names.at(i), match.captured(i));
            }
        }
        return result;
    }
    QStringList triggers{rule.trigger};
    if (rule.match == ChatAutomationMatch::Command)
    {
        triggers.append(rule.aliases);
    }
    for (const auto &trigger : triggers)
    {
        if (trigger.isEmpty())
        {
            continue;
        }
        qsizetype offset = -1;
        if (rule.match == ChatAutomationMatch::Exact)
        {
            if (trimmed.compare(trigger, sensitivity) == 0)
            {
                offset = 0;
            }
        }
        else if (rule.match == ChatAutomationMatch::ContainsWords)
        {
            offset = findWholePhrase(trimmed, trigger, sensitivity);
        }
        else if (rule.match == ChatAutomationMatch::ContainsText)
        {
            offset = trimmed.indexOf(trigger, 0, sensitivity);
        }
        else if (trimmed.startsWith(trigger, sensitivity) &&
                 (trimmed.size() == trigger.size() ||
                  trimmed.at(trigger.size()).isSpace()))
        {
            offset = 0;
        }
        if (offset >= 0)
        {
            result.matched = true;
            result.trigger = trigger;
            result.arguments = trimmed.mid(offset + trigger.size()).trimmed();
            if (rule.match == ChatAutomationMatch::Command ||
                rule.match == ChatAutomationMatch::StartsWith)
            {
                if (rule.arguments == ChatAutomationArguments::Required &&
                    result.arguments.isEmpty())
                {
                    result.matched = false;
                    result.error =
                        QStringLiteral("This rule requires text after %1.")
                            .arg(trigger);
                }
                else if (rule.arguments == ChatAutomationArguments::None &&
                         !result.arguments.isEmpty())
                {
                    result.matched = false;
                    result.error =
                        QStringLiteral(
                            "This rule only matches %1 with no text after it.")
                            .arg(trigger);
                }
            }
            return result;
        }
    }
    return result;
}

QString ChatAutomationController::validateRule(const ChatAutomation &rule)
{
    if (rule.match != ChatAutomationMatch::AnyMessage &&
        rule.trigger.trimmed().isEmpty())
    {
        return QStringLiteral("Add a trigger.");
    }
    if (rule.response.trimmed().isEmpty())
    {
        return QStringLiteral("Add a response.");
    }
    if (rule.trigger.size() > MAX_TRIGGER_LENGTH ||
        rule.response.size() > MAX_RESPONSE_LENGTH)
    {
        return QStringLiteral("Keep triggers to %1 characters and responses "
                              "to %2 characters.")
            .arg(MAX_TRIGGER_LENGTH)
            .arg(MAX_RESPONSE_LENGTH);
    }
    if (rule.timeFormat.size() > MAX_TIME_FORMAT_LENGTH ||
        rule.dateFormat.size() > MAX_TIME_FORMAT_LENGTH)
    {
        return QStringLiteral("Keep each date or time format to %1 characters.")
            .arg(MAX_TIME_FORMAT_LENGTH);
    }
    if (rule.maximumMessageLength > 0 &&
        rule.minimumMessageLength > rule.maximumMessageLength)
    {
        return QStringLiteral(
            "Minimum message length must not exceed the maximum.");
    }
    if (rule.match == ChatAutomationMatch::RegularExpression)
    {
        const auto pattern = automationPattern(rule);
        if (!pattern.isValid())
        {
            return QStringLiteral("Fix the pattern: %1")
                .arg(pattern.errorString());
        }
    }
    if (rule.match == ChatAutomationMatch::Command)
    {
        static const QRegularExpression whitespace(QStringLiteral("\\s"));
        auto triggers = rule.aliases;
        triggers.prepend(rule.trigger);
        for (const auto &trigger : triggers)
        {
            if (trigger.contains(whitespace))
            {
                return QStringLiteral("Use one word per command or choose "
                                      "Starts with for a phrase.");
            }
        }
    }
    if (!rule.timeZone.isEmpty() &&
        !QTimeZone(rule.timeZone.toUtf8()).isValid())
    {
        return QStringLiteral("Choose a valid time zone.");
    }
    if (!rule.allOpenTwitchChannels && rule.channels.isEmpty())
    {
        return QStringLiteral(
            "Choose at least one channel or use All open channels.");
    }
    if (rule.access == ChatAutomationAccess::NamedUsers && rule.users.isEmpty())
    {
        return QStringLiteral("Add at least one username under Conditions.");
    }
    return {};
}

bool ChatAutomationController::audienceAllows(const ChatAutomation &rule,
                                              const Message &message)
{
    if (rule.access == ChatAutomationAccess::NamedUsers)
    {
        return rule.users.contains(message.loginName, Qt::CaseInsensitive);
    }
    if (rule.access == ChatAutomationAccess::Everyone)
    {
        return true;
    }
    const bool broadcaster = hasBadge(message, u"broadcaster");
    if (rule.access == ChatAutomationAccess::Broadcaster)
    {
        return broadcaster;
    }
    if (broadcaster || hasBadge(message, u"moderator"))
    {
        return true;
    }
    if (rule.access == ChatAutomationAccess::Subscribers)
    {
        return hasBadge(message, u"subscriber") ||
               hasBadge(message, u"founder");
    }
    if (rule.access == ChatAutomationAccess::VIPs)
    {
        return hasBadge(message, u"vip");
    }
    return false;
}

QString ChatAutomationController::conditionFailure(
    const ChatAutomation &rule, const Message &message, const QString &channel,
    bool fromSelectedAccount, bool live,
    ChatAutomationChannelRoles accountRoles)
{
    const auto text = message.messageText.trimmed();
    const auto sensitivity =
        rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!rule.requiredText.isEmpty() &&
        !text.contains(rule.requiredText, sensitivity))
    {
        return QStringLiteral("The message must contain the required text.");
    }
    if (!rule.excludedText.isEmpty() &&
        text.contains(rule.excludedText, sensitivity))
    {
        return QStringLiteral(
            "The message contains text excluded by this rule.");
    }
    if (text.size() < rule.minimumMessageLength ||
        (rule.maximumMessageLength > 0 &&
         text.size() > rule.maximumMessageLength))
    {
        return QStringLiteral(
            "The message is outside the chosen length limits.");
    }
    const auto key = channelKey(channel);
    if ((!rule.allOpenTwitchChannels &&
         !rule.channels.contains(key, Qt::CaseInsensitive)) ||
        (rule.allOpenTwitchChannels &&
         rule.excludedChannels.contains(key, Qt::CaseInsensitive)))
    {
        return QStringLiteral("This channel is not selected for this rule.");
    }
    if (rule.channelRole == ChatAutomationChannelRole::Moderator &&
        !accountRoles.moderator && !accountRoles.broadcaster)
    {
        return QStringLiteral(
            "Your selected account must be a moderator or broadcaster in this "
            "channel.");
    }
    if (rule.channelRole == ChatAutomationChannelRole::Broadcaster &&
        !accountRoles.broadcaster)
    {
        return QStringLiteral("This rule only runs in your own channel.");
    }
    if (rule.channelRole == ChatAutomationChannelRole::VIP &&
        !accountRoles.vip && !accountRoles.moderator &&
        !accountRoles.broadcaster)
    {
        return QStringLiteral(
            "Your selected account must be a VIP, moderator or broadcaster in "
            "this channel.");
    }
    if (rule.onlySelf && !fromSelectedAccount)
    {
        return QStringLiteral(
            "This rule only responds to messages from your selected account.");
    }
    if (fromSelectedAccount && !rule.respondToSelf)
    {
        return QStringLiteral(
            "Messages from your selected account are excluded.");
    }
    if (rule.ignoredUsers.contains(message.loginName, Qt::CaseInsensitive))
    {
        return QStringLiteral("This username is in the ignored list.");
    }
    if (!audienceAllows(rule, message))
    {
        return QStringLiteral("This user is not allowed by this rule.");
    }
    if (rule.stream == ChatAutomationStream::Live && !live)
    {
        return QStringLiteral("This rule only runs while the stream is live.");
    }
    if (rule.stream == ChatAutomationStream::Offline && live)
    {
        return QStringLiteral(
            "This rule only runs while the stream is offline.");
    }
    return {};
}

bool ChatAutomationController::channelRateLimitAllows(const QString &key,
                                                      qint64 now)
{
    const auto maximumRuns = static_cast<size_t>(std::clamp(
        getSettings()->chatAutomationsMaxRunsPer30Seconds.getValue(), 1, 10));
    auto &runs = this->channelRuns_[key];
    while (!runs.empty() && now - runs.front() >= CHANNEL_WINDOW_MS)
    {
        runs.pop_front();
    }
    if ((!runs.empty() && now - runs.back() < MIN_CHANNEL_INTERVAL_MS) ||
        runs.size() >= maximumRuns)
    {
        return false;
    }
    runs.push_back(now);
    return true;
}

bool ChatAutomationController::consumeRecentOutput(const QString &key,
                                                   const QString &message,
                                                   qint64 now)
{
    auto found = this->recentOutputs_.find(key);
    if (found == this->recentOutputs_.end())
    {
        return false;
    }

    auto &outputs = found->second;
    while (!outputs.empty() &&
           now - outputs.front().sentAt >= RECENT_OUTPUT_LIFETIME_MS)
    {
        outputs.pop_front();
    }

    const auto normalized = message.simplified();
    const auto output =
        std::ranges::find(outputs, normalized, &RecentOutput::message);
    if (output == outputs.end())
    {
        if (outputs.empty())
        {
            this->recentOutputs_.erase(found);
        }
        return false;
    }

    outputs.erase(output);
    if (outputs.empty())
    {
        this->recentOutputs_.erase(found);
    }
    return true;
}

void ChatAutomationController::rememberOutput(const QString &key,
                                              const QString &message,
                                              qint64 now)
{
    auto &outputs = this->recentOutputs_[key];
    while (!outputs.empty() &&
           now - outputs.front().sentAt >= RECENT_OUTPUT_LIFETIME_MS)
    {
        outputs.pop_front();
    }
    while (outputs.size() >= MAX_RECENT_OUTPUTS_PER_CHANNEL)
    {
        outputs.pop_front();
    }
    outputs.push_back({message.simplified(), now});
}

QString ChatAutomationController::normalizeExpectedEcho(
    const QString &message, bool directBotBadgeDelivery)
{
    if (directBotBadgeDelivery)
    {
        return message.simplified();
    }

    return getApp()
        ->getEmotes()
        ->getEmojis()
        ->replaceShortCodes(message)
        .simplified();
}

QString ChatAutomationController::expandResponse(
    const ChatAutomation &rule, const ChatAutomationMatchResult &match,
    const std::shared_ptr<TwitchChannel> &channel, const Message &message,
    const TwitchAccount &selectedAccount, const Settings &settings,
    CommandController &commands, int responseIndex, quint64 counterValue)
{
    QString senderName = selectedAccount.getUserName();
    QString senderID = selectedAccount.getUserId();
    const auto botUserID = settings.botBadgeUserID.getValue().trimmed();
    const bool globalBotBadgeDelivery =
        settings.botBadgeAlwaysUse &&
        (settings.botBadgeOverrideAllAccounts ||
         (!botUserID.isEmpty() && selectedAccount.getUserId() == botUserID));
    if (rule.action != ChatAutomationAction::RunCommand &&
        ((rule.action == ChatAutomationAction::SendMessage &&
          rule.useBotBadge) ||
         globalBotBadgeDelivery) &&
        !settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty() &&
        !settings.botBadgeClientID.getValue().trimmed().isEmpty() &&
        !botUserID.isEmpty())
    {
        senderID = botUserID;
        const auto botLogin = settings.botBadgeUserLogin.getValue().trimmed();
        if (!botLogin.isEmpty())
        {
            senderName = botLogin;
        }
    }

    const auto streamStatus = channel->accessStreamStatus();
    const auto displayName =
        message.displayName.isEmpty() ? message.loginName : message.displayName;
    std::unordered_map<QString, QString> context{
        {QStringLiteral("count"), QString::number(counterValue)},
        {QStringLiteral("user.display_name"), displayName},
        {QStringLiteral("user.name"), message.loginName},
        {QStringLiteral("user.id"), message.userID},
        {QStringLiteral("msg.text"), message.messageText},
        {QStringLiteral("msg.id"), message.id},
        {QStringLiteral("channel.name"), channel->getName()},
        {QStringLiteral("channel.id"), channel->roomId()},
        {QStringLiteral("my.name"), senderName},
        {QStringLiteral("my.id"), senderID},
        {QStringLiteral("stream.title"), streamStatus->title},
        {QStringLiteral("stream.game"), streamStatus->game},
    };

    return expandTemplate(rule, match, std::move(context), commands, channel,
                          message, responseIndex);
}

ChatAutomationPreview ChatAutomationController::preview(
    const ChatAutomation &rule, const QString &text, const QString &user,
    const QString &channelName, int responseIndex, quint64 counterValue) const
{
    ChatAutomationPreview result;
    result.match = matchMessage(rule, text);
    if (!result.match.matched || !this->commands_)
    {
        return result;
    }
    auto login =
        user.trimmed().isEmpty() ? QStringLiteral("viewer42") : user.trimmed();
    if (login.startsWith(QLatin1Char('@')))
    {
        login.remove(0, 1);
    }
    const auto name = channelName.trimmed().isEmpty()
                          ? QStringLiteral("channel")
                          : channelKey(channelName);

    const auto channel = std::make_shared<Channel>(name, Channel::Type::None);
    Message message;
    message.loginName = login.toCaseFolded();
    message.displayName = login;
    message.userID = QStringLiteral("123456");
    message.id = QStringLiteral("message123");
    message.messageText = text;
    std::unordered_map<QString, QString> context{
        {QStringLiteral("count"), QString::number(counterValue)},
        {QStringLiteral("user.display_name"), message.displayName},
        {QStringLiteral("user.name"), message.loginName},
        {QStringLiteral("user.id"), message.userID},
        {QStringLiteral("msg.text"), text},
        {QStringLiteral("msg.id"), message.id},
        {QStringLiteral("channel.name"), name},
        {QStringLiteral("channel.id"), QStringLiteral("987654")},
        {QStringLiteral("my.name"), QStringLiteral("myaccount")},
        {QStringLiteral("my.id"), QStringLiteral("555555")},
        {QStringLiteral("stream.title"), QStringLiteral("Late night stream")},
        {QStringLiteral("stream.game"), QStringLiteral("Just Chatting")},
    };
    result.response =
        expandTemplate(rule, result.match, std::move(context), *this->commands_,
                       channel, message, responseIndex)
            .simplified();
    return result;
}

void ChatAutomationController::handleMessage(
    const QString &key, const std::shared_ptr<TwitchChannel> &channel,
    const std::shared_ptr<const Message> &message)
{
    if (!this->enabled_ || this->rules_.empty() || !message ||
        !this->commands_ || !channel || !channel->canSendMessage() ||
        message->platform != MessagePlatform::AnyOrTwitch ||
        message->messageText.trimmed().isEmpty() ||
        message->loginName.trimmed().isEmpty())
    {
        return;
    }

    const auto &settings = *getSettings();
    if (!settings.chatAutomationsRunInBackground.getValue() &&
        QGuiApplication::applicationState() != Qt::ApplicationActive)
    {
        return;
    }

    constexpr MessageFlags ignored{MessageFlag::System,
                                   MessageFlag::Timeout,
                                   MessageFlag::Disabled,
                                   MessageFlag::Subscription,
                                   MessageFlag::Whisper,
                                   MessageFlag::AutoMod,
                                   MessageFlag::AutoModOffendingMessage,
                                   MessageFlag::AutoModOffendingMessageHeader,
                                   MessageFlag::ModerationAction,
                                   MessageFlag::RecentMessage,
                                   MessageFlag::SharedMessage,
                                   MessageFlag::EventSub};
    if (message->flags.hasAny(ignored))
    {
        return;
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return;
    }
    const auto botUserID = settings.botBadgeUserID.getValue().trimmed();
    const auto botUserLogin = settings.botBadgeUserLogin.getValue().trimmed();
    const bool fromSelectedAccount =
        (!message->userID.isEmpty() &&
         message->userID == account->getUserId()) ||
        message->loginName.compare(account->getUserName(),
                                   Qt::CaseInsensitive) == 0;
    const bool fromBotAccount =
        (!botUserID.isEmpty() && !message->userID.isEmpty() &&
         message->userID == botUserID) ||
        (!botUserLogin.isEmpty() &&
         message->loginName.compare(botUserLogin, Qt::CaseInsensitive) == 0);

    const auto now = monotonicNowMs();
    if ((fromSelectedAccount || fromBotAccount) &&
        this->consumeRecentOutput(key, message->messageText, now))
    {
        return;
    }

    auto &ruleLastRun = this->ruleLastRunByChannel_[key];
    for (const auto &rule : this->rules_)
    {
        if (!rule.enabled ||
            (rule.trigger.isEmpty() &&
             rule.match != ChatAutomationMatch::AnyMessage) ||
            rule.response.isEmpty() ||
            (fromBotAccount && !fromSelectedAccount))
        {
            continue;
        }

        const ChatAutomationChannelRoles accountRoles{
            .moderator = channel->isMod(),
            .broadcaster = channel->isBroadcaster(),
            .vip = rule.channelRole == ChatAutomationChannelRole::VIP &&
                   std::ranges::any_of(
                       channel->currentUserBadges(account->getUserId()),
                       [](const auto &badge) {
                           return badge.key_ == u"vip";
                       })};
        if (!conditionFailure(rule, *message, key, fromSelectedAccount,
                              channel->isLive(), accountRoles)
                 .isEmpty())
        {
            continue;
        }

        const auto match = matchMessage(rule, message->messageText);
        if (!match.matched)
        {
            continue;
        }

        const auto last = ruleLastRun.find(rule.id);
        if (last != ruleLastRun.end() &&
            now - last->second < qint64{rule.cooldownSeconds} * 1000)
        {
            return;
        }
        if ((rule.action == ChatAutomationAction::SendMessage &&
             rule.useBotBadge && !botBadgeConfigured()) ||
            (rule.action == ChatAutomationAction::ReplyToMessage &&
             message->id.isEmpty()))
        {
            return;
        }
        auto &userRuns = this->userRuns_[key];
        const auto userKey =
            rule.id + QLatin1Char('\n') +
            (message->userID.isEmpty()
                 ? QStringLiteral("login:") + message->loginName.toCaseFolded()
                 : QStringLiteral("id:") + message->userID);
        const auto previousUser = userRuns.expires.find(userKey);
        if (rule.userCooldownSeconds > 0 &&
            previousUser != userRuns.expires.end() &&
            now < previousUser->second)
        {
            return;
        }
        if (!this->channelRateLimitAllows(key, now))
        {
            return;
        }

        ruleLastRun[rule.id] = now;
        if (rule.userCooldownSeconds > 0)
        {
            if (now >= userRuns.nextCleanupAt ||
                userRuns.expires.size() >= MAX_USER_COOLDOWNS_PER_CHANNEL)
            {
                std::erase_if(userRuns.expires, [now](const auto &entry) {
                    return entry.second <= now;
                });
                userRuns.nextCleanupAt = now + CHANNEL_WINDOW_MS;
            }

            if (userRuns.expires.size() >= MAX_USER_COOLDOWNS_PER_CHANNEL)
            {
                return;
            }
            userRuns.expires.insert_or_assign(
                userKey, now + qint64{rule.userCooldownSeconds} * 1000);
        }

        quint64 counter = 1;
        static const QRegularExpression counterToken(
            QStringLiteral(R"((^|[^{])(?:\{\{)*\{count(?:;[^}]*)?\})"));
        if (counterToken.match(rule.response).hasMatch())
        {
            auto &uses = this->counters_[rule.id];
            uses = std::min(uses + 1, MAX_COUNTER_VALUE);
            counter = uses;
            this->countersDirty_ = true;
            if (!this->counterSaveTimer_->isActive())
            {
                this->counterSaveTimer_->start();
            }
        }
        const auto responseIndex = this->nextResponseIndex(rule, key);
        auto output =
            expandResponse(rule, match, channel, *message, *account,
                           settings, *this->commands_, responseIndex, counter);
        output = output.trimmed();
        if (output.isEmpty())
        {
            return;
        }

        const auto action = rule.action;
        const bool directBotBadgeDelivery =
            action == ChatAutomationAction::SendMessage && rule.useBotBadge &&
            botBadgeConfigured();
        if (action == ChatAutomationAction::RunCommand)
        {
            output = this->commands_->execCommand(output.simplified(), channel,
                                                  false);
            if (output.trimmed().isEmpty())
            {
                return;
            }
        }
        output = output.simplified();
        const auto expectedEcho =
            normalizeExpectedEcho(output, directBotBadgeDelivery);
        if (action != ChatAutomationAction::RunCommand &&
            (output.startsWith(QLatin1Char('.')) ||
             output.startsWith(QLatin1Char('/'))))
        {
            output.prepend(QStringLiteral(". "));
        }
        this->rememberOutput(key, expectedEcho, now);
        if (directBotBadgeDelivery)
        {
            channel->sendBotMessage(output);
        }
        else if (action == ChatAutomationAction::ReplyToMessage)
        {
            channel->sendReply(output, message->id);
        }
        else
        {
            channel->sendMessage(output);
        }
        return;
    }
}

}
