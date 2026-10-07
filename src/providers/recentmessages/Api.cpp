// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Api.hpp"

#include "Application.hpp"
#include "common/Env.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/recentmessages/Impl.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QJsonArray>

#include <algorithm>

namespace {

const auto &LOG = chatterinoRecentMessages;

}

namespace chatterino::recentmessages {

using namespace recentmessages::detail;

const std::vector<Provider> &providers()
{
    static const std::vector<Provider> items{
        {
            .id = QStringLiteral("robotty"),
            .name = QStringLiteral("Robotty"),
            .urlTemplate = Env::get().recentMessagesApiUrl,
        },
        {
            .id = QStringLiteral("zneix"),
            .name = QStringLiteral("Zneix"),
            .urlTemplate = QStringLiteral(
                "https://recent-messages.zneix.eu/api/v2/recent-messages/%1"),
        },
        {
            .id = QStringLiteral("lilb"),
            .name = QStringLiteral("lilb"),
            .urlTemplate =
                QStringLiteral("https://rm.lilb.dev/api/v2/recent-messages/%1"),
        },
        {
            .id = QStringLiteral("zonian"),
            .name = QStringLiteral("Zonian"),
            .urlTemplate = QStringLiteral("https://logs.zonian.dev/rm/%1"),
        },
    };
    return items;
}

namespace {

constexpr int PROVIDER_TIMEOUT_MS = 5000;
constexpr qsizetype MAX_PROVIDER_RESPONSE_BYTES = 16 * 1024 * 1024;

std::vector<Provider> orderedProviders()
{
    auto ordered = providers();
    const auto preferred =
        getSettings()->recentMessagesProvider.getValue().trimmed().toLower();
    const auto preferredIt =
        std::find_if(ordered.begin(), ordered.end(), [&](const auto &provider) {
            return provider.id == preferred;
        });
    if (preferredIt != ordered.end())
    {
        std::rotate(ordered.begin(), preferredIt, preferredIt + 1);
    }
    return ordered;
}

class LoadRequest : public std::enable_shared_from_this<LoadRequest>
{
public:
    QString channelName;
    std::weak_ptr<Channel> channelPtr;
    ResultCallback onLoaded;
    ErrorCallback onError;
    int limit{};
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after;
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before;
    std::vector<Provider> providerQueue;
    std::vector<QJsonArray> messageBatches;
    size_t nextProvider{};
    QString lastError;

    void tryNextProvider()
    {
        if (isAppAboutToQuit() || this->channelPtr.expired())
        {
            return;
        }

        if (this->nextProvider >= this->providerQueue.size())
        {
            if (!this->messageBatches.empty())
            {
                this->finishWithMessages();
            }
            else
            {
                this->finishWithError();
            }
            return;
        }

        const auto provider = this->providerQueue[this->nextProvider++];
        const auto url =
            constructRecentMessagesUrl(provider.urlTemplate, this->channelName,
                                       this->limit, this->after, this->before);
        auto self = this->shared_from_this();

        qCDebug(LOG) << "Trying recent-message provider" << provider.name
                     << "for" << this->channelName;

        NetworkRequest(url)
            .timeout(PROVIDER_TIMEOUT_MS)
            .maximumResponseSize(MAX_PROVIDER_RESPONSE_BYTES)
            .onSuccess([self, provider](const NetworkResult &result) {
                self->handleSuccess(provider, result);
            })
            .onError([self, provider](const NetworkResult &result) {
                self->lastError = result.formatError();
                qCDebug(LOG) << "Recent-message provider" << provider.name
                             << "failed for" << self->channelName << ':'
                             << self->lastError;
                self->tryNextProvider();
            })
            .execute();
    }

private:
    void handleSuccess(const Provider &provider, const NetworkResult &result)
    {
        auto shared = this->channelPtr.lock();
        if (!shared || isAppAboutToQuit())
        {
            return;
        }

        auto root = result.parseJson();
        const auto messagesValue = root.value(QStringLiteral("messages"));
        if (!messagesValue.isArray())
        {
            this->lastError = QStringLiteral("invalid response");
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "returned an invalid response for"
                         << this->channelName;
            this->tryNextProvider();
            return;
        }
        const auto messageValues = messagesValue.toArray();
        if (!std::ranges::all_of(messageValues, [](const auto &messageValue) {
                return messageValue.isString();
            }))
        {
            this->lastError = QStringLiteral("invalid message data");
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "returned invalid message data for"
                         << this->channelName;
            this->tryNextProvider();
            return;
        }

        const auto errorCode =
            root.value(QStringLiteral("error_code")).toString();
        if (!errorCode.isEmpty() &&
            errorCode != QStringLiteral("channel_not_joined"))
        {
            this->lastError = errorCode;
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "returned an error for" << this->channelName << ':'
                         << errorCode;
            this->tryNextProvider();
            return;
        }
        if (errorCode == QStringLiteral("channel_not_joined") &&
            messageValues.isEmpty())
        {
            this->lastError = errorCode;
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "is not tracking" << this->channelName;
            this->tryNextProvider();
            return;
        }

        if (!messageValues.isEmpty() &&
            !hasParseableRecentMessages(messageValues))
        {
            this->lastError = QStringLiteral("unparseable message data");
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "returned no parseable IRC messages for"
                         << this->channelName;
            this->tryNextProvider();
            return;
        }
        if (errorCode == QStringLiteral("channel_not_joined"))
        {
            this->messageBatches.emplace_back(messageValues);
            this->lastError = errorCode;
            qCDebug(LOG) << "Recent-message provider" << provider.name
                         << "returned partial history for" << this->channelName
                         << "; trying a fallback";
            this->tryNextProvider();
            return;
        }

        qCDebug(LOG) << "Successfully loaded recent messages for"
                     << shared->getName() << "from" << provider.name;

        if (this->messageBatches.empty())
        {
            this->buildAndPostLoadedMessages(messageValues, 1);
            return;
        }

        this->messageBatches.emplace_back(messageValues);
        this->finishWithMessages();
    }

