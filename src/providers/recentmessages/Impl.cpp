// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Impl.hpp"

#include "Application.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/Helpers.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/VectorMessageSink.hpp"

#include <IrcMessage>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSet>
#include <QUrlQuery>

#include <algorithm>
#include <deque>
#include <limits>

namespace chatterino::recentmessages::detail {

namespace {

constexpr qint64 RECEIPT_DUPLICATE_TOLERANCE_MS = 2000;
constexpr size_t BUILD_SLICE_MESSAGE_LIMIT = 25;
constexpr qint64 BUILD_SLICE_TIME_LIMIT_MS = 3;
constexpr size_t MEMORY_RELIEF_MESSAGE_THRESHOLD = 128;

struct MergeCandidate {
    QString raw;
    QString key;
    qint64 timestamp{};
    bool hasStableIdentity{};
};

struct ReceiptOccurrence {
    qint64 timestamp{};
    size_t batch{};
};

std::optional<MergeCandidate> inspectMessage(const QString &raw)
{
    const auto content = unescapeZeroWidthJoiner(raw);
    auto *message = Communi::IrcMessage::fromData(content.toUtf8(), nullptr);
    if (message == nullptr)
    {
        return std::nullopt;
    }

    auto tags = message->tags();
    bool hasSentTimestamp = false;
    auto timestamp =
        tags.value(QStringLiteral("tmi-sent-ts")).toLongLong(&hasSentTimestamp);
    bool hasReceiptTimestamp = false;
    if (!hasSentTimestamp)
    {
        timestamp = tags.value(QStringLiteral("rm-received-ts"))
                        .toLongLong(&hasReceiptTimestamp);
    }
    if (!hasSentTimestamp && !hasReceiptTimestamp)
    {
        timestamp = 0;
    }

    QString key;
    const auto messageId = tags.value(QStringLiteral("id")).toString();
    if (!messageId.isEmpty())
    {
        key = QStringLiteral("id:") + messageId;
    }
    else
    {
        tags.remove(QStringLiteral("rm-received-ts"));
        tags.remove(QStringLiteral("historical"));

        QJsonObject canonical;
        canonical.insert(QStringLiteral("prefix"), message->prefix());
        canonical.insert(QStringLiteral("command"), message->command());
        canonical.insert(QStringLiteral("tags"),
                         QJsonObject::fromVariantMap(tags));

        QJsonArray parameters;
        for (const auto &parameter : message->parameters())
        {
            parameters.append(parameter);
        }
        canonical.insert(QStringLiteral("parameters"), parameters);
        key = QString::fromUtf8(
            QJsonDocument(canonical).toJson(QJsonDocument::Compact));
    }

    delete message;
    return MergeCandidate{
        .raw = raw,
        .key = std::move(key),
        .timestamp = timestamp,
        .hasStableIdentity = !messageId.isEmpty() || hasSentTimestamp,
    };
}

bool closeReceiptTimes(qint64 lhs, qint64 rhs)
{
    const auto delta =
        lhs > rhs ? quint64(lhs) - quint64(rhs) : quint64(rhs) - quint64(lhs);
    return delta <= RECEIPT_DUPLICATE_TOLERANCE_MS;
}

class RecentMessagesBuildTask;

struct BuildSchedulerState {
    std::deque<std::shared_ptr<RecentMessagesBuildTask>> tasks;
    bool callbackQueued = false;
    size_t completedMessagesSinceDrain = 0;
};

BuildSchedulerState &buildSchedulerState()
{
    static auto *state = new BuildSchedulerState;
    return *state;
}

void scheduleNextBuildSlice();

class RecentMessagesBuildTask
{
public:
    RecentMessagesBuildTask(
        QJsonArray messages, std::weak_ptr<Channel> channel,
        std::function<void(std::vector<MessagePtr>)> onBuilt)
        : messages_(std::move(messages))
        , channel_(std::move(channel))
        , onBuilt_(std::move(onBuilt))
        , sink_({}, MessageFlag::RecentMessage)
    {
    }

