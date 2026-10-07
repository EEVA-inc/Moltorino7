#include "providers/tiktok/TikTokAccountManager.hpp"

#include "common/AccountCredentials.hpp"
#include "common/ChatterinoSetting.hpp"
#include "providers/tiktok/TikTokTypes.hpp"
#include "singletons/Settings.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep
#include "util/SharedPtrElementLess.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUuid>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;
namespace {

constexpr int PART_SIZE = 32 * 1024;
constexpr int MAX_PARTS = 19;
const auto ROOT = "/tiktokAccounts/";
const auto SETTINGS_ERROR =
    u"Could not save the TikTok account settings. Check available disk space and try again."_s;

QString credentialKey(const QString &userID, const QString &revision, int part)
{
    return u"tiktok-session/%1/%2/%3"_s.arg(userID, revision).arg(part);
}
bool validRevision(const QString &revision)
{
    const QUuid uuid(revision);
    return !uuid.isNull() &&
           uuid.toString(QUuid::WithoutBraces) == revision;
}
std::string metadataPath(const QString &userID)
{
    return ROOT + userID.toStdString();
}
std::string cleanupPath(const QString &revision)
{
    return std::string(ROOT) + "cleanup/" + revision.toStdString();
}

QString cleanupMetadata(const QString &userID, int parts)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{u"userID"_s, userID}, {u"parts"_s, parts}})
            .toJson(QJsonDocument::Compact));
}

void restoreMetadata(const std::string &path, const QString &value)
{
    if (value.isEmpty())
    {
        pajlada::Settings::SettingManager::gRemoveSetting(path);
    }
    else
    {
        QStringSetting::set(path, value);
    }
}
}

struct TikTokAccountManager::Save {
    TikTokAccountData data;
    QString encoded;
    Stored stored;
    SaveCallback callback;
    quint64 generation = 0;
    int written = 0;
    std::shared_ptr<TikTokAccount> existing;
};

TikTokAccountManager::TikTokAccountManager(CredentialStore store)
    : accounts(SharedPtrElementLess<TikTokAccount>{})
    , store_(std::move(store))
    , anonymous_(std::make_shared<TikTokAccount>(
          TikTokAccountData{.displayName = u"Anonymous"_s}))
{
    if (!this->store_.read)
    {
        this->store_.read = [](const auto &key, auto callback) {
            accountCredentials().read(key, std::move(callback));
        };
    }
    if (!this->store_.write)
    {
        this->store_.write = [](const auto &key, const auto &value,
                                auto callback) {
            accountCredentials().write(key, value, std::move(callback));
        };
    }
    if (!this->store_.remove)
    {
        this->store_.remove = [](const auto &key, auto callback) {
            accountCredentials().remove(key, std::move(callback));
        };
    }
    if (!this->store_.saveSettings)
    {
        this->store_.saveSettings = [] {
            return getSettings()->requestSave() ==
                   pajlada::Settings::SettingManager::SaveResult::Success;
        };
    }
    this->current_ = this->anonymous_;
    std::ignore = this->accounts.itemRemoved.connect([this](const auto &args) {
        if (args.caller != this)
        {
            this->removed(args.item);
        }
    });
}

TikTokAccountManager::~TikTokAccountManager() = default;

