#include "common/AccountCredentials.hpp"

#include "common/ChatterinoSetting.hpp"
#include "singletons/Settings.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

namespace chatterino {
namespace {
using namespace Qt::Literals::StringLiterals;
constexpr auto MISSING = "Credential was not found.";
constexpr auto SAVE_ERROR = "Could not save account credentials. Check "
                            "available disk space and try again.";

std::string credentialPath(const QString &key)
{
    return "/accountCredentials/" +
           key.toUtf8()
               .toBase64(QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals)
               .toStdString();
}

QString credentialEntry(const QString &value, bool legacy)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{u"value"_s, value}, {u"legacy"_s, legacy}})
            .toJson(QJsonDocument::Compact));
}

void restoreCredential(const QString &key, const QString &value)
{
    if (value.isEmpty())
    {
        pajlada::Settings::SettingManager::gRemoveSetting(credentialPath(key));
    }
    else
    {
        QStringSetting::set(credentialPath(key), value);
    }
}
}

AccountCredentials::AccountCredentials(Backend backend)
    : backend_(std::move(backend))
{
    if (!this->backend_.save)
    {
        this->backend_.save = [] {
            return getSettings()->requestSave() ==
                   pajlada::Settings::SettingManager::SaveResult::Success;
        };
    }
    if (!this->backend_.readLegacy)
    {
        this->backend_.readLegacy = SecureCredentials::read;
    }
    if (!this->backend_.removeLegacy)
    {
        this->backend_.removeLegacy = SecureCredentials::remove;
    }
}

void AccountCredentials::enqueue(const QString &key,
                                 std::function<void()> operation)
{
    auto &queue = this->pending_[key];
    queue.push_back(std::move(operation));
    if (queue.size() == 1)
    {
        QTimer::singleShot(0, this, [this, key] {
            auto operation = this->pending_[key].front();
            operation();
        });
    }
}

void AccountCredentials::finish(const QString &key,
                                std::function<void()> callback)
{
    auto &queue = this->pending_[key];
    queue.pop_front();
    const bool more = !queue.empty();
    if (!more)
    {
        this->pending_.remove(key);
    }

    if (more)
    {
        QTimer::singleShot(0, this, [this, key] {
            auto operation = this->pending_[key].front();
            operation();
        });
    }
    callback();
}

void AccountCredentials::clearLegacy(const QString &key, WriteCallback callback)
{
    const bool running = this->legacyCleanups_.contains(key);
    auto &callbacks = this->legacyCleanups_[key];
    if (callback)
    {
        callbacks.push_back(std::move(callback));
    }
    if (running)
    {
        return;
    }

    this->backend_.removeLegacy(key, [self = QPointer(this),
                                      key](ExpectedStr<void> result) {
        if (!self)
        {
            return;
        }
        if (result)
        {
            self->legacyPending_.remove(key);
            const auto before = QStringSetting::get(credentialPath(key));
            const auto json = QJsonDocument::fromJson(before.toUtf8()).object();

            if (json.value(u"legacy"_s).toBool() &&
                json.value(u"value"_s).isString())
            {
                const auto value = json.value(u"value"_s).toString();
                restoreCredential(key, value.isEmpty()
                                           ? QString{}
                                           : credentialEntry(value, false));
                if (!self->backend_.save())
                {
                    restoreCredential(key, before);
                    result = makeUnexpected(QString::fromLatin1(SAVE_ERROR));
                }
            }
        }
        const auto callbacks = self->legacyCleanups_.take(key);
        for (const auto &callback : callbacks)
        {
            if (!self)
            {
                break;
            }
            callback(result);
        }
    });
}

void AccountCredentials::read(const QString &key, ReadCallback callback)
{
    this->enqueue(key, [this, key, callback = std::move(callback)] {
        const auto stored = QStringSetting::get(credentialPath(key));
        auto done = [self = QPointer(this), key,
                     callback](ExpectedStr<QString> result) {
            if (self)
            {
                self->finish(key, [callback, result] {
                    callback(result);
                });
            }
        };
        if (!stored.isEmpty())
        {
            const auto json = QJsonDocument::fromJson(stored.toUtf8()).object();
            if (!json.value(u"value"_s).isString())
            {
                done(makeUnexpected(
                    u"The saved account credential is invalid. Reconnect this account."_s));
                return;
            }
            const auto value = json.value(u"value"_s).toString();
            const auto result = value.isEmpty()
                                    ? ExpectedStr<QString>(makeUnexpected(
                                          QString::fromLatin1(MISSING)))
                                    : ExpectedStr<QString>(value);
            if (json.value(u"legacy"_s).toBool())
            {
                this->clearLegacy(key);
            }
            done(result);
            return;
        }
        this->legacyPending_.insert(key);
        this->backend_.readLegacy(key, [self = QPointer(this), key,
                                        done](ExpectedStr<QString> result) {
            if (!self)
            {
                return;
            }
            if (!result || result->isEmpty())
            {
                if (!result && result.error() == QString::fromLatin1(MISSING))
                {
                    self->legacyPending_.remove(key);
                }
                done(std::move(result));
                return;
            }
            QStringSetting::set(credentialPath(key),
                                credentialEntry(*result, true));
            if (!self->backend_.save())
            {
                restoreCredential(key, {});
                done(result);
                return;
            }

            self->clearLegacy(key);
            done(result);
        });
    });
}

void AccountCredentials::write(const QString &key, const QString &value,
                               WriteCallback callback)
{
    this->enqueue(key, [this, key, value, callback = std::move(callback)] {
        const auto before = QStringSetting::get(credentialPath(key));

        const auto legacy = this->legacyPending_.contains(key) ||
                            QJsonDocument::fromJson(before.toUtf8())
                                .object()
                                .value(u"legacy"_s)
                                .toBool();
        QStringSetting::set(credentialPath(key),
                            credentialEntry(value, legacy));
        ExpectedStr<void> result;
        if (!this->backend_.save())
        {
            restoreCredential(key, before);
            result = makeUnexpected(QString::fromLatin1(SAVE_ERROR));
        }
        this->finish(key, [callback, result] {
            callback(result);
        });
    });
}

void AccountCredentials::remove(const QString &key, WriteCallback callback)
{
    this->enqueue(key, [this, key, callback = std::move(callback)] {
        const auto before = QStringSetting::get(credentialPath(key));
        const bool legacy =
            before.isEmpty() || QJsonDocument::fromJson(before.toUtf8())
                                    .object()
                                    .value(u"legacy"_s)
                                    .toBool();

        restoreCredential(key, legacy ? credentialEntry({}, true) : QString{});
        if (!this->backend_.save())
        {
            restoreCredential(key, before);
            this->finish(key, [callback] {
                callback(makeUnexpected(QString::fromLatin1(SAVE_ERROR)));
            });
            return;
        }
        if (legacy)
        {
            this->finish(key, [this, key, callback] {
                this->clearLegacy(key, callback);
            });
        }
        else
        {
            this->finish(key, [callback] {
                callback({});
            });
        }
    });
}

AccountCredentials &accountCredentials()
{
    static AccountCredentials store;
    return store;
}
}