    bool processSlice()
    {
        assertInGuiThread();

        auto channel = this->channel_.lock();
        auto *twitchChannel =
            channel ? dynamic_cast<TwitchChannel *>(channel.get()) : nullptr;
        if (twitchChannel == nullptr || isAppAboutToQuit())
        {
            return true;
        }

        QElapsedTimer timer;
        timer.start();
        size_t processed = 0;
        while (this->nextMessage_ < this->messages_.size())
        {
            const auto content = unescapeZeroWidthJoiner(
                this->messages_.at(this->nextMessage_++).toString());
            std::unique_ptr<Communi::IrcMessage> message(
                Communi::IrcMessage::fromData(content.toUtf8(), nullptr));
            if (message != nullptr)
            {
                if (message->tags().contains("rm-received-ts"))
                {
                    const auto msgDate =
                        QDateTime::fromMSecsSinceEpoch(
                            message->tags().value("rm-received-ts").toLongLong())
                            .date();

                    if (msgDate != channel->lastDate_)
                    {
                        channel->lastDate_ = msgDate;
                        auto msg = makeSystemMessage(
                            QLocale().toString(msgDate, QLocale::LongFormat),
                            QTime(0, 0));
                        this->sink_.addMessage(msg, MessageContext::Original);
                    }
                }

                IrcMessageHandler::parseMessageInto(message.get(), this->sink_,
                                                    twitchChannel);
                ++this->parsedMessageCount_;
            }

            processed++;

            if (processed >= BUILD_SLICE_MESSAGE_LIMIT ||
                timer.elapsed() >= BUILD_SLICE_TIME_LIMIT_MS)
            {
                break;
            }
        }

        if (this->nextMessage_ != this->messages_.size())
        {
            return false;
        }

        auto onBuilt = std::move(this->onBuilt_);
        onBuilt(std::move(this->sink_).takeMessages());
        return true;
    }

