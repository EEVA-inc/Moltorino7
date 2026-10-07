#include "providers/youtube/YouTubeInnertubeSession.hpp"

#include "providers/youtube/YouTubeEmotes.hpp"
#include "providers/youtube/YouTubeInnertubeParser.hpp"
#include "providers/youtube/YouTubeMessagePacing.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr qsizetype MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
constexpr qsizetype INITIAL_RESPONSE_RESERVE = 64 * 1024;
constexpr qsizetype MAX_GUI_DELIVERY_TEXT_CHARS = 64 * 1024;
constexpr std::size_t MAX_PENDING_DELIVERY_MESSAGES = 5'000;
constexpr std::uint8_t MAX_REQUEST_FAILURES = 6;
constexpr std::uint8_t MAX_REBOOTSTRAPS = 1;
constexpr std::uint8_t MAX_BOOTSTRAP_FAILURES = 3;
constexpr int REQUEST_TIMEOUT_MS = 20'000;
constexpr std::chrono::milliseconds MIN_RATE_LIMIT_DELAY{30'000};

constexpr auto YOUTUBE_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 "
    "Safari/537.36 Moltorino/7.5";

QString responseError(const QByteArray &body, const QString &fallback)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && document.isObject())
    {
        const auto root = document.object();
        const auto nested = root.value(u"error"_s).toObject();
        const auto message = nested.value(u"message"_s).toString().trimmed();
        if (!message.isEmpty())
        {
            return message;
        }
    }
    return fallback.trimmed().isEmpty()
               ? u"YouTube's live chat request failed."_s
               : fallback.trimmed();
}

std::optional<std::chrono::milliseconds> retryAfter(QNetworkReply *reply)
{
    if (!reply)
    {
        return {};
    }
    bool ok = false;
    const auto seconds =
        reply->rawHeader("Retry-After").trimmed().toLongLong(&ok);
    if (!ok || seconds <= 0)
    {
        return {};
    }
    return std::chrono::seconds{std::clamp<qint64>(seconds, 1, 15 * 60)};
}

}

namespace chatterino {

struct YouTubeInnertubeSession::Runtime {
    struct PendingDelivery {
        std::vector<YouTubeMessage> messages;
        std::size_t next = 0;
        std::uint64_t generation = 0;
        bool historical = false;
    };

    QNetworkAccessManager network;
    QTimer timer;
    QTimer deliveryTimer;
    QPointer<QNetworkReply> reply;
    YouTubeEmotes::PagePtr page;
    QByteArray response;
    QString continuation;
    QString clickTrackingParams;
    std::shared_ptr<YouTubeMessageDeduper> deduper;
    std::deque<PendingDelivery> pendingDeliveries;
    std::optional<std::chrono::steady_clock::time_point> deliveryDeadline;
    std::optional<std::chrono::steady_clock::time_point> lastLiveBatchAt;
    std::chrono::milliseconds learnedDeliveryWindow =
        youtube::DEFAULT_MESSAGE_DELIVERY_WINDOW;
    std::optional<State> deferredTerminalState;
    QString deferredTerminalDetail;
    std::uint64_t callbackGeneration = 0;
    std::uint8_t requestFailures = 0;
    std::uint8_t rebootstrapCount = 0;
    std::uint8_t bootstrapFailures = 0;
    bool responseTooLarge = false;
    bool connected = false;
    bool terminal = false;
    bool firstBatch = true;
    bool bootstrapPending = false;
    bool liveBurstStarted = false;
};

YouTubeInnertubeSession::YouTubeInnertubeSession(
    QString videoID, QString liveChatID, MessagesCallback messages,
    StateCallback state, std::shared_ptr<YouTubeMessageDeduper> deduper)
    : videoID_(std::move(videoID))
    , liveChatID_(std::move(liveChatID))
    , messagesCallback_(std::move(messages))
    , stateCallback_(std::move(state))
    , runtime_(std::make_unique<Runtime>())
{
    this->runtime_->deduper =
        deduper ? std::move(deduper)
                : std::make_shared<YouTubeMessageDeduper>();
    this->runtime_->timer.setSingleShot(true);
    QObject::connect(&this->runtime_->timer, &QTimer::timeout, this, [this] {
        if (this->runtime_->bootstrapPending)
        {
            this->runtime_->bootstrapPending = false;
            this->bootstrap(true);
        }
        else
        {
            this->requestContinuation();
        }
    });
    this->runtime_->deliveryTimer.setSingleShot(true);
    this->runtime_->deliveryTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->runtime_->deliveryTimer, &QTimer::timeout, this,
                     [this] {
                         this->deliverPendingMessages();
                     });
}

