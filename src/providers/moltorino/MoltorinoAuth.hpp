#pragma once

#include <functional>
#include <optional>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

namespace chatterino {

class TwitchChannel;

struct MoltorinoAuthChannel {
    QString id;
    QString login;
    QString displayName;
};

struct MoltorinoAuthAccount {
    QString userId;
    QString login;
    QString displayName;
    QString token;
    QString clientId;
    bool enabled = true;
    bool valid = false;
    QString lastError;
    QString lastValidatedAt;
    bool moderatedChannelsManualRefreshOnly = false;
    QVector<MoltorinoAuthChannel> moderatedChannels;
    QVector<MoltorinoAuthChannel> verifiedEditorChannels;
};

struct MoltorinoAuthToken {
    QString token;
    QString userId;
    QString login;
    QString clientId;
    bool legacy = false;

    [[nodiscard]] bool hasToken() const
    {
        return !this->token.trimmed().isEmpty();
    }
};

struct MoltorinoAuthSummary {
    int accountCount = 0;
    int enabledAccountCount = 0;
    int disabledAccountCount = 0;
    int validAccountCount = 0;
    int invalidAccountCount = 0;
    int moderatedChannelCount = 0;
    bool hasLegacyToken = false;
    bool hasOnlyLegacyToken = false;
};

struct MoltorinoAuthRefreshResult {
    int total = 0;
    int valid = 0;
    int invalid = 0;
    int moderatedChannels = 0;
    QStringList errors;
};

enum class MoltorinoAuthRefreshMode {
    Automatic,
    Manual,
};

namespace MoltorinoAuth {

std::vector<MoltorinoAuthAccount> accounts();
MoltorinoAuthSummary summary();
QString legacyToken();
bool hasConfiguredAuth();

void addOrUpdateToken(
    const QString &token,
    std::function<void(MoltorinoAuthAccount)> successCallback,
    std::function<void(const QString &)> failureCallback);
bool setAccountEnabled(const QString &userId, const QString &token,
                       bool enabled);
void removeAccount(const QString &userId, const QString &token);
void refreshAccounts(
    MoltorinoAuthRefreshMode mode,
    std::function<void(MoltorinoAuthRefreshResult)> callback);
void rememberEditorChannel(const QString &token,
                           const MoltorinoAuthChannel &channel);
void forgetEditorChannel(const QString &token, const QString &channelId,
                         const QString &channelLogin);
void scheduleStartupRefresh();

MoltorinoAuthToken resolveModerationToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage = nullptr);
MoltorinoAuthToken resolveSavedBroadcasterToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage = nullptr);
MoltorinoAuthToken resolveBroadcasterToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage = nullptr);
MoltorinoAuthToken resolveSelectedUserToken(QString *errorMessage = nullptr);
MoltorinoAuthToken resolveCurrentUserToken(QString *errorMessage = nullptr);
MoltorinoAuthToken resolveReadToken(QString *errorMessage = nullptr);

QString authRequiredMessage(const QString &action);
QString authExpiredMessage(const QString &action);
QString normalizeAuthError(const QString &action, const QString &error);

}  // namespace MoltorinoAuth

}  // namespace chatterino
