// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Settings.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Modes.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "controllers/ignores/HiddenUser.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "controllers/nicknames/Nickname.hpp"
#include "debug/Benchmark.hpp"
#include "pajlada/settings/signalargs.hpp"
#include "util/Backup.hpp"
#include "util/RapidjsonHelpers.hpp"
#include "util/WindowsHelper.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <rapidjson/pointer.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cmath>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

float normalizedUiScale(float value)
{
    if (!std::isfinite(value))
    {
        return 1.F;
    }
    return std::round(std::clamp(value, 0.2F, 10.F) * 10000.F) / 10000.F;
}

template <typename T>
void initializeSignalVector(pajlada::Signals::SignalHolder &signalHolder,
                            ChatterinoSetting<std::vector<T>> &setting,
                            SignalVector<T> &vec)
{
    // Fill the SignalVector up with initial values
    for (auto &&item : setting.getValue())
    {
        vec.append(item);
    }

    // Set up a signal to
    signalHolder.managedConnect(vec.delayedItemsChanged, [&] {
        setting.setValue(vec.raw());
    });
}

struct OutgoingTranslationChannelSettings {
    QString channel;
    QString mode;
    QString targetLanguage;
};

QString normalizedOutgoingTranslationChannel(QString channelName)
{
    channelName = channelName.trimmed().toLower();
    if (channelName.startsWith('#'))
    {
        channelName.remove(0, 1);
    }

    return channelName;
}

std::optional<OutgoingTranslationChannelSettings>
parseOutgoingTranslationChannelSettings(const QString &entry)
{
    const auto parts = entry.split('\t');
    if (parts.size() < 3)
    {
        return std::nullopt;
    }

    auto channel = normalizedOutgoingTranslationChannel(parts.at(0));
    if (channel.isEmpty())
    {
        return std::nullopt;
    }

    return OutgoingTranslationChannelSettings{
        .channel = channel,
        .mode = parts.at(1).trimmed(),
        .targetLanguage = parts.at(2).trimmed(),
    };
}

QString formatOutgoingTranslationChannelSettings(
    const OutgoingTranslationChannelSettings &settings)
{
    return QStringList{settings.channel, settings.mode,
                       settings.targetLanguage}
        .join(QLatin1Char('\t'));
}

QString normalizedUsercardActionPlacement(QString placement)
{
    placement = placement.trimmed().toLower();
    if (placement == u"menu" || placement == u"hidden")
    {
        return placement;
    }
    return QStringLiteral("bar");
}

}  // namespace