YouTubeInnertubeSession::~YouTubeInnertubeSession()
{
    this->stop();
}

void YouTubeInnertubeSession::start()
{
    if (this->started_)
    {
        return;
    }
    this->started_ = true;
    this->stopping_ = false;
    ++this->runtime_->callbackGeneration;
    this->runtime_->terminal = false;
    this->runtime_->requestFailures = 0;
    this->runtime_->rebootstrapCount = 0;
    this->runtime_->bootstrapFailures = 0;
    this->runtime_->bootstrapPending = false;
    this->runtime_->firstBatch = true;
    this->runtime_->deliveryTimer.stop();
    this->runtime_->pendingDeliveries.clear();
    this->runtime_->deliveryDeadline.reset();
    this->runtime_->lastLiveBatchAt.reset();
    this->runtime_->learnedDeliveryWindow =
        youtube::DEFAULT_MESSAGE_DELIVERY_WINDOW;
    this->runtime_->liveBurstStarted = false;
    this->runtime_->deferredTerminalState.reset();
    this->runtime_->deferredTerminalDetail.clear();
    this->bootstrap(false);
}

void YouTubeInnertubeSession::stop()
{
    if (!this->started_ && !this->runtime_->reply &&
        !this->runtime_->timer.isActive() &&
        !this->runtime_->deliveryTimer.isActive() &&
        this->runtime_->pendingDeliveries.empty() &&
        !this->runtime_->deferredTerminalState)
    {
        return;
    }
    this->stopping_ = true;
    this->started_ = false;
    ++this->runtime_->callbackGeneration;
    this->runtime_->timer.stop();
    this->runtime_->deliveryTimer.stop();
    this->runtime_->bootstrapPending = false;
    this->runtime_->pendingDeliveries.clear();
    this->runtime_->deliveryDeadline.reset();
    this->runtime_->lastLiveBatchAt.reset();
    this->runtime_->learnedDeliveryWindow =
        youtube::DEFAULT_MESSAGE_DELIVERY_WINDOW;
    this->runtime_->liveBurstStarted = false;
    this->runtime_->deferredTerminalState.reset();
    this->runtime_->deferredTerminalDetail.clear();
    this->cancelActiveCall();
}

void YouTubeInnertubeSession::bootstrap(bool fresh)
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }

    const auto generation = this->runtime_->callbackGeneration;
    YouTubeEmotes::loadPageForVideo(
        this->videoID_,
        [self = QPointer(this), generation](
            ExpectedStr<YouTubeEmotes::PagePtr> result) {
            if (!self || !self->started_ || self->stopping_ ||
                self->runtime_->terminal ||
                self->runtime_->callbackGeneration != generation)
            {
                return;
            }
            if (!result || !*result || (*result)->apiKey.isEmpty() ||
                (*result)->clientName.isEmpty() ||
                (*result)->clientVersion.isEmpty() ||
                (*result)->continuation.isEmpty() ||
                (*result)->innertubeContext.isEmpty())
            {
                self->retryBootstrap(
                    result ? u"YouTube's live chat page was incomplete."_s
                           : result.error());
                return;
            }

            self->runtime_->page = *result;
            self->runtime_->continuation = (*result)->continuation;
            self->runtime_->clickTrackingParams =
                (*result)->clickTrackingParams;
            self->runtime_->requestFailures = 0;
            self->runtime_->bootstrapFailures = 0;
            self->requestContinuation();
        },
        fresh);
}

