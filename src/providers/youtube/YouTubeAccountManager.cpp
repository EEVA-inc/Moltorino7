#include "providers/youtube/YouTubeAccountManager.hpp"

#include "common/AccountCredentials.hpp"
#include "common/ChatterinoSetting.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeCredentialStore.hpp"
#include "singletons/Settings.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep
#include "util/SharedPtrElementLess.hpp"

#include <pajlada/settings/settingmanager.hpp>
#include <QCryptographicHash>
#include <QDebug>
#include <QRegularExpression>
#include <QTimer>

#include <tuple>
#include <utility>

namespace {

void repairStoredCredential(
    const QString &channelID,
    const std::shared_ptr<chatterino::YouTubeAccount> &replacement)
{
    if (replacement && !replacement->refreshToken().isEmpty())
    {
        chatterino::writeYouTubeCredential(
            channelID, replacement->refreshToken(), [](auto) {});
    }
    else if (!replacement)
    {
        chatterino::accountCredentials().remove(
            chatterino::youtubeCredentialKey(channelID), [](auto) {});
    }
}

std::string credentialCleanupPath(const QString &channelID)
{
    return "/youtubeCredentialCleanup/" + channelID.toStdString();
}

std::string credentialCleanupErrorPath(const QString &channelID)
{
    return "/youtubeCredentialCleanupErrors/" + channelID.toStdString();
}

QString tokenFingerprint(const QString &token)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

constexpr auto UNKNOWN_TOKEN_FINGERPRINT = "unknown";
constexpr auto METADATA_REFRESH_AFTER_DAYS = 29;
constexpr auto METADATA_EXPIRES_AFTER_DAYS = 30;
constexpr auto METADATA_POLICY_INTERVAL_MS = 6 * 60 * 60 * 1000;

bool isMissingCredentialError(const QString &error)
{
    return error == QStringLiteral("Credential was not found.");
}

bool metadataTimestampIsUsable(const QDateTime &timestamp, const QDateTime &now)
{
    return timestamp.isValid() && timestamp <= now.addSecs(5 * 60);
}

bool metadataNeedsRefresh(const QDateTime &timestamp, const QDateTime &now)
{
    return !metadataTimestampIsUsable(timestamp, now) ||
           timestamp <= now.addDays(-METADATA_REFRESH_AFTER_DAYS);
}

bool metadataIsExpired(const QDateTime &timestamp, const QDateTime &now)
{
    return !metadataTimestampIsUsable(timestamp, now) ||
           timestamp <= now.addDays(-METADATA_EXPIRES_AFTER_DAYS);
}

bool isYouTubeChannelID(const QString &channelID)
{
    static const QRegularExpression pattern(
        QStringLiteral("^UC[A-Za-z0-9_-]{22}$"));
    return pattern.match(channelID).hasMatch();
}

}

namespace chatterino {

YouTubeAccountManager::YouTubeAccountManager()
    : accounts(SharedPtrElementLess<YouTubeAccount>{})
    , anonymous_(std::make_shared<YouTubeAccount>(YouTubeAccountData{
          .displayName = "Anonymous",
      }))
{
    this->metadataPolicyTimer_.setInterval(METADATA_POLICY_INTERVAL_MS);
    this->metadataPolicyTimer_.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&this->metadataPolicyTimer_, &QTimer::timeout, [this] {
        this->auditStoredMetadata();
    });

    std::ignore = this->accounts.itemRemoved.connect([this](const auto &args) {
        if (args.caller == this)
        {
            return;
        }
        if (this->current_ == args.item)
        {
            this->selectAccount({});
        }
        const auto channelID = args.item->channelID();
        const auto refreshToken = args.item->refreshToken();
        args.item->clearTokens();
        this->eraseStoredAccount(channelID, refreshToken);
        this->userListUpdated.invoke();
        std::ignore = getSettings()->requestSave();
    });
}

YouTubeAccountManager::~YouTubeAccountManager()
{
    this->metadataPolicyTimer_.stop();
    this->lifetimeToken_.reset();
    this->credentialLoads_.clear();
    this->credentialLoadWaiters_.clear();
    this->tokenRefreshes_.clear();
    this->metadataRefreshes_.clear();
}