void TikTokAccountManager::load()
{
    if (this->loaded_)
    {
        return;
    }
    this->loaded_ = true;
    for (const auto &key :
         pajlada::Settings::SettingManager::getObjectKeys("/tiktokAccounts"))
    {
        const auto id = QString::fromStdString(key);
        if (!isTikTokUserID(id))
        {
            continue;
        }
        const auto json = QJsonDocument::fromJson(
                              QStringSetting::get(metadataPath(id)).toUtf8())
                              .object();
        const auto handle =
            normalizeTikTokHandle(json.value("handle").toString());
        const auto name = json.value("name").toString().trimmed().left(256);
        Stored stored{id, json.value("revision").toString(),
                      json.value("parts").toInt()};
        if (!handle || name.isEmpty() || !validRevision(stored.revision) ||
            stored.parts <= 0 || stored.parts > MAX_PARTS)
        {
            continue;
        }
        auto account = std::make_shared<TikTokAccount>(TikTokAccountData{
            .userID = id, .handle = *handle, .displayName = name});
        account->setLoadingCredentials(!json.value("needsLogin").toBool());
        this->stored_.insert(id, stored);
        this->accounts.insert(account);
        if (account->isLoadingCredentials())
        {
            this->readNext(account, stored, 0, {});
        }
    }
    this->current_ = this->findByUserID(this->selected_.getValue());
    if (!this->current_)
    {
        this->current_ = this->anonymous_;
    }
    this->userListUpdated.invoke();
    this->currentChanged.invoke();
    for (const auto &key : pajlada::Settings::SettingManager::getObjectKeys(
             std::string(ROOT) + "cleanup"))
    {
        const auto revision = QString::fromStdString(key);
        const auto json =
            QJsonDocument::fromJson(
                QStringSetting::get(cleanupPath(revision)).toUtf8())
                .object();
        Stored stored{json.value("userID").toString(), revision,
                      json.value("parts").toInt()};
        if (validRevision(revision) && isTikTokUserID(stored.userID) &&
            stored.parts > 0 && stored.parts <= MAX_PARTS &&
            this->stored_.value(stored.userID).revision != revision)
        {
            this->cleanupNext(stored, 0);
        }
    }
}

void TikTokAccountManager::readNext(
    const std::shared_ptr<TikTokAccount> &account, const Stored &stored,
    int index, QString data)
{
    if (index == stored.parts)
    {
        auto decoded = decodeTikTokSession(data);
        account->setLoadingCredentials(false);
        if (decoded)
        {
            account->session() = std::move(*decoded);
        }
        else
        {
            this->reportCredentialError(decoded.error());
        }
        this->credentialsChanged.invoke();
        return;
    }
    this->store_.read(
        credentialKey(stored.userID, stored.revision, index),
        [guard = QPointer(this), account, stored, index,
         data = std::move(data)](ExpectedStr<QString> result) mutable {
            if (!guard || guard->findByUserID(stored.userID) != account ||
                guard->stored_.value(stored.userID).revision !=
                    stored.revision ||
                !account->isLoadingCredentials())
            {
                return;
            }
            if (!result || result->size() > PART_SIZE ||
                data.size() + result->size() > PART_SIZE)
            {
                account->setLoadingCredentials(false);
                guard->reportCredentialError(
                    u"The saved TikTok session could not be loaded. Log in again."_s);
                guard->credentialsChanged.invoke();
                return;
            }
            data.append(*result);
            guard->readNext(account, stored, index + 1, std::move(data));
        });
}

void TikTokAccountManager::addAccount(TikTokAccountData data,
                                      SaveCallback callback)
{
    this->saveAccount(std::move(data), std::move(callback));
}

void TikTokAccountManager::sessionUpdated(
    const std::shared_ptr<TikTokAccount> &account)
{
    if (!getSettings()->isSaveEnabled() || !account ||
        !account->hasCredentials() ||
        this->pendingLogins_.contains(account->userID()) ||
        this->findByUserID(account->userID()) != account)
    {
        return;
    }
    if (this->refreshingAccounts_.contains(account->userID()))
    {
        this->dirtySessions_.insert(account->userID());
        return;
    }
    auto *&timer = this->refreshTimers_[account->userID()];
    if (!timer)
    {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        QObject::connect(
            timer, &QTimer::timeout, this, [this, id = account->userID()] {
                const auto current = this->findByUserID(id);
                if (!getSettings()->isSaveEnabled() || !current ||
                    !current->hasCredentials() ||
                    this->pendingLogins_.contains(id) ||
                    this->refreshingAccounts_.contains(id))
                {
                    return;
                }
                this->refreshingAccounts_.insert(id);
                this->saveAccount(
                    {id, current->handle(), current->displayName(),
                     current->session()},
                    [weak = QPointer(this), id](ExpectedStr<void> result) {
                        if (!weak)
                        {
                            return;
                        }
                        weak->refreshingAccounts_.remove(id);
                        if (!result && !weak->pendingLogins_.contains(id))
                        {
                            weak->reportCredentialError(result.error());
                        }
                        if (weak->dirtySessions_.remove(id))
                        {
                            weak->sessionUpdated(weak->findByUserID(id));
                        }
                    },
                    current);
            });
    }
    if (!timer->isActive())
    {
        timer->start(2000);
    }
}