void YouTubeInnertubeSession::retryBootstrap(const QString &detail)
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }

    this->cancelActiveCall();
    this->runtime_->page.reset();
    this->runtime_->continuation.clear();
    this->runtime_->clickTrackingParams.clear();
    this->runtime_->lastLiveBatchAt.reset();
    this->runtime_->learnedDeliveryWindow =
        youtube::DEFAULT_MESSAGE_DELIVERY_WINDOW;
    this->runtime_->connected = false;
    this->dispatchState(State::Reconnecting, detail);

    if (++this->runtime_->bootstrapFailures > MAX_BOOTSTRAP_FAILURES)
    {
        this->finish(State::RefreshRequired, detail);
        return;
    }

    static constexpr std::array<std::chrono::seconds,
                                MAX_BOOTSTRAP_FAILURES>
        BOOTSTRAP_BACKOFF{
            std::chrono::seconds(1),
            std::chrono::seconds(3),
            std::chrono::seconds(10),
        };
    const auto index = std::min<std::size_t>(
        this->runtime_->bootstrapFailures - 1, BOOTSTRAP_BACKOFF.size() - 1);
    this->runtime_->bootstrapPending = true;
    this->schedulePoll(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            BOOTSTRAP_BACKOFF[index]));
}

void YouTubeInnertubeSession::requestContinuation()
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal ||
        this->runtime_->reply || !this->runtime_->page)
    {
        return;
    }
    std::size_t pendingMessages = 0;
    for (const auto &delivery : this->runtime_->pendingDeliveries)
    {
        pendingMessages += delivery.messages.size() - delivery.next;
    }
    if (pendingMessages >= MAX_PENDING_DELIVERY_MESSAGES)
    {
        this->schedulePoll(youtube::MIN_CONTINUATION_POLL_DELAY);
        return;
    }
    const auto &page = *this->runtime_->page;
    if (this->runtime_->continuation.isEmpty())
    {
        this->finish(State::Ended);
        return;
    }

    auto context = page.innertubeContext;
    if (!this->runtime_->clickTrackingParams.isEmpty())
    {
        context.insert(
            u"clickTracking"_s,
            QJsonObject{{u"clickTrackingParams"_s,
                         this->runtime_->clickTrackingParams}});
    }
    const QJsonObject body{
        {u"context"_s, context},
        {u"continuation"_s, this->runtime_->continuation},
    };

    QUrl url(u"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat"_s);
    QUrlQuery query;
    query.addQueryItem(u"key"_s, page.apiKey);
    query.addQueryItem(u"prettyPrint"_s, u"false"_s);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      u"application/json"_s);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Origin", "https://www.youtube.com");
    request.setRawHeader("Referer", "https://www.youtube.com/");
    request.setRawHeader("User-Agent", YOUTUBE_USER_AGENT);
    request.setRawHeader("X-YouTube-Client-Name",
                         page.clientName.toUtf8());
    request.setRawHeader("X-YouTube-Client-Version",
                         page.clientVersion.toUtf8());
    if (!page.visitorData.isEmpty())
    {
        request.setRawHeader("X-Goog-Visitor-Id",
                             page.visitorData.toUtf8());
    }
    request.setTransferTimeout(REQUEST_TIMEOUT_MS);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    this->runtime_->response.clear();

    this->runtime_->response.reserve(INITIAL_RESPONSE_RESERVE);
    this->runtime_->responseTooLarge = false;
    auto *reply = this->runtime_->network.post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply->setReadBufferSize(MAX_RESPONSE_BYTES + 1);
    this->runtime_->reply = reply;
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (this->runtime_->reply == reply)
        {
            this->readAvailable(reply);
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (this->runtime_->reply == reply)
        {
            this->requestFinished(reply);
        }
        else
        {
            reply->deleteLater();
        }
    });
}

void YouTubeInnertubeSession::readAvailable(QNetworkReply *reply)
{
    if (!reply || this->runtime_->reply != reply ||
        this->runtime_->responseTooLarge)
    {
        return;
    }
    const auto bytes = reply->readAll();
    if (bytes.size() > MAX_RESPONSE_BYTES - this->runtime_->response.size())
    {
        this->runtime_->response.clear();
        this->runtime_->responseTooLarge = true;
        reply->abort();
        return;
    }
    this->runtime_->response.append(bytes);
}