void YouTubeAccountManager::load()
{
    if (this->loaded_)
    {
        return;
    }
    this->loaded_ = true;

    bool addedAny = false;
    bool removedMalformedMetadata = false;
    std::set<QString> malformedCredentialIDs;
    const auto keys =
        pajlada::Settings::SettingManager::getObjectKeys("/youtubeAccounts");
    for (const auto &key : keys)
    {
        if (key == "current")
        {
            continue;
        }

        const auto basePath = "/youtubeAccounts/" + key + "/";
        auto channelID = QStringSetting::get(basePath + "channelID").trimmed();
        auto handle =
            visibleYouTubeName(QStringSetting::get(basePath + "handle"));
        auto displayName =
            QStringSetting::get(basePath + "displayName").trimmed();
        auto avatarUrl = QStringSetting::get(basePath + "avatarUrl").trimmed();
        auto metadataRefreshedAt = QDateTime::fromString(
            QStringSetting::get(basePath + "metadataRefreshedAt"),
            Qt::ISODateWithMs);
        if (!metadataRefreshedAt.isValid())
        {
            metadataRefreshedAt = QDateTime::fromString(
                QStringSetting::get(basePath + "metadataRefreshedAt"),
                Qt::ISODate);
        }
        const bool migrateMetadataTimestamp = !metadataTimestampIsUsable(
            metadataRefreshedAt, QDateTime::currentDateTimeUtc());
        if (migrateMetadataTimestamp)
        {
            metadataRefreshedAt = QDateTime::currentDateTimeUtc();
        }
        if (!isYouTubeChannelID(channelID) || displayName.isEmpty())
        {
            const auto storedChannelID = QString::fromStdString(key).trimmed();
            QString credentialChannelID;
            if (isYouTubeChannelID(channelID))
            {
                credentialChannelID = channelID;
            }
            else if (isYouTubeChannelID(storedChannelID))
            {
                credentialChannelID = storedChannelID;
            }
            qWarning().noquote()
                << "Removing malformed stored YouTube account metadata for"
                << storedChannelID;
            pajlada::Settings::SettingManager::gRemoveSetting(
                "/youtubeAccounts/" + key);
            removedMalformedMetadata = true;
            if (!credentialChannelID.isEmpty())
            {
                malformedCredentialIDs.insert(credentialChannelID);
            }
            continue;
        }
        if (this->findByChannelID(channelID))
        {
            continue;
        }

        auto account = std::make_shared<YouTubeAccount>(YouTubeAccountData{
            .channelID = std::move(channelID),
            .handle = std::move(handle),
            .displayName = std::move(displayName),
            .avatarUrl = std::move(avatarUrl),
            .metadataRefreshedAt = metadataRefreshedAt.toUTC(),
        });
        this->accounts.insert(account);
        this->observeAccount(account);
        if (migrateMetadataTimestamp)
        {
            account->saveMetadata();
        }
        this->loadStoredCredentials(account);
        addedAny = true;
    }

    this->applyCurrent(this->currentChannelID_.getValue());
    this->currentChannelID_.connect([this](const QString &channelID) {
        this->applyCurrent(channelID);
    });

    if (addedAny)
    {
        this->userListUpdated.invoke();
    }

    this->retryPendingCredentialCleanup();
    for (const auto &channelID : malformedCredentialIDs)
    {
        if (!this->findByChannelID(channelID) &&
            !this->credentialCleanupGenerations_.contains(channelID))
        {
            this->beginCredentialCleanup(channelID, {});
        }
    }
    if (removedMalformedMetadata)
    {
        std::ignore = getSettings()->requestSave();
    }
    this->auditStoredMetadata();
    this->metadataPolicyTimer_.start();
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::addAccount(
    const YouTubeAccountData &data)
{
    YouTubeAccountData normalized = data;
    normalized.channelID = normalized.channelID.trimmed();
    normalized.handle = visibleYouTubeName(normalized.handle);
    normalized.displayName = normalized.displayName.trimmed();
    normalized.avatarUrl = normalized.avatarUrl.trimmed();
    if (normalized.metadataRefreshedAt.isValid())
    {
        normalized.metadataRefreshedAt = normalized.metadataRefreshedAt.toUTC();
    }
    else if (!normalized.accessToken.isEmpty() ||
             !normalized.refreshToken.isEmpty())
    {
        normalized.metadataRefreshedAt = QDateTime::currentDateTimeUtc();
    }
    if (normalized.channelID.isEmpty() || normalized.displayName.isEmpty())
    {
        return nullptr;
    }

    auto existing = this->findByChannelID(normalized.channelID);
    if (existing)
    {
        const bool replacesCredentials = !normalized.accessToken.isEmpty() ||
                                         !normalized.refreshToken.isEmpty() ||
                                         normalized.expiresAt.isValid();
        if (replacesCredentials)
        {
            this->accountsRequiringReconnect_.erase(normalized.channelID);
            this->finishTokenRefresh(
                normalized.channelID,
                makeUnexpected(
                    QStringLiteral("The YouTube account was reconnected.")));
        }
        const auto previousName = existing->displayName();
        auto metadata = normalized;
        metadata.accessToken.clear();
        metadata.refreshToken.clear();
        metadata.expiresAt = {};
        const bool metadataChanged = existing->update(metadata);
        if (replacesCredentials)
        {
            existing->replaceCredentials(normalized);
        }
        existing->saveMetadata();
        if (!normalized.refreshToken.isEmpty())
        {
            this->clearPendingCredentialCleanup(normalized.channelID);
        }

        if (metadataChanged && previousName != existing->displayName())
        {
            this->accounts.removeFirstMatching(
                [&existing](const auto &item) {
                    return item == existing;
                },
                this);
            this->accounts.insert(existing, -1, this);
        }

        this->userListUpdated.invoke();
        std::ignore = getSettings()->requestSave();
        return existing;
    }

    auto account = std::make_shared<YouTubeAccount>(std::move(normalized));
    this->accountsRequiringReconnect_.erase(account->channelID());
    account->saveMetadata();
    if (!account->refreshToken().isEmpty())
    {
        this->clearPendingCredentialCleanup(account->channelID());
    }
    this->accounts.insert(account);
    this->observeAccount(account);
    this->userListUpdated.invoke();
    std::ignore = getSettings()->requestSave();
    return account;
}

bool YouTubeAccountManager::removeAccount(const QString &channelID)
{
    auto account = this->findByChannelID(channelID);
    if (!account)
    {
        return false;
    }

    if (this->current_ == account)
    {
        this->selectAccount({});
    }

    const bool removed = this->accounts.removeFirstMatching(
        [&channelID](const auto &item) {
            return item->channelID() == channelID;
        },
        this);
    if (!removed)
    {
        return false;
    }

    const auto refreshToken = account->refreshToken();
    account->clearTokens();
    this->eraseStoredAccount(channelID, refreshToken);
    this->userListUpdated.invoke();
    std::ignore = getSettings()->requestSave();
    return true;
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::current() const
{
    return this->current_ ? this->current_ : this->anonymous_;
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::findByChannelID(
    const QString &channelID) const
{
    if (channelID.isEmpty())
    {
        return nullptr;
    }
    for (const auto &account : this->accounts.raw())
    {
        if (account->channelID() == channelID)
        {
            return account;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<YouTubeAccount>>
    YouTubeAccountManager::accountList() const
{
    return this->accounts.raw();
}

std::vector<std::pair<QString, QString>>
    YouTubeAccountManager::credentialCleanupErrors() const
{
    std::vector<std::pair<QString, QString>> errors;
    const auto pending = pajlada::Settings::SettingManager::getObjectKeys(
        "/youtubeCredentialCleanupErrors");
    errors.reserve(pending.size());
    for (const auto &key : pending)
    {
        auto error = QStringSetting::get(
            credentialCleanupErrorPath(QString::fromStdString(key)));
        if (!error.isEmpty())
        {
            errors.emplace_back(QString::fromStdString(key), std::move(error));
        }
    }
    return errors;
}

void YouTubeAccountManager::dismissCredentialCleanupErrors()
{
    const auto errors = pajlada::Settings::SettingManager::getObjectKeys(
        "/youtubeCredentialCleanupErrors");
    if (errors.empty())
    {
        return;
    }

    for (const auto &key : errors)
    {
        pajlada::Settings::SettingManager::gRemoveSetting(
            "/youtubeCredentialCleanupErrors/" + key);
    }
    std::ignore = getSettings()->requestSave();
    this->credentialCleanupChanged.invoke();
}

void YouTubeAccountManager::selectAccount(const QString &channelID)
{
    if (this->currentChannelID_.getValue() == channelID)
    {
        this->applyCurrent(channelID);
        return;
    }
    this->currentChannelID_ = channelID;
    std::ignore = getSettings()->requestSave();
}

bool YouTubeAccountManager::isLoggedIn() const
{
    const auto account = this->current();
    return !account->isAnonymous() && account->hasCredentials();
}

void YouTubeAccountManager::getAccessToken(AccessTokenCallback callback)
{
    if (!callback)
    {
        return;
    }

    this->getAccessTokenForAccount(this->current(), true, std::move(callback));
}

void YouTubeAccountManager::getAccessTokenForAccount(
    const std::shared_ptr<YouTubeAccount> &account, bool requireCurrentAccount,
    AccessTokenCallback callback)
{
    if (!callback)
    {
        return;
    }

    if (!account || this->findByChannelID(account->channelID()) != account)
    {
        callback(makeUnexpected(
            QStringLiteral("The YouTube account is no longer available.")));
        return;
    }
    if (account->isAnonymous())
    {
        callback(makeUnexpected(
            QStringLiteral("Connect a YouTube account to continue.")));
        return;
    }

    if (this->accountsRequiringReconnect_.contains(account->channelID()))
    {
        callback(makeUnexpected(QStringLiteral(
            "Reconnect this YouTube account in Settings > Accounts.")));
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!account->accessToken().isEmpty() && account->expiresAt().isValid() &&
        account->expiresAt() > now.addSecs(5 * 60))
    {
        callback(account->accessToken());
        return;
    }

    if (!YouTubeApi::isOAuthConfigured())
    {
        callback(makeUnexpected(QStringLiteral(
            "YouTube login is not configured in this build. "
            "The builder must supply Google Desktop OAuth credentials.")));
        return;
    }

    const auto channelID = account->channelID();
    if (this->credentialLoads_.contains(channelID))
    {
        this->credentialLoadWaiters_[channelID].emplace_back(PendingAccessToken{
            .account = account,
            .selectionGeneration = this->selectionGeneration_,
            .requireCurrentAccount = requireCurrentAccount,
            .callback = std::move(callback),
        });
        return;
    }

    const auto refreshToken = account->refreshToken();
    if (refreshToken.isEmpty())
    {
        callback(makeUnexpected(
            QStringLiteral("This YouTube account needs to be reconnected.")));
        return;
    }

    auto existingRefresh = this->tokenRefreshes_.find(channelID);
    if (existingRefresh != this->tokenRefreshes_.end() &&
        existingRefresh->second.account.lock() != account)
    {
        this->finishTokenRefresh(
            channelID, makeUnexpected(QStringLiteral(
                           "The YouTube account changed while signing in.")));
        if (requireCurrentAccount && this->current() != account)
        {
            callback(makeUnexpected(
                QStringLiteral("The selected YouTube account changed.")));
            return;
        }
    }

    auto &state = this->tokenRefreshes_[channelID];
    state.callbacks.emplace_back(PendingAccessToken{
        .account = account,
        .selectionGeneration = this->selectionGeneration_,
        .requireCurrentAccount = requireCurrentAccount,
        .callback = std::move(callback),
    });
    if (state.inFlight)
    {
        return;
    }
    state.inFlight = true;
    state.account = account;
    const auto generation = ++this->nextTokenRefreshGeneration_;
    state.generation = generation;

    std::weak_ptr<YouTubeAccount> weakAccount = account;
    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    auto refreshed = [this, lifetime, channelID, generation,
                      weakAccount = std::move(weakAccount)](
                         Expected<YouTubeTokenResponse, YouTubeApiError>
                             result) mutable {
        if (lifetime.expired())
        {
            return;
        }
        auto account = weakAccount.lock();
        if (!account || this->findByChannelID(channelID) != account ||
            !this->isTokenRefreshCurrent(channelID, generation, account))
        {
            this->finishTokenRefresh(
                channelID, generation,
                makeUnexpected(QStringLiteral(
                    "The YouTube account was removed while signing in.")));
            return;
        }
        if (!result.has_value())
        {
            if (result.error().isInvalidGrant())
            {
                this->accountsRequiringReconnect_.insert(channelID);
                account->clearTokens();
                this->userListUpdated.invoke();
                this->finishTokenRefresh(
                    channelID, generation,
                    makeUnexpected(QStringLiteral(
                        "Google could not refresh this YouTube login. "
                        "Reconnect the account in Settings > Accounts.")));
                return;
            }
            this->finishTokenRefresh(
                channelID, generation,
                makeUnexpected(
                    QStringLiteral("Could not refresh YouTube login: ") +
                    result.error().message));
            return;
        }
        if (result->accessToken.isEmpty())
        {
            this->finishTokenRefresh(
                channelID, generation,
                makeUnexpected(QStringLiteral(
                    "Google returned an empty YouTube access token.")));
            return;
        }

        const bool refreshTokenRotated =
            !result->refreshToken.isEmpty() &&
            result->refreshToken != account->refreshToken();
        const auto rotatedRefreshToken = result->refreshToken;
        auto applyTokens = [this, lifetime, channelID, generation, weakAccount,
                            tokens = std::move(*result)]() mutable {
            if (lifetime.expired())
            {
                return;
            }
            auto account = weakAccount.lock();
            if (!account || this->findByChannelID(channelID) != account ||
                !this->isTokenRefreshCurrent(channelID, generation, account))
            {
                repairStoredCredential(channelID,
                                       this->findByChannelID(channelID));
                this->finishTokenRefresh(
                    channelID, generation,
                    makeUnexpected(QStringLiteral(
                        "The YouTube account was removed while signing in.")));
                return;
            }

            account->updateTokens(tokens.accessToken, tokens.refreshToken,
                                  tokens.expiresAt);
            this->finishTokenRefresh(channelID, generation,
                                     account->accessToken());
        };

        if (refreshTokenRotated)
        {
            writeYouTubeCredential(
                channelID, rotatedRefreshToken,
                [this, lifetime, channelID, generation, weakAccount,
                 applyTokens =
                     std::move(applyTokens)](ExpectedStr<void> stored) mutable {
                    if (lifetime.expired())
                    {
                        return;
                    }
                    const auto account = weakAccount.lock();
                    if (!account || !this->isTokenRefreshCurrent(
                                        channelID, generation, account))
                    {
                        repairStoredCredential(
                            channelID, this->findByChannelID(channelID));
                        return;
                    }
                    if (!stored.has_value())
                    {
                        this->finishTokenRefresh(
                            channelID, generation,
                            makeUnexpected(
                                QStringLiteral(
                                    "Could not safely store refreshed YouTube "
                                    "credentials: ") +
                                stored.error()));
                        return;
                    }
                    applyTokens();
                });
            return;
        }

        applyTokens();
    };

    YouTubeApi::refreshAccessToken(refreshToken, std::move(refreshed));
}

bool YouTubeAccountManager::invalidateAccessToken(
    const QString &channelID, const QString &rejectedAccessToken)
{
    const auto account = this->findByChannelID(channelID);
    if (!account || rejectedAccessToken.isEmpty() ||
        account->accessToken() != rejectedAccessToken)
    {
        return false;
    }

    account->updateTokens({}, {}, {});
    return true;
}

bool YouTubeAccountManager::requireReconnect(const QString &channelID,
                                             const QString &rejectedAccessToken)
{
    const auto account = this->findByChannelID(channelID);
    if (!account || rejectedAccessToken.isEmpty() ||
        account->accessToken() != rejectedAccessToken)
    {
        return false;
    }

    this->accountsRequiringReconnect_.insert(channelID);
    account->clearTokens();
    this->userListUpdated.invoke();
    return true;
}

void YouTubeAccountManager::applyCurrent(const QString &channelID)
{
    auto next = this->findByChannelID(channelID);
    if (!next)
    {
        next = this->anonymous_;
    }
    if (this->current_ == next)
    {
        this->refreshCurrentMetadataIfNeeded();
        return;
    }
    this->current_ = std::move(next);
    ++this->selectionGeneration_;
    this->currentChanged.invoke();
    this->refreshCurrentMetadataIfNeeded();
}

void YouTubeAccountManager::eraseStoredAccount(const QString &channelID,
                                               const QString &refreshToken)
{
    if (channelID.isEmpty())
    {
        return;
    }
    pajlada::Settings::SettingManager::gRemoveSetting("/youtubeAccounts/" +
                                                      channelID.toStdString());
    this->accountsRequiringReconnect_.erase(channelID);

    this->finishTokenRefresh(
        channelID,
        makeUnexpected(QStringLiteral("The YouTube account was removed.")));
    this->credentialLoads_.erase(channelID);
    this->finishCredentialLoad(
        channelID, QStringLiteral("The YouTube account was removed."));

    this->beginCredentialCleanup(channelID, refreshToken);
}

void YouTubeAccountManager::beginCredentialCleanup(const QString &channelID,
                                                   const QString &refreshToken)
{
    if (channelID.isEmpty())
    {
        return;
    }

    auto fingerprint =
        refreshToken.isEmpty()
            ? QStringSetting::get(credentialCleanupPath(channelID))
            : tokenFingerprint(refreshToken);
    if (fingerprint.isEmpty())
    {
        fingerprint = QString::fromLatin1(UNKNOWN_TOKEN_FINGERPRINT);
    }
    QStringSetting::set(credentialCleanupPath(channelID), fingerprint);
    std::ignore = getSettings()->requestSave();

    const auto cleanupGeneration = ++this->nextCredentialCleanupGeneration_;
    this->credentialCleanupGenerations_[channelID] = cleanupGeneration;

    const auto replacement = this->findByChannelID(channelID);
    if (replacement && replacement->hasCredentials())
    {
        this->clearPendingCredentialCleanup(channelID);
        return;
    }

    if (!refreshToken.isEmpty())
    {
        this->revokeRemovedToken(channelID, refreshToken, cleanupGeneration);
        return;
    }

    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    accountCredentials().read(
        youtubeCredentialKey(channelID),
        [this, lifetime, channelID, cleanupGeneration,
         fingerprint](ExpectedStr<QString> result) mutable {
            if (lifetime.expired())
            {
                return;
            }
            if (!this->isCredentialCleanupCurrent(channelID, cleanupGeneration))
            {
                return;
            }
            const auto replacement = this->findByChannelID(channelID);
            if (replacement && replacement->hasCredentials())
            {
                this->clearPendingCredentialCleanup(channelID);
                return;
            }
            if (result && !result->isEmpty())
            {
                const auto credential = parseYouTubeCredential(*result);
                if (!credential)
                {
                    this->recordCredentialCleanupFailure(
                        channelID,
                        QStringLiteral(
                            "Could not read the saved YouTube credential: ") +
                            credential.error());
                    return;
                }
                const auto storedFingerprint = tokenFingerprint(*credential);
                if (fingerprint !=
                        QString::fromLatin1(UNKNOWN_TOKEN_FINGERPRINT) &&
                    fingerprint != storedFingerprint)
                {
                    this->clearPendingCredentialCleanup(channelID);
                    return;
                }
                fingerprint = storedFingerprint;
                QStringSetting::set(credentialCleanupPath(channelID),
                                    fingerprint);
                this->revokeRemovedToken(channelID, *credential,
                                         cleanupGeneration);
                return;
            }
            if (!result && !isMissingCredentialError(result.error()))
            {
                qWarning().noquote()
                    << "Could not read the YouTube credential before cleanup "
                       "for"
                    << channelID << ':' << result.error();
                this->recordCredentialCleanupFailure(
                    channelID,
                    QStringLiteral("Could not read the saved Google token: ") +
                        result.error());
                return;
            }

            this->credentialCleanupGenerations_.erase(channelID);
            pajlada::Settings::SettingManager::gRemoveSetting(
                credentialCleanupPath(channelID));
            this->recordCredentialCleanupFailure(
                channelID,
                QStringLiteral(
                    "The saved token is missing, so Google revocation could "
                    "not be verified. Remove Moltorino from Google Account "
                    "connections manually."));
        });
}

void YouTubeAccountManager::removeStoredCredential(
    const QString &channelID, std::uint64_t cleanupGeneration, int attempt)
{
    if (!this->isCredentialCleanupCurrent(channelID, cleanupGeneration))
    {
        return;
    }
    const auto replacement = this->findByChannelID(channelID);
    if (replacement && replacement->hasCredentials())
    {
        this->clearPendingCredentialCleanup(channelID);
        return;
    }

    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    accountCredentials().remove(
        youtubeCredentialKey(channelID),
        [this, lifetime, channelID, cleanupGeneration,
         attempt](ExpectedStr<void> result) {
            if (lifetime.expired())
            {
                return;
            }
            if (!this->isCredentialCleanupCurrent(channelID, cleanupGeneration))
            {
                const auto replacement = this->findByChannelID(channelID);
                if (replacement && !replacement->refreshToken().isEmpty())
                {
                    writeYouTubeCredential(
                        channelID, replacement->refreshToken(), [](auto) {});
                }
                return;
            }
            if (result)
            {
                const auto replacement = this->findByChannelID(channelID);
                if (replacement && !replacement->refreshToken().isEmpty())
                {
                    writeYouTubeCredential(
                        channelID, replacement->refreshToken(), [](auto) {});
                }
                this->clearPendingCredentialCleanup(channelID);
                return;
            }

            if (attempt < 2)
            {
                const auto delay = attempt == 0 ? 1000 : 5000;
                QTimer::singleShot(
                    delay,
                    [this, lifetime, channelID, cleanupGeneration, attempt] {
                        if (!lifetime.expired() &&
                            this->isCredentialCleanupCurrent(channelID,
                                                             cleanupGeneration))
                        {
                            this->removeStoredCredential(
                                channelID, cleanupGeneration, attempt + 1);
                        }
                    });
                return;
            }

            qWarning().noquote()
                << "Failed to remove YouTube credentials for" << channelID
                << "after retries:" << result.error();
            this->recordCredentialCleanupFailure(
                channelID,
                QStringLiteral("Could not remove the saved Google token: ") +
                    result.error());
        });
}

void YouTubeAccountManager::revokeRemovedToken(const QString &channelID,
                                               const QString &token,
                                               std::uint64_t cleanupGeneration,
                                               int attempt)
{
    if (token.isEmpty() ||
        !this->isCredentialCleanupCurrent(channelID, cleanupGeneration))
    {
        return;
    }
    const auto replacement = this->findByChannelID(channelID);
    if (replacement && replacement->hasCredentials())
    {
        this->clearPendingCredentialCleanup(channelID);
        return;
    }
    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    YouTubeApi::revokeToken(token, [this, lifetime, channelID, token,
                                    cleanupGeneration, attempt](
                                       Expected<void, YouTubeApiError> result) {
        if (lifetime.expired() ||
            !this->isCredentialCleanupCurrent(channelID, cleanupGeneration))
        {
            return;
        }
        const auto replacement = this->findByChannelID(channelID);
        if (replacement && replacement->hasCredentials())
        {
            this->clearPendingCredentialCleanup(channelID);
            return;
        }

        if (result ||
            result.error().code.compare(QStringLiteral("invalid_token"),
                                        Qt::CaseInsensitive) == 0)
        {
            this->removeStoredCredential(channelID, cleanupGeneration);
            return;
        }
        if (attempt < 2)
        {
            const auto delay = attempt == 0 ? 1000 : 5000;
            QTimer::singleShot(delay, [this, lifetime, channelID, token,
                                       cleanupGeneration, attempt] {
                if (!lifetime.expired() && this->isCredentialCleanupCurrent(
                                               channelID, cleanupGeneration))
                {
                    this->revokeRemovedToken(channelID, token,
                                             cleanupGeneration, attempt + 1);
                }
            });
            return;
        }
        qWarning().noquote()
            << "Google token revocation failed for removed YouTube "
               "account"
            << channelID << "with HTTP status" << result.error().httpStatus
            << "and reason" << result.error().reason;
        this->recordCredentialCleanupFailure(
            channelID,
            QStringLiteral(
                "Google did not confirm token revocation: %1. The token "
                "remains saved locally and "
                "Moltorino will retry next startup.")
                .arg(result.error().message));
    });
}

void YouTubeAccountManager::recordCredentialCleanupFailure(
    const QString &channelID, const QString &error)
{
    QStringSetting::set(credentialCleanupErrorPath(channelID), error);
    std::ignore = getSettings()->requestSave();
    this->credentialCleanupChanged.invoke();
}

void YouTubeAccountManager::retryPendingCredentialCleanup()
{
    const auto pending = pajlada::Settings::SettingManager::getObjectKeys(
        "/youtubeCredentialCleanup");
    for (const auto &key : pending)
    {
        this->beginCredentialCleanup(QString::fromStdString(key), {});
    }
}

void YouTubeAccountManager::clearPendingCredentialCleanup(
    const QString &channelID)
{
    this->credentialCleanupGenerations_.erase(channelID);
    pajlada::Settings::SettingManager::gRemoveSetting(
        credentialCleanupPath(channelID));
    pajlada::Settings::SettingManager::gRemoveSetting(
        credentialCleanupErrorPath(channelID));
    std::ignore = getSettings()->requestSave();
    this->credentialCleanupChanged.invoke();
}

bool YouTubeAccountManager::isCredentialCleanupCurrent(
    const QString &channelID, std::uint64_t cleanupGeneration) const
{
    const auto it = this->credentialCleanupGenerations_.find(channelID);
    return it != this->credentialCleanupGenerations_.end() &&
           it->second == cleanupGeneration;
}

void YouTubeAccountManager::auditStoredMetadata()
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto storedAccounts = this->accounts.raw();
    for (const auto &account : storedAccounts)
    {
        if (!account || account->isAnonymous() ||
            !metadataNeedsRefresh(account->metadataRefreshedAt(), now))
        {
            continue;
        }

        this->refreshMetadata(account);
    }
}

void YouTubeAccountManager::refreshCurrentMetadataIfNeeded()
{
    const auto account = this->current();
    if (!account || account->isAnonymous() ||
        (!account->handle().isEmpty() &&
         !metadataNeedsRefresh(account->metadataRefreshedAt(),
                               QDateTime::currentDateTimeUtc())))
    {
        return;
    }
    this->refreshMetadata(account);
}

void YouTubeAccountManager::refreshMetadata(
    const std::shared_ptr<YouTubeAccount> &account)
{
    if (!YouTubeApi::isOAuthConfigured() || !account ||
        account->isAnonymous() ||
        this->accountsRequiringReconnect_.contains(account->channelID()) ||
        this->findByChannelID(account->channelID()) != account ||
        !this->metadataRefreshes_.insert(account->channelID()).second)
    {
        return;
    }

    const auto channelID = account->channelID();
    std::weak_ptr<YouTubeAccount> weakAccount = account;
    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    this->getAccessTokenForAccount(
        account, false,
        [this, lifetime, channelID, weakAccount = std::move(weakAccount)](
            ExpectedStr<QString> token) mutable {
            if (lifetime.expired())
            {
                return;
            }
            auto account = weakAccount.lock();
            if (!account || this->findByChannelID(channelID) != account)
            {
                this->metadataRefreshes_.erase(channelID);
                return;
            }
            if (!token)
            {
                this->failMetadataRefresh(account, token.error());
                return;
            }
            this->fetchMetadata(account, *token);
        });
}

void YouTubeAccountManager::fetchMetadata(
    const std::shared_ptr<YouTubeAccount> &account, const QString &accessToken,
    bool retriedUnauthorized)
{
    if (!account || this->findByChannelID(account->channelID()) != account)
    {
        return;
    }

    const auto channelID = account->channelID();
    std::weak_ptr<YouTubeAccount> weakAccount = account;
    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    auto identityLoaded =
        [this, lifetime, channelID, accessToken, retriedUnauthorized,
         weakAccount = std::move(weakAccount)](
            Expected<YouTubeOwnChannel, YouTubeApiError> identity) mutable {
            if (lifetime.expired())
            {
                return;
            }
            auto account = weakAccount.lock();
            if (!account || this->findByChannelID(channelID) != account)
            {
                this->metadataRefreshes_.erase(channelID);
                return;
            }
            if (!identity)
            {
                if (!retriedUnauthorized && identity.error().isUnauthorized() &&
                    this->invalidateAccessToken(channelID, accessToken))
                {
                    this->getAccessTokenForAccount(
                        account, false,
                        [this, lifetime, channelID,
                         weakAccount = std::move(weakAccount)](
                            ExpectedStr<QString> refreshedToken) mutable {
                            if (lifetime.expired())
                            {
                                return;
                            }
                            auto account = weakAccount.lock();
                            if (!account ||
                                this->findByChannelID(channelID) != account)
                            {
                                this->metadataRefreshes_.erase(channelID);
                                return;
                            }
                            if (!refreshedToken)
                            {
                                this->failMetadataRefresh(
                                    account, refreshedToken.error());
                                return;
                            }
                            this->fetchMetadata(account, *refreshedToken, true);
                        });
                    return;
                }
                if (retriedUnauthorized && identity.error().isUnauthorized() &&
                    this->requireReconnect(channelID, accessToken))
                {
                    this->failMetadataRefresh(
                        account,
                        QStringLiteral(
                            "YouTube rejected the refreshed authorization. "
                            "Reconnect this account in Settings."));
                    return;
                }
                this->failMetadataRefresh(account, identity.error().message);
                return;
            }
            if (identity->channelID.trimmed() != channelID ||
                identity->displayName.trimmed().isEmpty())
            {
                this->failMetadataRefresh(
                    account,
                    QStringLiteral(
                        "YouTube returned a different or unusable channel "
                        "identity."));
                return;
            }
            this->applyMetadataRefresh(account, identity->handle.trimmed(),
                                       identity->displayName.trimmed(),
                                       identity->avatarUrl.trimmed());
        };
    YouTubeApi::getOwnChannelAuthenticated(accessToken,
                                           std::move(identityLoaded));
}

void YouTubeAccountManager::applyMetadataRefresh(
    const std::shared_ptr<YouTubeAccount> &account, const QString &handle,
    const QString &displayName, const QString &avatarUrl)
{
    if (!account || this->findByChannelID(account->channelID()) != account)
    {
        return;
    }
    this->metadataRefreshes_.erase(account->channelID());

    const auto previousName = account->displayName();
    const bool metadataChanged = account->update(YouTubeAccountData{
        .channelID = account->channelID(),
        .handle = visibleYouTubeName(handle),
        .displayName = displayName,
        .avatarUrl = avatarUrl,
        .metadataRefreshedAt = QDateTime::currentDateTimeUtc(),
    });
    account->saveMetadata();

    if (metadataChanged && previousName != account->displayName())
    {
        this->accounts.removeFirstMatching(
            [&account](const auto &item) {
                return item == account;
            },
            this);
        this->accounts.insert(account, -1, this);
    }
    this->userListUpdated.invoke();
    std::ignore = getSettings()->requestSave();
}

void YouTubeAccountManager::failMetadataRefresh(
    const std::shared_ptr<YouTubeAccount> &account, const QString &reason)
{
    if (!account)
    {
        return;
    }
    this->metadataRefreshes_.erase(account->channelID());
    qWarning().noquote() << "Could not refresh stored YouTube metadata for"
                         << account->channelID() << ':' << reason;

    if (this->findByChannelID(account->channelID()) == account &&
        !this->accountsRequiringReconnect_.contains(account->channelID()) &&
        metadataIsExpired(account->metadataRefreshedAt(),
                          QDateTime::currentDateTimeUtc()))
    {
        this->purgeExpiredMetadata(account, reason);
    }
}

void YouTubeAccountManager::purgeExpiredMetadata(
    const std::shared_ptr<YouTubeAccount> &account, const QString &reason)
{
    if (!account || this->findByChannelID(account->channelID()) != account ||
        !metadataIsExpired(account->metadataRefreshedAt(),
                           QDateTime::currentDateTimeUtc()))
    {
        return;
    }

    qWarning().noquote() << "Removing expired YouTube account metadata for"
                         << account->channelID() << "because" << reason;
    this->metadataRefreshes_.erase(account->channelID());
    this->removeAccount(account->channelID());
}

void YouTubeAccountManager::loadStoredCredentials(
    const std::shared_ptr<YouTubeAccount> &account)
{
    const auto channelID = account->channelID();
    this->credentialLoads_.insert(channelID);
    std::weak_ptr<YouTubeAccount> weakAccount = account;
    std::weak_ptr<char> lifetime = this->lifetimeToken_;
    accountCredentials().read(
        youtubeCredentialKey(channelID),
        [this, lifetime, channelID,
         weakAccount = std::move(weakAccount)](ExpectedStr<QString> result) {
            if (lifetime.expired())
            {
                return;
            }
            this->credentialLoads_.erase(channelID);
            auto account = weakAccount.lock();
            if (!account || this->findByChannelID(channelID) != account)
            {
                this->finishCredentialLoad(
                    channelID,
                    QStringLiteral(
                        "The YouTube account was removed while its saved "
                        "credentials were loading."));
                return;
            }

            if (!account->refreshToken().isEmpty())
            {
                this->finishCredentialLoad(channelID);
                return;
            }

            if (!result.has_value())
            {
                this->finishCredentialLoad(
                    channelID,
                    isMissingCredentialError(result.error())
                        ? QString{}
                        : QStringLiteral(
                              "Could not load this YouTube account from the "
                              "saved settings: ") +
                              result.error());
                return;
            }

            if (!result.value().isEmpty())
            {
                const auto credential = parseYouTubeCredential(result.value());
                if (!credential)
                {
                    this->finishCredentialLoad(
                        channelID,
                        QStringLiteral(
                            "Could not load this YouTube account from the "
                            "saved settings: ") +
                            credential.error());
                    return;
                }
                account->replaceCredentials(YouTubeAccountData{
                    .refreshToken = *credential,
                });
            }
            this->finishCredentialLoad(channelID);
        });
}

void YouTubeAccountManager::finishCredentialLoad(const QString &channelID,
                                                 const QString &error)
{
    const auto it = this->credentialLoadWaiters_.find(channelID);
    if (it == this->credentialLoadWaiters_.end())
    {
        return;
    }

    auto callbacks = std::move(it->second);
    this->credentialLoadWaiters_.erase(it);
    for (auto &pending : callbacks)
    {
        if (!pending.callback)
        {
            continue;
        }
        QString validationError;
        if (!this->validatePendingAccessToken(pending, channelID,
                                              validationError))
        {
            pending.callback(makeUnexpected(validationError));
            continue;
        }
        if (!error.isEmpty())
        {
            pending.callback(makeUnexpected(error));
            continue;
        }
        auto account = pending.account.lock();
        this->getAccessTokenForAccount(account, pending.requireCurrentAccount,
                                       std::move(pending.callback));
    }
}

void YouTubeAccountManager::observeAccount(
    const std::shared_ptr<YouTubeAccount> &account)
{
    std::weak_ptr<YouTubeAccount> weakAccount = account;
    this->signals_.managedConnect(
        account->credentialsUpdated,
        [this, weakAccount = std::move(weakAccount)] {
            const auto account = weakAccount.lock();
            if (account && this->current_ == account)
            {
                this->credentialsChanged.invoke();
                this->refreshCurrentMetadataIfNeeded();
            }
        });
}

void YouTubeAccountManager::finishTokenRefresh(const QString &channelID,
                                               ExpectedStr<QString> result)
{
    const auto it = this->tokenRefreshes_.find(channelID);
    if (it == this->tokenRefreshes_.end())
    {
        return;
    }

    auto callbacks = std::move(it->second.callbacks);
    this->tokenRefreshes_.erase(it);
    for (auto &pending : callbacks)
    {
        if (!pending.callback)
        {
            continue;
        }
        QString validationError;
        if (!this->validatePendingAccessToken(pending, channelID,
                                              validationError))
        {
            pending.callback(makeUnexpected(validationError));
            continue;
        }
        pending.callback(result);
    }
}

bool YouTubeAccountManager::validatePendingAccessToken(
    const PendingAccessToken &pending, const QString &channelID,
    QString &error) const
{
    const auto account = pending.account.lock();
    if (!account || account->channelID() != channelID ||
        this->findByChannelID(channelID) != account)
    {
        error = QStringLiteral("The YouTube account was removed.");
        return false;
    }
    if (pending.requireCurrentAccount &&
        (pending.selectionGeneration != this->selectionGeneration_ ||
         this->current() != account))
    {
        error = QStringLiteral("The selected YouTube account changed.");
        return false;
    }
    return true;
}

void YouTubeAccountManager::finishTokenRefresh(const QString &channelID,
                                               std::uint64_t generation,
                                               ExpectedStr<QString> result)
{
    const auto it = this->tokenRefreshes_.find(channelID);
    if (it == this->tokenRefreshes_.end() ||
        it->second.generation != generation)
    {
        return;
    }
    this->finishTokenRefresh(channelID, std::move(result));
}

bool YouTubeAccountManager::isTokenRefreshCurrent(
    const QString &channelID, std::uint64_t generation,
    const std::shared_ptr<YouTubeAccount> &account) const
{
    const auto it = this->tokenRefreshes_.find(channelID);
    return it != this->tokenRefreshes_.end() && it->second.inFlight &&
           it->second.generation == generation &&
           it->second.account.lock() == account;
}

}