    size_t messageCount() const
    {
        return this->parsedMessageCount_;
    }

private:
    QJsonArray messages_;
    std::weak_ptr<Channel> channel_;
    std::function<void(std::vector<MessagePtr>)> onBuilt_;
    VectorMessageSink sink_;
    qsizetype nextMessage_ = 0;
    size_t parsedMessageCount_ = 0;
};

void processNextBuildSlice()
{
    assertInGuiThread();
    auto &state = buildSchedulerState();
    state.callbackQueued = false;

    if (isAppAboutToQuit())
    {
        state.tasks.clear();
        return;
    }
    if (state.tasks.empty())
    {
        return;
    }

    auto task = std::move(state.tasks.front());
    state.tasks.pop_front();
    if (!task->processSlice())
    {
        state.tasks.push_back(std::move(task));
    }
    else
    {
        state.completedMessagesSinceDrain += task->messageCount();

        task.reset();
    }

    if (state.tasks.empty())
    {
        const auto completedMessages = state.completedMessagesSinceDrain;
        state.completedMessagesSinceDrain = 0;
        if (completedMessages >= MEMORY_RELIEF_MESSAGE_THRESHOLD)
        {
            requestMemoryPressureRelief();
        }
    }

    scheduleNextBuildSlice();
}

void scheduleNextBuildSlice()
{
    auto &state = buildSchedulerState();
    if (state.callbackQueued || state.tasks.empty())
    {
        return;
    }

    state.callbackQueued = true;
    QMetaObject::invokeMethod(qApp, processNextBuildSlice,
                              Qt::QueuedConnection);
}

}

bool hasParseableRecentMessages(const QJsonArray &messages)
{
    for (const auto &jsonMessage : messages)
    {
        auto content = unescapeZeroWidthJoiner(jsonMessage.toString());

        auto *message =
            Communi::IrcMessage::fromData(content.toUtf8(), nullptr);

        if (message != nullptr)
        {
            delete message;
            return true;
        }
    }

    return false;
}

void buildRecentMessagesBatched(
    QJsonArray messages, std::weak_ptr<Channel> channel,
    std::function<void(std::vector<MessagePtr>)> onBuilt)
{
    assertInGuiThread();

    auto &state = buildSchedulerState();
    state.tasks.emplace_back(std::make_shared<RecentMessagesBuildTask>(
        std::move(messages), std::move(channel), std::move(onBuilt)));
    scheduleNextBuildSlice();
}

QJsonArray mergeRecentMessageBatches(const std::vector<QJsonArray> &batches,
                                     const int limit)
{
    std::vector<MergeCandidate> candidates;
    QSet<QString> stableIdentities;
    QHash<QString, std::vector<ReceiptOccurrence>> receiptOccurrences;
    QHash<QString, size_t> untimedRawMessages;

    for (size_t batchIndex = 0; batchIndex < batches.size(); ++batchIndex)
    {
        const auto &batch = batches[batchIndex];
        candidates.reserve(candidates.size() + size_t(batch.size()));
        for (const auto &value : batch)
        {
            if (!value.isString())
            {
                continue;
            }

            auto candidate = inspectMessage(value.toString());
            if (!candidate)
            {
                continue;
            }

            bool isDuplicate = false;
            if (candidate->hasStableIdentity)
            {
                isDuplicate = stableIdentities.contains(candidate->key);
                stableIdentities.insert(candidate->key);
            }
            else if (candidate->timestamp > 0)
            {
                auto &occurrences = receiptOccurrences[candidate->key];
                isDuplicate = std::any_of(
                    occurrences.cbegin(), occurrences.cend(),
                    [candidateTimestamp = candidate->timestamp,
                     batchIndex](const auto &occurrence) {
                        return occurrence.batch != batchIndex &&
                               closeReceiptTimes(occurrence.timestamp,
                                                 candidateTimestamp);
                    });
                if (!isDuplicate)
                {
                    occurrences.push_back({candidate->timestamp, batchIndex});
                }
            }
            else
            {
                const auto previous =
                    untimedRawMessages.constFind(candidate->raw);
                isDuplicate = previous != untimedRawMessages.cend() &&
                              *previous != batchIndex;
                if (previous == untimedRawMessages.cend())
                {
                    untimedRawMessages.insert(candidate->raw, batchIndex);
                }
            }

            if (isDuplicate)
            {
                continue;
            }

            candidates.emplace_back(std::move(*candidate));
        }
    }

    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const auto &lhs, const auto &rhs) {
            const auto lhsTimestamp = lhs.timestamp > 0
                                          ? lhs.timestamp
                                          : std::numeric_limits<qint64>::max();
            const auto rhsTimestamp = rhs.timestamp > 0
                                          ? rhs.timestamp
                                          : std::numeric_limits<qint64>::max();
            return lhsTimestamp < rhsTimestamp;
        });

    if (limit > 0 && candidates.size() > size_t(limit))
    {
        candidates.erase(candidates.begin(), candidates.end() - limit);
    }

    QJsonArray merged;
    for (const auto &candidate : candidates)
    {
        merged.append(candidate.raw);
    }
    return merged;
}

QUrl constructRecentMessagesUrl(
    const QString &urlTemplate, const QString &name, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before)
{
    QUrl url(urlTemplate.arg(name));
    QUrlQuery urlQuery(url);
    if (!urlQuery.hasQueryItem("limit"))
    {
        urlQuery.addQueryItem("limit", QString::number(limit));
    }
    if (after.has_value())
    {
        urlQuery.addQueryItem(
            "after", QString::number(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             after->time_since_epoch())
                             .count()));
    }
    if (before.has_value())
    {
        urlQuery.addQueryItem(
            "before", QString::number(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              before->time_since_epoch())
                              .count()));
    }
    url.setQuery(urlQuery);
    return url;
}

}