void YouTubeInnertubeSession::requestFinished(QNetworkReply *reply)
{
    this->readAvailable(reply);
    if (this->runtime_->reply != reply)
    {
        return;
    }

    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const auto fallback = reply->errorString();
    const auto serverDelay = retryAfter(reply);
    const auto body = std::move(this->runtime_->response);
    const bool tooLarge = this->runtime_->responseTooLarge;
    this->runtime_->reply.clear();
    reply->deleteLater();

    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }
    if (tooLarge)
    {
        this->retry(u"YouTube returned an oversized live chat response."_s,
                    true);
        return;
    }
    if (status == 404 || status == 410)
    {
        this->finish(State::Ended);
        return;
    }
    if (status == 429)
    {
        this->retry(
            responseError(body, fallback), false,
            std::max(serverDelay.value_or(MIN_RATE_LIMIT_DELAY),
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         MIN_RATE_LIMIT_DELAY)));
        return;
    }
    if (networkError != QNetworkReply::NoError || status < 200 ||
        status >= 300)
    {
        const bool refreshClient = status == 400 || status == 401 ||
                                   status == 403;
        this->retry(responseError(body, fallback), refreshClient,
                    serverDelay);
        return;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        this->retry(
            u"YouTube returned malformed live chat data: "_s +
                parseError.errorString(),
            true);
        return;
    }
    auto parsed = parseYouTubeInnertubeChat(document.object(),
                                            this->liveChatID_);
    if (!parsed.valid)
    {
        this->retry(parsed.error, true);
        return;
    }

    this->runtime_->requestFailures = 0;
    this->runtime_->rebootstrapCount = 0;
    if (!this->runtime_->connected)
    {
        this->runtime_->connected = true;
        this->dispatchState(State::Connected, {});
    }

    std::vector<YouTubeMessage> messages;
    messages.reserve(parsed.messages.size());
    for (auto &message : parsed.messages)
    {
        if (!this->runtime_->deduper->accept(message))
        {
            continue;
        }
        messages.emplace_back(std::move(message));
    }
    const bool receivedMessages = !messages.empty();
    if (receivedMessages)
    {
        this->dispatchMessages(std::move(messages),
                               this->runtime_->firstBatch);
    }
    this->runtime_->firstBatch = false;
    if (parsed.ended || parsed.nextContinuation.isEmpty())
    {
        this->finish(State::Ended);
        return;
    }
    this->runtime_->continuation = std::move(parsed.nextContinuation);
    if (!parsed.clickTrackingParams.isEmpty())
    {
        this->runtime_->clickTrackingParams =
            std::move(parsed.clickTrackingParams);
    }
    const auto now = std::chrono::steady_clock::now();
    const bool recentlyActive =
        this->runtime_->lastLiveBatchAt &&
        now - *this->runtime_->lastLiveBatchAt <=
            youtube::RECENT_ACTIVITY_POLL_WINDOW;

    const auto delay = youtube::continuationPollDelay(
        receivedMessages, recentlyActive, parsed.serverTimeout);
    this->schedulePoll(delay);
}

void YouTubeInnertubeSession::schedulePoll(std::chrono::milliseconds delay)
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }
    this->runtime_->timer.start(
        std::max(delay, youtube::MIN_CONTINUATION_POLL_DELAY));
}

void YouTubeInnertubeSession::retry(
    const QString &detail, bool rebootstrap,
    std::optional<std::chrono::milliseconds> delay)
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }
    this->cancelActiveCall();
    this->runtime_->lastLiveBatchAt.reset();
    this->runtime_->learnedDeliveryWindow =
        youtube::DEFAULT_MESSAGE_DELIVERY_WINDOW;
    this->runtime_->connected = false;
    this->dispatchState(State::Reconnecting, detail);

    if (rebootstrap &&
        this->runtime_->rebootstrapCount < MAX_REBOOTSTRAPS)
    {
        ++this->runtime_->rebootstrapCount;
        this->runtime_->page.reset();
        this->runtime_->continuation.clear();
        this->runtime_->clickTrackingParams.clear();
        this->bootstrap(true);
        return;
    }
    if (++this->runtime_->requestFailures > MAX_REQUEST_FAILURES)
    {
        this->finish(State::RefreshRequired, detail);
        return;
    }

    if (!delay)
    {
        static constexpr std::array<std::chrono::seconds, 6> BACKOFF{
            std::chrono::seconds(1),  std::chrono::seconds(2),
            std::chrono::seconds(4),  std::chrono::seconds(8),
            std::chrono::seconds(16), std::chrono::seconds(30),
        };
        const auto index = std::min<std::size_t>(
            this->runtime_->requestFailures - 1, BACKOFF.size() - 1);
        auto base = std::chrono::duration_cast<std::chrono::milliseconds>(
            BACKOFF[index]);
        const auto jitter =
            static_cast<int>(QRandomGenerator::global()->bounded(401)) - 200;
        delay = std::max(youtube::MIN_CONTINUATION_POLL_DELAY,
                         base + base * jitter / 1000);
    }
    this->schedulePoll(*delay);
}