void TikTokAccountManager::saveAccount(TikTokAccountData data,
                                       SaveCallback callback,
                                       std::shared_ptr<TikTokAccount> existing)
{
    const auto handle = normalizeTikTokHandle(data.handle);
    data.displayName = data.displayName.trimmed().left(256);
    auto encoded = encodeTikTokSession(data.session);
    if (!isTikTokUserID(data.userID) || !handle || data.displayName.isEmpty() ||
        !encoded)
    {
        callback(makeUnexpected(
            u"TikTok did not return a usable account. Try logging in again."_s));
        return;
    }
    data.handle = *handle;
    auto save = std::make_shared<Save>();
    save->stored = {data.userID,
                    QUuid::createUuid().toString(QUuid::WithoutBraces),
                    int((encoded->size() + PART_SIZE - 1) / PART_SIZE)};
    save->generation = ++this->generations_[data.userID];
    save->data = std::move(data);
    save->encoded = std::move(*encoded);
    save->callback = std::move(callback);
    save->existing = std::move(existing);
    if (!save->existing)
    {
        const auto &id = save->stored.userID;
        this->pendingLogins_.insert(id, save->generation);
        if (auto *timer = this->refreshTimers_.value(id))
        {
            timer->stop();
        }
        save->callback =
            [guard = QPointer(this), id, generation = save->generation,
             callback = std::move(save->callback)](ExpectedStr<void> result) {
                if (guard && guard->pendingLogins_.value(id) == generation)
                {
                    guard->pendingLogins_.remove(id);

                    if (!result)
                    {
                        guard->sessionUpdated(guard->findByUserID(id));
                    }
                }
                callback(std::move(result));
            };
    }

    QStringSetting::set(
        cleanupPath(save->stored.revision),
        cleanupMetadata(save->stored.userID, save->stored.parts));
    if (!this->store_.saveSettings())
    {
        pajlada::Settings::SettingManager::gRemoveSetting(
            cleanupPath(save->stored.revision));
        save->callback(makeUnexpected(SETTINGS_ERROR));
        return;
    }
    this->writeNext(save);
}