    void finishWithMessages()
    {
        auto shared = this->channelPtr.lock();
        if (!shared || isAppAboutToQuit())
        {
            return;
        }

        auto messages =
            mergeRecentMessageBatches(this->messageBatches, this->limit);
        const auto providerResponseCount = this->messageBatches.size();
        this->messageBatches.clear();
        this->buildAndPostLoadedMessages(std::move(messages),
                                         providerResponseCount);
    }

    void buildAndPostLoadedMessages(QJsonArray messages,
                                    size_t providerResponseCount)
    {
        auto self = this->shared_from_this();
        buildRecentMessagesBatched(
            std::move(messages), this->channelPtr,
            [self, providerResponseCount](
                std::vector<MessagePtr> builtMessages) mutable {
                self->postLoadedMessages(std::move(builtMessages),
                                         providerResponseCount);
            });
    }

    void postLoadedMessages(std::vector<MessagePtr> messages,
                            size_t providerResponseCount)
    {
        qCDebug(LOG) << "Using" << messages.size() << "recent messages for"
                     << this->channelName << "from" << providerResponseCount
                     << "provider response(s)";

        auto onLoaded = this->onLoaded;
        postToThread([messages = std::move(messages),
                      onLoaded = std::move(onLoaded)]() mutable {
            if (!isAppAboutToQuit())
            {
                onLoaded(messages);
            }
        });
    }

    void finishWithError()
    {
        auto channelPtr = this->channelPtr;
        auto onError = this->onError;
        auto lastError = this->lastError;
        postToThread([channelPtr, onError = std::move(onError),
                      lastError = std::move(lastError)] {
            auto shared = channelPtr.lock();
            if (!shared || isAppAboutToQuit())
            {
                return;
            }

            const auto detail = lastError.trimmed().isEmpty()
                                    ? QStringLiteral("all providers failed")
                                    : lastError;
            shared->addSystemMessage(
                QStringLiteral(
                    "Message history services unavailable (Error: %1)")
                    .arg(detail));
            onError();
        });
    }
};

}

void load(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before,
    const bool jitter)
{
    qCDebug(LOG) << "Loading recent messages for" << channelName;

    const long delayMs = jitter ? std::rand() % 100 : 0;
    QTimer::singleShot(delayMs, [=] {
        if (isAppAboutToQuit())
        {
            return;
        }

        auto request = std::make_shared<LoadRequest>();
        request->channelName = channelName;
        request->channelPtr = channelPtr;
        request->onLoaded = onLoaded;
        request->onError = onError;
        request->limit = limit;
        request->after = after;
        request->before = before;
        request->providerQueue = orderedProviders();
        request->tryNextProvider();
    });
}

}