void YouTubeInnertubeSession::finish(State state, const QString &detail)
{
    if (this->runtime_->terminal)
    {
        return;
    }
    this->runtime_->terminal = true;
    this->started_ = false;
    this->runtime_->timer.stop();
    this->runtime_->bootstrapPending = false;
    this->cancelActiveCall();
    if (!this->runtime_->pendingDeliveries.empty())
    {
        this->runtime_->deferredTerminalState = state;
        this->runtime_->deferredTerminalDetail = detail;
        return;
    }
    this->dispatchState(state, detail);
}

void YouTubeInnertubeSession::cancelActiveCall()
{
    auto *reply = this->runtime_->reply.data();
    this->runtime_->reply.clear();
    if (!reply)
    {
        return;
    }
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void YouTubeInnertubeSession::dispatchMessages(
    std::vector<YouTubeMessage> messages, bool historical)
{
    if (messages.empty())
    {
        return;
    }

    std::unordered_set<QString> purgedAuthors;
    std::unordered_set<QString> tombstonedMessages;
    for (const auto &message : messages)
    {
        if (message.kind == YouTubeMessageKind::AuthorMessagesDeleted &&
            !message.targetAuthorChannelID.isEmpty())
        {
            purgedAuthors.insert(message.targetAuthorChannelID);
        }
        else if (message.kind == YouTubeMessageKind::UserBanned &&
                 message.ban && !message.ban->targetChannelId.isEmpty())
        {
            purgedAuthors.insert(message.ban->targetChannelId);
        }
        else if (message.kind == YouTubeMessageKind::Tombstone &&
                 !message.targetMessageID.isEmpty())
        {
            tombstonedMessages.insert(message.targetMessageID);
        }
    }
    if (!purgedAuthors.empty() || !tombstonedMessages.empty())
    {
        for (auto &delivery : this->runtime_->pendingDeliveries)
        {
            auto first = delivery.messages.begin() +
                         static_cast<std::ptrdiff_t>(delivery.next);
            delivery.messages.erase(
                std::remove_if(
                    first, delivery.messages.end(),
                    [&purgedAuthors,
                     &tombstonedMessages](const YouTubeMessage &message) {
                        return message.isUserChatMessage() &&
                               (purgedAuthors.contains(
                                    message.author.channelId) ||
                                tombstonedMessages.contains(message.id));
                    }),
                delivery.messages.end());
        }
        std::erase_if(this->runtime_->pendingDeliveries,
                      [](const Runtime::PendingDelivery &delivery) {
                          return delivery.next >= delivery.messages.size();
                      });
        std::erase_if(messages, [&purgedAuthors, &tombstonedMessages](
                                    const YouTubeMessage &message) {
            return message.isUserChatMessage() &&
                   (purgedAuthors.contains(message.author.channelId) ||
                    tombstonedMessages.contains(message.id));
        });
    }
    if (messages.empty())
    {
        return;
    }

    const bool queueWasEmpty = this->runtime_->pendingDeliveries.empty();
    this->runtime_->pendingDeliveries.emplace_back(Runtime::PendingDelivery{
        .messages = std::move(messages),
        .generation = this->runtime_->callbackGeneration,
        .historical = historical,
    });
    if (!historical)
    {
        const auto now = std::chrono::steady_clock::now();
        if (this->runtime_->lastLiveBatchAt)
        {
            this->runtime_->learnedDeliveryWindow =
                youtube::nextMessageDeliveryWindow(
                    this->runtime_->learnedDeliveryWindow,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - *this->runtime_->lastLiveBatchAt));
        }
        this->runtime_->lastLiveBatchAt = now;

        this->runtime_->deliveryDeadline =
            now + this->runtime_->learnedDeliveryWindow;
        if (queueWasEmpty)
        {
            this->runtime_->liveBurstStarted = false;
        }
    }
    if (!this->runtime_->deliveryTimer.isActive())
    {
        this->runtime_->deliveryTimer.start(0);
    }
}