void TikTokAccountManager::writeNext(const std::shared_ptr<Save> &save)
{
    if (this->generations_.value(save->stored.userID) != save->generation ||
        (save->existing &&
         (this->findByUserID(save->stored.userID) != save->existing ||
          !save->existing->hasCredentials())))
    {
        this->queueCleanup(save->stored);
        save->callback(
            makeUnexpected(u"This TikTok login was replaced or removed."_s));
        return;
    }
    if (save->written < save->stored.parts)
    {
        const int part = save->written;
        this->store_.write(
            credentialKey(save->stored.userID, save->stored.revision, part),
            save->encoded.mid(part * PART_SIZE, PART_SIZE),
            [guard = QPointer(this), save](ExpectedStr<void> result) {
                if (!guard)
                {
                    return;
                }
                if (!result)
                {
                    guard->queueCleanup(save->stored);
                    save->callback(makeUnexpected(
                        u"Could not save the TikTok login in settings."_s));
                    return;
                }
                ++save->written;
                guard->writeNext(save);
            });
        return;
    }

    const auto old = this->stored_.value(save->stored.userID);
    const auto oldMetadata =
        QStringSetting::get(metadataPath(save->stored.userID));
    const auto oldCleanup =
        old.revision.isEmpty() ? QString{}
                               : QStringSetting::get(cleanupPath(old.revision));
    const auto oldSelected = this->selected_.getValue();
    const auto json = QJsonObject{{u"handle"_s, save->data.handle},
                                  {u"name"_s, save->data.displayName},
                                  {u"revision"_s, save->stored.revision},
                                  {u"parts"_s, save->stored.parts}};
    QStringSetting::set(
        metadataPath(save->stored.userID),
        QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    pajlada::Settings::SettingManager::gRemoveSetting(
        cleanupPath(save->stored.revision));
    if (!old.revision.isEmpty())
    {
        QStringSetting::set(cleanupPath(old.revision),
                            cleanupMetadata(old.userID, old.parts));
    }
    if (!save->existing)
    {
        this->selected_ = save->stored.userID;
    }

    if (!this->store_.saveSettings())
    {
        restoreMetadata(metadataPath(save->stored.userID), oldMetadata);
        if (!old.revision.isEmpty())
        {
            restoreMetadata(cleanupPath(old.revision), oldCleanup);
        }
        this->selected_ = oldSelected;
        this->queueCleanup(save->stored);
        save->callback(makeUnexpected(SETTINGS_ERROR));
        return;
    }
    this->stored_.insert(save->stored.userID, save->stored);
    if (!save->existing)
    {
        if (auto previous = this->findByUserID(save->stored.userID))
        {
            previous->clearSession();
            this->accounts.removeFirstMatching(
                [&previous](const auto &item) {
                    return item == previous;
                },
                this);
        }
        auto account = std::make_shared<TikTokAccount>(std::move(save->data));
        this->accounts.insert(account);
        this->selectAccount(account->userID());
        this->userListUpdated.invoke();
        this->credentialsChanged.invoke();
    }

    this->reportCredentialError({});
    if (!old.revision.isEmpty())
    {
        this->queueCleanup(old);
    }
    save->callback({});
}

void TikTokAccountManager::queueCleanup(const Stored &stored)
{
    if (stored.revision.isEmpty() || stored.parts <= 0)
    {
        return;
    }
    QStringSetting::set(cleanupPath(stored.revision),
                        cleanupMetadata(stored.userID, stored.parts));
    if (!this->store_.saveSettings())
    {
        this->reportCredentialError(SETTINGS_ERROR);
        return;
    }
    this->cleanupNext(stored, 0);
}

void TikTokAccountManager::cleanupNext(const Stored &stored, int index)
{
    if (this->stored_.value(stored.userID).revision == stored.revision)
    {
        return;
    }
    if (index == stored.parts)
    {
        pajlada::Settings::SettingManager::gRemoveSetting(
            cleanupPath(stored.revision));
        if (!this->store_.saveSettings())
        {
            QStringSetting::set(cleanupPath(stored.revision),
                                cleanupMetadata(stored.userID, stored.parts));
            this->reportCredentialError(SETTINGS_ERROR);
        }
        return;
    }
    this->store_.remove(
        credentialKey(stored.userID, stored.revision, index),
        [guard = QPointer(this), stored, index](ExpectedStr<void> result) {
            if (!guard)
            {
                return;
            }
            if (!result)
            {
                guard->reportCredentialError(
                    u"Could not remove an old TikTok session from storage. Moltorino will retry at startup."_s);
                return;
            }
            guard->cleanupNext(stored, index + 1);
        });
}

void TikTokAccountManager::removed(
    const std::shared_ptr<TikTokAccount> &account)
{
    ++this->generations_[account->userID()];
    if (auto *timer = this->refreshTimers_.take(account->userID()))
    {
        timer->stop();
        timer->deleteLater();
    }
    const auto stored = this->stored_.value(account->userID());
    const auto oldMetadata =
        QStringSetting::get(metadataPath(account->userID()));
    const auto oldSelected = this->selected_.getValue();
    const auto oldCleanup =
        stored.revision.isEmpty()
            ? QString{}
            : QStringSetting::get(cleanupPath(stored.revision));
    if (this->current_ == account)
    {
        this->selected_ = QString{};
    }
    pajlada::Settings::SettingManager::gRemoveSetting(
        metadataPath(account->userID()));
    if (!stored.revision.isEmpty())
    {
        QStringSetting::set(cleanupPath(stored.revision),
                            cleanupMetadata(stored.userID, stored.parts));
    }
    if (!this->store_.saveSettings())
    {
        restoreMetadata(metadataPath(account->userID()), oldMetadata);
        if (!stored.revision.isEmpty())
        {
            restoreMetadata(cleanupPath(stored.revision), oldCleanup);
        }
        this->selected_ = oldSelected;
        this->reportCredentialError(SETTINGS_ERROR);

        QTimer::singleShot(
            0, this,
            [this, account, stored,
             generation = this->generations_.value(account->userID())] {
                if (this->generations_.value(account->userID()) != generation ||
                    this->findByUserID(account->userID()))
                {
                    return;
                }
                this->accounts.insert(account, -1, this);
                if (account->isLoadingCredentials())
                {
                    this->readNext(account, stored, 0, {});
                }
                this->userListUpdated.invoke();
            });
        return;
    }
    this->stored_.remove(account->userID());
    account->clearSession();
    if (this->current_ == account)
    {
        this->selectAccount({});
    }
    if (!stored.revision.isEmpty())
    {
        this->cleanupNext(stored, 0);
    }
    this->userListUpdated.invoke();
    this->credentialsChanged.invoke();
}

void TikTokAccountManager::selectAccount(const QString &userID)
{
    auto account = this->findByUserID(userID);
    if (!account)
    {
        account = this->anonymous_;
    }
    this->selected_ = account->userID();
    if (this->current_ != account)
    {
        this->current_ = std::move(account);
        this->currentChanged.invoke();
    }
}

std::shared_ptr<TikTokAccount> TikTokAccountManager::current() const
{
    return this->current_;
}

std::shared_ptr<TikTokAccount> TikTokAccountManager::findByUserID(
    const QString &userID) const
{
    for (const auto &account : this->accounts)
    {
        if (account->userID() == userID)
        {
            return account;
        }
    }
    return {};
}

bool TikTokAccountManager::isLoggedIn() const
{
    return this->current_ && this->current_->hasCredentials();
}

void TikTokAccountManager::requireLogin(
    const std::shared_ptr<TikTokAccount> &account)
{
    if (account && this->findByUserID(account->userID()) == account)
    {
        if (!this->pendingLogins_.contains(account->userID()))
        {
            ++this->generations_[account->userID()];
        }
        if (auto *timer = this->refreshTimers_.value(account->userID()))
        {
            timer->stop();
        }
        account->clearSession();
        if (this->stored_.contains(account->userID()))
        {
            const auto path = metadataPath(account->userID());
            auto metadata =
                QJsonDocument::fromJson(QStringSetting::get(path).toUtf8())
                    .object();
            metadata.insert(u"needsLogin"_s, true);
            QStringSetting::set(
                path, QString::fromUtf8(QJsonDocument(metadata).toJson(
                          QJsonDocument::Compact)));
            if (!this->store_.saveSettings())
            {
                this->reportCredentialError(SETTINGS_ERROR);
            }
        }
        this->credentialsChanged.invoke();
    }
}

const QString &TikTokAccountManager::credentialError() const
{
    return this->credentialError_;
}

void TikTokAccountManager::reportCredentialError(const QString &text)
{
    this->credentialError_ = text;
    this->credentialErrorChanged.invoke();
}
}