namespace chatterino {

struct RegisteredSetting {
    std::weak_ptr<pajlada::Settings::SettingData> data;
    SettingResetter resetToDefault;
    SettingValidator validate;
};

std::vector<RegisteredSetting> _settings;

void _actuallyRegisterSetting(
    std::weak_ptr<pajlada::Settings::SettingData> setting,
    SettingResetter resetToDefault, SettingValidator validate)
{
    std::erase_if(_settings, [](const auto &entry) {
        return entry.data.expired();
    });
    if (std::ranges::any_of(_settings, [&](const auto &entry) {
            return !entry.data.owner_before(setting) &&
                   !setting.owner_before(entry.data);
        }))
    {
        return;
    }
    _settings.push_back(
        {std::move(setting), std::move(resetToDefault), validate});
}

bool Settings::isEmoteModifierEnabled(const QString &name) const
{
    return this->enableEmoteModifiers.getValue() &&
           !this->disabledEmoteModifiers.getValue().contains(name);
}

bool Settings::isHighlightedUser(const QString &username)
{
    auto items = this->highlightedUsers.readOnly();

    for (const auto &highlightedUser : *items)
    {
        if (highlightedUser.isMatch(username))
        {
            return true;
        }
    }

    return false;
}

bool Settings::isBlacklistedUser(const QString &username)
{
    auto items = this->blacklistedUsers.readOnly();

    for (const auto &blacklistedUser : *items)
    {
        if (blacklistedUser.isMatch(username))
        {
            return true;
        }
    }

    return false;
}

bool Settings::isMutedChannel(const QString &channelName)
{
    auto items = this->mutedChannels.readOnly();

    for (const auto &channel : *items)
    {
        if (channelName.toLower() == channel.toLower())
        {
            return true;
        }
    }
    return false;
}

bool Settings::isAutoTranslateChannel(const QString &channelName)
{
    auto items = this->autoTranslateChannels.readOnly();

    for (const auto &channel : *items)
    {
        if (channelName.compare(channel, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }
    return false;
}

std::optional<QString> Settings::matchNickname(const QString &usernameText)
{
    auto nicknames = this->nicknames.readOnly();

    for (const auto &nickname : *nicknames)
    {
        if (auto nicknameText = nickname.match(usernameText))
        {
            return nicknameText;
        }
    }

    return std::nullopt;
}

void Settings::mute(const QString &channelName)
{
    if (!this->isMutedChannel(channelName))
    {
        this->mutedChannels.append(channelName);
    }
}

void Settings::unmute(const QString &channelName)
{
    for (std::vector<int>::size_type i = 0;
         i != this->mutedChannels.raw().size(); i++)
    {
        if (this->mutedChannels.raw()[i].toLower() == channelName.toLower())
        {
            this->mutedChannels.removeAt(i);
            i--;
        }
    }
}

void Settings::enableAutoTranslateChannel(const QString &channelName)
{
    if (!this->isAutoTranslateChannel(channelName))
    {
        this->autoTranslateChannels.append(channelName);
    }
}

void Settings::disableAutoTranslateChannel(const QString &channelName)
{
    for (std::vector<int>::size_type i = 0;
         i != this->autoTranslateChannels.raw().size(); i++)
    {
        if (this->autoTranslateChannels.raw()[i].compare(
                channelName, Qt::CaseInsensitive) == 0)
        {
            this->autoTranslateChannels.removeAt(i);
            i--;
        }
    }
}

bool Settings::toggleMutedChannel(const QString &channelName)
{
    if (this->isMutedChannel(channelName))
    {
        this->unmute(channelName);
        return false;
    }
    else
    {
        this->mutedChannels.append(channelName);
        return true;
    }
}

bool Settings::toggleAutoTranslateChannel(const QString &channelName)
{
    if (this->isAutoTranslateChannel(channelName))
    {
        this->disableAutoTranslateChannel(channelName);
        return false;
    }

    this->autoTranslateChannels.append(channelName);
    return true;
}

QString Settings::outgoingTranslationModeForChannel(const QString &channelName)
{
    const auto channel = normalizedOutgoingTranslationChannel(channelName);
    if (channel.isEmpty())
    {
        return this->outgoingTranslationMode.getValue();
    }

    for (const auto &entry :
         this->outgoingTranslationChannelSettingsSetting.getValue())
    {
        const auto parsed = parseOutgoingTranslationChannelSettings(entry);
        if (parsed.has_value() && parsed->channel == channel &&
            !parsed->mode.isEmpty())
        {
            return parsed->mode;
        }
    }

    return this->outgoingTranslationMode.getValue();
}

QString Settings::outgoingTranslationTargetLanguageForChannel(
    const QString &channelName)
{
    const auto channel = normalizedOutgoingTranslationChannel(channelName);
    if (channel.isEmpty())
    {
        return this->outgoingTranslationTargetLanguage.getValue();
    }

    for (const auto &entry :
         this->outgoingTranslationChannelSettingsSetting.getValue())
    {
        const auto parsed = parseOutgoingTranslationChannelSettings(entry);
        if (parsed.has_value() && parsed->channel == channel &&
            !parsed->targetLanguage.isEmpty())
        {
            return parsed->targetLanguage;
        }
    }

    return this->outgoingTranslationTargetLanguage.getValue();
}

void Settings::setOutgoingTranslationModeForChannel(
    const QString &channelName, const QString &mode)
{
    const auto channel = normalizedOutgoingTranslationChannel(channelName);
    if (channel.isEmpty())
    {
        this->outgoingTranslationMode.setValue(mode);
        return;
    }

    auto entries = this->outgoingTranslationChannelSettingsSetting.getValue();
    std::vector<QString> cleaned;
    cleaned.reserve(entries.size() + 1);
    bool updated = false;
    for (const auto &entry : entries)
    {
        auto parsed = parseOutgoingTranslationChannelSettings(entry);
        if (!parsed.has_value())
        {
            continue;
        }

        if (parsed->channel == channel)
        {
            if (updated)
            {
                continue;
            }

            parsed->mode = mode;
            if (parsed->targetLanguage.isEmpty())
            {
                parsed->targetLanguage =
                    this->outgoingTranslationTargetLanguage.getValue();
            }
            updated = true;
        }

        cleaned.push_back(formatOutgoingTranslationChannelSettings(*parsed));
    }

    if (!updated)
    {
        cleaned.push_back(formatOutgoingTranslationChannelSettings({
            .channel = channel,
            .mode = mode,
            .targetLanguage =
                this->outgoingTranslationTargetLanguage.getValue(),
        }));
    }

    this->outgoingTranslationChannelSettingsSetting.setValue(cleaned);
}

void Settings::setOutgoingTranslationTargetLanguageForChannel(
    const QString &channelName, const QString &targetLanguage)
{
    const auto channel = normalizedOutgoingTranslationChannel(channelName);
    if (channel.isEmpty())
    {
        this->outgoingTranslationTargetLanguage.setValue(targetLanguage);
        return;
    }

    auto entries = this->outgoingTranslationChannelSettingsSetting.getValue();
    std::vector<QString> cleaned;
    cleaned.reserve(entries.size() + 1);
    bool updated = false;
    for (const auto &entry : entries)
    {
        auto parsed = parseOutgoingTranslationChannelSettings(entry);
        if (!parsed.has_value())
        {
            continue;
        }

        if (parsed->channel == channel)
        {
            if (updated)
            {
                continue;
            }

            parsed->targetLanguage = targetLanguage;
            updated = true;
        }

        cleaned.push_back(formatOutgoingTranslationChannelSettings(*parsed));
    }

    if (!updated)
    {
        cleaned.push_back(formatOutgoingTranslationChannelSettings({
            .channel = channel,
            .mode = this->outgoingTranslationMode.getValue(),
            .targetLanguage = targetLanguage,
        }));
    }

    this->outgoingTranslationChannelSettingsSetting.setValue(cleaned);
}

Settings *Settings::instance_ = nullptr;

Settings::Settings(const Modes &modes, const Args &args,
                   const QString &settingsDirectory,
                   const SettingsArgs &settingsArgs)
    : prevInstance_(Settings::instance_)
    , disableSaving(args.dontSaveSettings)
    , createShortcutForToasts(
          "/notifications/createShortcutForToasts",
          (modes.isPortable || modes.isExternallyPackaged) ? false : true)
{
    QString settingsPath = settingsDirectory + "/settings.json";

    // get global instance of the settings library
    auto settingsInstance = pajlada::Settings::SettingManager::getInstance();

    if (settingsArgs.isTest)
    {
        settingsInstance->load(qPrintable(settingsPath));
    }
    else
    {
        backup::loadWithBackups(
            backup::FileData{
                .fileName = u"settings.json"_s,
                .directory = settingsDirectory,
                .fileKind = u"Settings"_s,
                .fileDescription =
                    u"This file contains the main application settings such as accounts and hotkeys."_s,
            },
            [&]() -> ExpectedStr<void> {
                using LoadError = pajlada::Settings::SettingManager::LoadError;
                auto err = settingsInstance->load(qPrintable(settingsPath));
                switch (err)
                {
                    case LoadError::NoError:
                        return {};  // ok
                    case LoadError::CannotOpenFile:
                        return makeUnexpected(u"Failed to open '" %
                                              settingsPath % '\'');
                    case LoadError::FileHandleError:
                        return makeUnexpected("File handle error");
                    case LoadError::FileReadError:
                        return makeUnexpected("Failed to read file");
                    case LoadError::FileSeekError:
                        return makeUnexpected("Failed to seek in file");
                    case LoadError::JSONParseError:
                        return makeUnexpected("File contained malformed JSON");
                }
                assert(false);
                return makeUnexpected("Unknown error");
            });
    }

    settingsInstance->setBackupEnabled(true);
    settingsInstance->setBackupSlots(9);
    settingsInstance->saveMethod = static_cast<
        pajlada::Settings::SettingManager::SaveMethod>(
        static_cast<uint64_t>(
            pajlada::Settings::SettingManager::SaveMethod::SaveManually) |
        static_cast<uint64_t>(
            pajlada::Settings::SettingManager::SaveMethod::OnlySaveIfChanged));

    initializeSignalVector(this->signalHolder, this->highlightedMessagesSetting,
                           this->highlightedMessages);
    initializeSignalVector(this->signalHolder, this->highlightedUsersSetting,
                           this->highlightedUsers);
    initializeSignalVector(this->signalHolder, this->highlightWordListsSetting,
                           this->highlightWordLists);
    initializeSignalVector(this->signalHolder, this->highlightedBadgesSetting,
                           this->highlightedBadges);
    initializeSignalVector(this->signalHolder, this->blacklistedUsersSetting,
                           this->blacklistedUsers);
    initializeSignalVector(this->signalHolder, this->ignoredMessagesSetting,
                           this->ignoredMessages);
    initializeSignalVector(this->signalHolder, this->hiddenUsersSetting,
                           this->hiddenUsers);
    initializeSignalVector(this->signalHolder, this->mutedChannelsSetting,
                           this->mutedChannels);
    initializeSignalVector(this->signalHolder,
                           this->autoTranslateChannelsSetting,
                           this->autoTranslateChannels);
    initializeSignalVector(this->signalHolder, this->filterRecordsSetting,
                           this->filterRecords);
    initializeSignalVector(this->signalHolder, this->nicknamesSetting,
                           this->nicknames);
    initializeSignalVector(this->signalHolder, this->moderationActionsSetting,
                           this->moderationActions);
    initializeSignalVector(this->signalHolder, this->loggedChannelsSetting,
                           this->loggedChannels);

    instance_ = this;

    BoolSetting popupSizesReset("/moltorino/appearance/popupSizesResetV1", false);
    if (!popupSizesReset)
    {
        for (auto *setting : {&this->pollPopupSize, &this->predictionPopupSize,
                              &this->editorPopupSize, &this->rewardsPopupSize,
                              &this->vanityPopupSize, &this->gifPopupSize,
                              &this->usercardPopupSize, &this->threadPopupSize})
        {
            setting->resetToDefaultValue();
        }
        popupSizesReset = true;
        this->requestSave();
    }

#ifdef USEWINSDK
    if (!settingsArgs.isTest)
    {
        repairStartupRegistration();
    }
    this->autorun = isRegisteredForStartup();
    this->autorun.connect(
        [](bool autorun) {
            setRegisteredForStartup(autorun);
        },
        false);
#endif

    // migration for `/emotes/showUnlistedEmotes` -> `/emotes/showUnlistedSevenTVEmotes`
    if (this->showUnlistedEmotesDontUse && !this->showUnlistedSevenTVEmotes)
    {
        this->showUnlistedSevenTVEmotes.setValue(true);
        // reset to default, so it doesn't appear in the config
        settingsInstance->removeSetting(
            this->showUnlistedEmotesDontUse.getPath());
    }

    // migration for `/appearance/badges/homies`
    // -> `/appearance/badges/homies/supporter` and
    //    `/appearance/badges/homies/custom`
    constexpr const char *OLD_HOMIES_BADGES_SETTING =
        "/appearance/badges/homies";
    if (auto *oldHomiesBadgesSetting =
            settingsInstance->get(OLD_HOMIES_BADGES_SETTING);
        oldHomiesBadgesSetting != nullptr && oldHomiesBadgesSetting->IsBool())
    {
        const auto enabled = oldHomiesBadgesSetting->GetBool();

        // removeSetting would also unregister the new split badge settings
        const auto removedOldSetting =
            rapidjson::Pointer(OLD_HOMIES_BADGES_SETTING)
                .Erase(settingsInstance->document);
        const auto wroteSupporter =
            this->showBadgesHomiesSupporter.setValue(enabled);
        const auto wroteCustom = this->showBadgesHomiesCustom.setValue(enabled);
        if (removedOldSetting || wroteSupporter || wroteCustom)
        {
            this->requestSave();
        }
    }

    auto saveHomiesBadgeSetting = [this](bool, auto) {
        this->requestSave();
    };
    this->showBadgesHomiesSupporter.connect(saveHomiesBadgeSetting,
                                            this->signalHolder, false);
    this->showBadgesHomiesCustom.connect(saveHomiesBadgeSetting,
                                         this->signalHolder, false);

    if (this->spellCheckingDefaultDictionary.getValue().isEmpty())
    {
        this->spellCheckingDefaultDictionary.setValue("included:en_US");
        this->requestSave();
    }

    bool migratedUsercardActionPlacement = false;
    auto configureUsercardActionPlacement =
        [this, settingsInstance, &migratedUsercardActionPlacement](
            QStringSetting &placement, BoolSetting *legacyVisibility,
            QString defaultVisiblePlacement) {
            const bool placementWasStored =
                settingsInstance->get(placement.getPath()) != nullptr;
            auto normalized =
                normalizedUsercardActionPlacement(placement.getValue());
            if (!placementWasStored && legacyVisibility != nullptr)
            {
                normalized = legacyVisibility->getValue()
                                 ? defaultVisiblePlacement
                                 : QStringLiteral("hidden");
            }
            else if (legacyVisibility != nullptr &&
                     legacyVisibility->getValue() != (normalized != u"hidden"))
            {
                normalized = legacyVisibility->getValue()
                                 ? defaultVisiblePlacement
                                 : QStringLiteral("hidden");
            }
            if (placement.getValue() != normalized)
            {
                migratedUsercardActionPlacement |=
                    placement.setValue(normalized);
            }

            if (legacyVisibility == nullptr)
            {
                auto *placementPtr = &placement;
                placement.connect(
                    [placementPtr](const QString &value) {
                        const auto normalizedValue =
                            normalizedUsercardActionPlacement(value);
                        if (value != normalizedValue)
                        {
                            placementPtr->setValue(normalizedValue);
                        }
                    },
                    this->signalHolder, false);
                return;
            }

            const bool visible = normalized != u"hidden";
            if (legacyVisibility->getValue() != visible)
            {
                migratedUsercardActionPlacement |=
                    legacyVisibility->setValue(visible);
            }

            auto *placementPtr = &placement;
            placement.connect(
                [placementPtr, legacyVisibility](const QString &value) {
                    const auto normalizedValue =
                        normalizedUsercardActionPlacement(value);
                    if (value != normalizedValue)
                    {
                        placementPtr->setValue(normalizedValue);
                        return;
                    }
                    legacyVisibility->setValue(normalizedValue != u"hidden");
                },
                this->signalHolder, false);
            legacyVisibility->connect(
                [placementPtr, defaultVisiblePlacement = std::move(
                                   defaultVisiblePlacement)](bool visible) {
                    const auto current = normalizedUsercardActionPlacement(
                        placementPtr->getValue());
                    if (!visible)
                    {
                        placementPtr->setValue(QStringLiteral("hidden"));
                    }
                    else if (current == u"hidden")
                    {
                        placementPtr->setValue(defaultVisiblePlacement);
                    }
                },
                this->signalHolder, false);
        };

    configureUsercardActionPlacement(this->usercardUsercardActionPlacement,
                                     nullptr, QStringLiteral("menu"));
    configureUsercardActionPlacement(this->usercardCommentsActionPlacement,
                                     &this->showModeratorCommentsButton,
                                     QStringLiteral("bar"));
    configureUsercardActionPlacement(this->usercardLogsActionPlacement, nullptr,
                                     QStringLiteral("bar"));
    configureUsercardActionPlacement(this->usercardRolesActionPlacement,
                                     nullptr, QStringLiteral("bar"));
    configureUsercardActionPlacement(this->usercardSevenTVActionPlacement,
                                     &this->showSevenTVUsercardButton,
                                     QStringLiteral("bar"));
    configureUsercardActionPlacement(this->usercardNotesActionPlacement,
                                     nullptr, QStringLiteral("bar"));
    configureUsercardActionPlacement(this->usercardBlockActionPlacement,
                                     nullptr, QStringLiteral("menu"));
    configureUsercardActionPlacement(this->usercardHideActionPlacement, nullptr,
                                     QStringLiteral("menu"));
    configureUsercardActionPlacement(
        this->usercardIgnoreHighlightsActionPlacement, nullptr,
        QStringLiteral("menu"));
    configureUsercardActionPlacement(this->usercardCrossBanActionPlacement,
                                     &this->showUsercardCrossBanAction,
                                     QStringLiteral("menu"));
    configureUsercardActionPlacement(this->usercardCrossUnbanActionPlacement,
                                     &this->showUsercardCrossUnbanAction,
                                     QStringLiteral("menu"));
    if (migratedUsercardActionPlacement)
    {
        this->requestSave();
    }

    // migration for `/moltorino/pinnedMessages/showPinButtonOnModerators`
    // -> `/moltorino/pinnedMessages/showPinButtonOnModeratorsMode`
    constexpr const char *OLD_PIN_MODERATOR_BUTTON_SETTING =
        "/moltorino/pinnedMessages/showPinButtonOnModerators";
    if (auto *oldPinSetting =
            settingsInstance->get(OLD_PIN_MODERATOR_BUTTON_SETTING);
        oldPinSetting != nullptr)
    {
        if (settingsInstance->get(
                this->showPinButtonOnModeratorsMode.getPath()) == nullptr)
        {
            this->showPinButtonOnModeratorsMode.setValue(
                oldPinSetting->IsBool() && oldPinSetting->GetBool() ? 1 : 0);
        }
        settingsInstance->removeSetting(OLD_PIN_MODERATOR_BUTTON_SETTING);
    }
}

Settings::~Settings()
{
    Settings::instance_ = this->prevInstance_;
}

pajlada::Settings::SettingManager::SaveResult Settings::requestSave() const
{
    if (this->disableSaving)
    {
        return pajlada::Settings::SettingManager::SaveResult::Skipped;
    }

    return pajlada::Settings::SettingManager::gSave();
}

void Settings::saveSnapshot()
{
    BenchmarkGuard benchmark("Settings::saveSnapshot");

    rapidjson::Document *d = new rapidjson::Document(rapidjson::kObjectType);
    rapidjson::Document::AllocatorType &a = d->GetAllocator();
    this->snapshotUnsetPaths_.clear();

    for (const auto &entry : _settings)
    {
        auto setting = entry.data.lock();
        if (!setting)
        {
            continue;
        }

        rapidjson::Value key(setting->getPath().c_str(), a);
        auto *curVal = setting->unmarshalJSON();
        if (curVal == nullptr)
        {
            this->snapshotUnsetPaths_.insert(setting->getPath());
            continue;
        }

        rapidjson::Value val;
        val.CopyFrom(*curVal, a);
        d->AddMember(key.Move(), val.Move(), a);
    }

    // log("Snapshot state: {}", rj::stringify(*d));

    this->snapshot_.reset(d);
}

void Settings::restoreSnapshot()
{
    if (!this->snapshot_)
    {
        return;
    }

    BenchmarkGuard benchmark("Settings::restoreSnapshot");

    const auto &snapshot = *(this->snapshot_.get());

    if (!snapshot.IsObject())
    {
        return;
    }

    const auto registeredSettings = _settings;
    for (const auto &entry : registeredSettings)
    {
        auto setting = entry.data.lock();
        if (!setting)
        {
            continue;
        }

        const char *path = setting->getPath().c_str();

        if (!snapshot.HasMember(path))
        {
            if (this->snapshotUnsetPaths_.contains(path) &&
                setting->unmarshalJSON() != nullptr)
            {
                entry.resetToDefault(setting->getPath());
            }
            continue;
        }

        pajlada::Settings::SignalArgs args;
        args.compareBeforeSet = true;

        setting->marshalJSON(snapshot[path], std::move(args));
    }
}

QJsonObject Settings::registeredSettingsSnapshot() const
{
    rapidjson::Document document(rapidjson::kObjectType);
    auto &allocator = document.GetAllocator();

    for (const auto &entry : _settings)
    {
        const auto setting = entry.data.lock();
        if (!setting)
        {
            continue;
        }

        const auto *current = setting->unmarshalJSON();
        if (current == nullptr)
        {
            continue;
        }

        rapidjson::Value value;
        value.CopyFrom(*current, allocator);
        const auto path = setting->getPath();
        if (auto existing = document.FindMember(path.c_str());
            existing != document.MemberEnd())
        {
            existing->value = std::move(value);
        }
        else
        {
            rapidjson::Value key(path.c_str(), allocator);
            document.AddMember(key.Move(), value.Move(), allocator);
        }
    }

    const auto parsed =
        QJsonDocument::fromJson(rj::stringify(document).toUtf8());
    return parsed.isObject() ? parsed.object() : QJsonObject{};
}

bool Settings::isSettingRegistered(const QString &path) const
{
    const auto utf8Path = path.toUtf8().toStdString();
    return std::ranges::any_of(_settings, [&](const auto &entry) {
        const auto setting = entry.data.lock();
        return setting && setting->getPath() == utf8Path;
    });
}

bool Settings::isValidSettingValue(const QString &path,
                                   const QJsonValue &value) const
{
    const auto json = QJsonDocument(QJsonObject{{"value", value}})
                          .toJson(QJsonDocument::Compact);
    rapidjson::Document document;
    document.Parse(json.constData(), json.size());
    if (document.HasParseError() || !document.HasMember("value"))
    {
        return false;
    }
    const auto utf8Path = path.toUtf8().toStdString();
    for (const auto &entry : _settings)
    {
        const auto setting = entry.data.lock();
        if (setting && setting->getPath() == utf8Path)
        {
            return entry.validate(document["value"]);
        }
    }
    return false;
}

bool Settings::isSaveEnabled() const
{
    return !this->disableSaving;
}

void Settings::disableSave()
{
    this->disableSaving = true;
}

bool Settings::shouldSendHelixChat() const
{
    switch (this->chatSendProtocol.getEnum())
    {
        case ChatSendProtocol::Helix:
            return true;
        case ChatSendProtocol::Default:
        case ChatSendProtocol::IRC:
            return false;
        default:
            assert(false && "Invalid chat protocol value");
            return false;
    }
}

float Settings::getClampedUiScale() const
{
    return normalizedUiScale(this->uiScale.getValue());
}

void Settings::setClampedUiScale(float value)
{
    this->uiScale.setValue(normalizedUiScale(value));
}

float Settings::getClampedOverlayScale() const
{
    return std::clamp(this->overlayScaleFactor.getValue(), 0.2F, 10.F);
}

void Settings::setClampedOverlayScale(float value)
{
    this->overlayScaleFactor.setValue(std::clamp(value, 0.2F, 10.F));
}

Settings &Settings::instance()
{
    assert(instance_ != nullptr);

    return *instance_;
}

Settings *getSettings()
{
    return &Settings::instance();
}

}  // namespace chatterino