void YouTubeInnertubeSession::deliverPendingMessages()
{
    while (!this->runtime_->pendingDeliveries.empty() &&
           this->runtime_->pendingDeliveries.front().generation !=
               this->runtime_->callbackGeneration)
    {
        this->runtime_->pendingDeliveries.pop_front();
    }

    if (this->stopping_ || this->runtime_->pendingDeliveries.empty())
    {
        if (!this->stopping_ && this->runtime_->deferredTerminalState)
        {
            const auto state = *this->runtime_->deferredTerminalState;
            auto detail =
                std::move(this->runtime_->deferredTerminalDetail);
            this->runtime_->deferredTerminalState.reset();
            this->dispatchState(state, std::move(detail));
        }
        return;
    }

    auto &pending = this->runtime_->pendingDeliveries.front();
    const auto generation = pending.generation;
    const auto historical = pending.historical;
    const auto begin = pending.next;
    std::size_t targetMessages = youtube::MAX_MESSAGE_DELIVERY_BATCH;
    if (!historical)
    {
        if (!this->runtime_->liveBurstStarted)
        {
            targetMessages = 1;
            this->runtime_->liveBurstStarted = true;
        }
        else
        {
            targetMessages = youtube::messageDeliveryStep(
                                 this->pendingLiveMessageCount(),
                                 this->remainingDeliveryWindow())
                                 .batchSize;
        }
    }
    auto end = begin;
    qsizetype textChars = 0;
    while (end < pending.messages.size() &&
           end - begin < targetMessages)
    {
        const auto cost = youtube::messageDeliveryTextCost(
            pending.messages[end], MAX_GUI_DELIVERY_TEXT_CHARS);
        if (end != begin &&
            cost > MAX_GUI_DELIVERY_TEXT_CHARS - textChars)
        {
            break;
        }
        textChars += cost;
        ++end;
    }

    std::vector<YouTubeMessage> chunk;
    chunk.reserve(end - begin);
    for (auto index = begin; index < end; ++index)
    {
        chunk.emplace_back(std::move(pending.messages[index]));
    }
    pending.next = end;
    if (pending.next == pending.messages.size())
    {
        this->runtime_->pendingDeliveries.pop_front();
    }

    const auto self = QPointer(this);
    this->messagesCallback_(std::move(chunk), historical);
    if (!self || self->stopping_ ||
        self->runtime_->callbackGeneration != generation)
    {
        return;
    }

    if (!self->runtime_->pendingDeliveries.empty())
    {
        const auto &next = self->runtime_->pendingDeliveries.front();
        if (next.historical)
        {
            self->runtime_->deliveryTimer.start(0);
            return;
        }
        if (!self->runtime_->liveBurstStarted)
        {
            self->runtime_->deliveryTimer.start(0);
            return;
        }

        self->runtime_->deliveryTimer.start(
            youtube::messageDeliveryStep(self->pendingLiveMessageCount(),
                                         self->remainingDeliveryWindow())
                .delay);
        return;
    }
    self->runtime_->deliveryDeadline.reset();
    self->runtime_->liveBurstStarted = false;
    if (self->runtime_->deferredTerminalState)
    {
        const auto state = *self->runtime_->deferredTerminalState;
        auto detail = std::move(self->runtime_->deferredTerminalDetail);
        self->runtime_->deferredTerminalState.reset();
        self->dispatchState(state, std::move(detail));
    }
}

std::size_t YouTubeInnertubeSession::pendingLiveMessageCount() const
{
    std::size_t count = 0;
    for (const auto &delivery : this->runtime_->pendingDeliveries)
    {
        if (!delivery.historical)
        {
            count += delivery.messages.size() - delivery.next;
        }
    }
    return count;
}

std::chrono::milliseconds
YouTubeInnertubeSession::remainingDeliveryWindow() const
{
    if (!this->runtime_->deliveryDeadline)
    {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        *this->runtime_->deliveryDeadline - std::chrono::steady_clock::now());
}

void YouTubeInnertubeSession::dispatchState(State state, QString detail)
{
    const auto generation = this->runtime_->callbackGeneration;
    QTimer::singleShot(
        0, this,
        [self = QPointer(this), generation, state,
         detail = std::move(detail)] {
            if (self && self->runtime_->callbackGeneration == generation)
            {
                self->stateCallback_(state, detail);
            }
        });
}

}
