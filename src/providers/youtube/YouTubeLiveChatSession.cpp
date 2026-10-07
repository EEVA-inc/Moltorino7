#include "providers/youtube/YouTubeLiveChatSession.hpp"

#include "providers/youtube/YouTubeCredentials.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr qsizetype MAX_FRAME_BYTES = 16 * 1024 * 1024;
constexpr qsizetype MAX_ERROR_BYTES = 256 * 1024;
constexpr auto STREAM_INACTIVITY_TIMEOUT = std::chrono::minutes(2);
constexpr auto BUFFER_RELEASE_IDLE = std::chrono::seconds(30);
constexpr qsizetype RETAINED_BUFFER_LIMIT = 512 * 1024;
constexpr qsizetype RETAINED_ERROR_LIMIT = 64 * 1024;

QNetworkRequest makeStreamRequest(const QString &liveChatID,
                                  const QString &readTicket,
                                  const QString &cursor)
{
    auto root = youtube::credentials::readRoot().trimmed();
    if (!root.endsWith('/'))
    {
        root.append('/');
    }
    QUrl url(root + u"live"_s);
    QUrlQuery query{{u"liveChatId"_s, liveChatID}};
    if (!cursor.isEmpty())
    {
        query.addQueryItem(u"cursor"_s, cursor);
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/x-ndjson");
    request.setRawHeader("X-Moltorino-YouTube-Ticket",
                         readTicket.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    request.setTransferTimeout(STREAM_INACTIVITY_TIMEOUT);
    return request;
}

QString responseError(const QByteArray &body, const QString &fallback)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        return fallback;
    }

    const auto root = document.object();
    const auto message = root.value("message").toString().trimmed();
    if (!message.isEmpty())
    {
        return message;
    }
    const auto error = root.value("error");
    if (error.isString() && !error.toString().trimmed().isEmpty())
    {
        return error.toString().trimmed();
    }
    if (error.isObject())
    {
        const auto nested = error.toObject().value("message").toString();
        if (!nested.trimmed().isEmpty())
        {
            return nested.trimmed();
        }
    }
    return fallback;
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
    return std::chrono::milliseconds{
        std::clamp<qint64>(seconds, 1, 15 * 60) * 1000};
}

}

namespace chatterino {

struct YouTubeLiveChatSession::Runtime {
    QNetworkAccessManager network;
    QTimer reconnectTimer;
    QTimer bufferReleaseTimer;
    QPointer<QNetworkReply> reply;
    QByteArray buffer;
    QByteArray errorBody;
    QString cursor;
    QString parserError;
    std::shared_ptr<YouTubeMessageDeduper> deduper;
    std::chrono::milliseconds backoff{1000};
    std::uint64_t callbackGeneration = 0;
    bool connected = false;
    bool terminal = false;
};

YouTubeLiveChatSession::YouTubeLiveChatSession(
    QString liveChatID, QString readTicket,
    MessagesCallback messages, StateCallback state,
    std::shared_ptr<YouTubeMessageDeduper> deduper)
    : liveChatID_(std::move(liveChatID))
    , readTicket_(std::move(readTicket))
    , messagesCallback_(std::move(messages))
    , stateCallback_(std::move(state))
    , runtime_(std::make_unique<Runtime>())
{
    this->runtime_->deduper =
        deduper ? std::move(deduper)
                : std::make_shared<YouTubeMessageDeduper>();
    this->runtime_->reconnectTimer.setSingleShot(true);
    QObject::connect(&this->runtime_->reconnectTimer, &QTimer::timeout, this,
                     [this] {
                         this->openStream();
                     });
    this->runtime_->bufferReleaseTimer.setSingleShot(true);
    this->runtime_->bufferReleaseTimer.setInterval(BUFFER_RELEASE_IDLE);
    QObject::connect(&this->runtime_->bufferReleaseTimer, &QTimer::timeout,
                     this, [this] {
                         if (this->runtime_->buffer.isEmpty() &&
                             this->runtime_->buffer.capacity() >
                                 RETAINED_BUFFER_LIMIT)
                         {
                             this->runtime_->buffer.squeeze();
                         }
                     });
}

YouTubeLiveChatSession::~YouTubeLiveChatSession()
{
    this->stop();
}

void YouTubeLiveChatSession::start()
{
    if (this->started_)
    {
        return;
    }
    this->started_ = true;
    this->stopping_ = false;
    ++this->runtime_->callbackGeneration;
    this->runtime_->terminal = false;
    this->runtime_->backoff = std::chrono::seconds(1);
    if (this->readTicket_.isEmpty())
    {
        this->finish(State::RefreshRequired,
                     u"YouTube chat authorization is missing."_s);
        return;
    }
    this->openStream();
}

void YouTubeLiveChatSession::stop()
{
    if (!this->started_ && !this->runtime_->reply &&
        !this->runtime_->reconnectTimer.isActive())
    {
        return;
    }
    this->stopping_ = true;
    this->started_ = false;
    ++this->runtime_->callbackGeneration;
    this->runtime_->reconnectTimer.stop();
    this->cancelActiveCall();
}

void YouTubeLiveChatSession::cancelActiveCall()
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

void YouTubeLiveChatSession::openStream()
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal ||
        this->runtime_->reply)
    {
        return;
    }

    this->runtime_->buffer.clear();
    this->runtime_->errorBody.clear();
    this->runtime_->bufferReleaseTimer.stop();
    if (this->runtime_->buffer.capacity() > RETAINED_BUFFER_LIMIT)
    {
        this->runtime_->buffer.squeeze();
    }
    if (this->runtime_->errorBody.capacity() > RETAINED_ERROR_LIMIT)
    {
        this->runtime_->errorBody.squeeze();
    }
    this->runtime_->parserError.clear();
    this->runtime_->connected = false;

    const auto request = makeStreamRequest(
        this->liveChatID_, this->readTicket_, this->runtime_->cursor);
    auto *reply = this->runtime_->network.get(request);
    reply->setReadBufferSize(MAX_FRAME_BYTES + 1);
    this->runtime_->reply = reply;
    QObject::connect(reply, &QNetworkReply::readyRead, this,
                     [this, reply] {
                         if (this->runtime_->reply == reply)
                         {
                             this->readAvailable(reply);
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (this->runtime_->reply == reply)
        {
            this->streamFinished(reply);
        }
        else
        {
            reply->deleteLater();
        }
    });
}

void YouTubeLiveChatSession::readAvailable(QNetworkReply *reply)
{
    if (!reply || this->runtime_->reply != reply)
    {
        return;
    }
    const auto bytes = reply->readAll();
    if (bytes.isEmpty())
    {
        return;
    }

    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && (status < 200 || status >= 300))
    {
        const auto remaining =
            MAX_ERROR_BYTES - this->runtime_->errorBody.size();
        if (remaining > 0)
        {
            this->runtime_->errorBody.append(bytes.left(remaining));
        }
        return;
    }

    if (bytes.size() > MAX_FRAME_BYTES - this->runtime_->buffer.size())
    {
        this->runtime_->parserError =
            u"A YouTube chat frame exceeded the safety limit."_s;
        reply->abort();
        return;
    }

    this->runtime_->buffer.append(bytes);

    while (true)
    {
        const auto newline = this->runtime_->buffer.indexOf('\n');
        if (newline < 0)
        {
            break;
        }
        auto line = this->runtime_->buffer.left(newline);
        this->runtime_->buffer.remove(0, newline + 1);
        if (!this->processLine(line))
        {
            reply->abort();
            return;
        }
        if (this->runtime_->terminal || this->runtime_->reply != reply)
        {
            return;
        }
    }
    if (this->runtime_->buffer.isEmpty() &&
        this->runtime_->buffer.capacity() > RETAINED_BUFFER_LIMIT)
    {
        this->runtime_->bufferReleaseTimer.start();
    }
}

bool YouTubeLiveChatSession::processLine(const QByteArray &line)
{
    const auto trimmed = line.trimmed();
    if (trimmed.isEmpty())
    {
        return true;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        this->runtime_->parserError =
            u"Moltorino returned malformed YouTube chat data: "_s +
            parseError.errorString();
        return false;
    }
    this->processFrame(document.object());
    return this->runtime_->parserError.isEmpty();
}

void YouTubeLiveChatSession::processFrame(const QJsonObject &frame)
{
    if (frame.value("v").toInt() != 1)
    {
        this->runtime_->parserError =
            u"Moltorino returned an unsupported YouTube stream protocol."_s;
        return;
    }

    const auto type = frame.value("type").toString();
    const auto cursor = frame.value("cursor").toString();
    if (!cursor.isEmpty())
    {
        this->runtime_->cursor = cursor;
    }

    if (type == u"ping"_s)
    {
        return;
    }
    if (type == u"ready"_s)
    {
        if (!this->runtime_->connected)
        {
            this->runtime_->connected = true;
            this->runtime_->backoff = std::chrono::seconds(1);
            this->dispatchState(State::Connected, {});
        }
        return;
    }
    if (type == u"state"_s)
    {
        if (frame.value("state").toString() == u"reconnecting"_s)
        {
            this->runtime_->connected = false;
            this->dispatchState(State::Reconnecting,
                                frame.value("message").toString());
        }
        return;
    }
    if (type == u"ended"_s)
    {
        this->finish(State::Ended, frame.value("message").toString());
        return;
    }
    if (type == u"error"_s)
    {
        const auto detail = frame.value("message").toString();
        if (frame.value("retryable").toBool())
        {
            this->runtime_->connected = false;
            this->dispatchState(State::Reconnecting, detail);
        }
        else
        {
            this->finish(State::Failed, detail);
        }
        return;
    }
    if (type != u"snapshot"_s && type != u"messages"_s)
    {
        return;
    }

    if (!this->runtime_->connected)
    {
        this->runtime_->connected = true;
        this->runtime_->backoff = std::chrono::seconds(1);
        this->dispatchState(State::Connected, {});
    }

    std::vector<YouTubeMessage> messages;
    const auto items = frame.value("items").toArray();
    messages.reserve(items.size());
    for (const auto &value : items)
    {
        if (!value.isObject())
        {
            continue;
        }
        auto message = parseYouTubeMessage(value.toObject());
        if (message.isIgnored())
        {
            continue;
        }
        if (!this->runtime_->deduper->accept(message))
        {
            continue;
        }
        messages.emplace_back(std::move(message));
    }
    if (!messages.empty())
    {
        const bool historical = type == u"snapshot"_s ||
                                frame.value("historical").toBool();
        this->dispatchMessages(std::move(messages), historical);
    }
}

void YouTubeLiveChatSession::streamFinished(QNetworkReply *reply)
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

    if (status >= 200 && status < 300 &&
        this->runtime_->parserError.isEmpty() &&
        !this->runtime_->buffer.trimmed().isEmpty())
    {
        this->processLine(this->runtime_->buffer);
    }

    this->runtime_->reply.clear();
    reply->deleteLater();

    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }
    if (!this->runtime_->parserError.isEmpty())
    {
        this->scheduleReconnect(this->runtime_->parserError);
        return;
    }

    const auto detail = responseError(this->runtime_->errorBody, fallback);
    if (status == 401 || status == 403 || status == 409)
    {
        this->finish(State::RefreshRequired, detail);
        return;
    }
    if (status == 404 || status == 410)
    {
        this->finish(State::Ended, detail);
        return;
    }
    if (status == 400)
    {
        this->finish(State::Failed, detail);
        return;
    }
    if (status == 429)
    {
        this->scheduleReconnect(detail,
                                serverDelay.value_or(std::chrono::minutes(1)));
        return;
    }

    this->scheduleReconnect(
        networkError == QNetworkReply::NoError
            ? u"Moltorino closed the YouTube chat stream."_s
            : detail,
        serverDelay);
}

void YouTubeLiveChatSession::scheduleReconnect(
    const QString &detail,
    std::optional<std::chrono::milliseconds> requestedDelay)
{
    if (!this->started_ || this->stopping_ || this->runtime_->terminal)
    {
        return;
    }
    this->cancelActiveCall();
    this->runtime_->connected = false;
    this->dispatchState(State::Reconnecting, detail);

    auto delay = requestedDelay.value_or(this->runtime_->backoff);
    if (!requestedDelay)
    {
        this->runtime_->backoff = std::min(
            this->runtime_->backoff * 2,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(30)));
        const auto jitter =
            static_cast<int>(QRandomGenerator::global()->bounded(401)) - 200;
        delay = std::max(std::chrono::milliseconds(250),
                         delay + delay * jitter / 1000);
    }
    this->runtime_->reconnectTimer.start(delay);
}

void YouTubeLiveChatSession::finish(State state, const QString &detail)
{
    if (this->runtime_->terminal)
    {
        return;
    }
    this->runtime_->terminal = true;
    this->started_ = false;
    this->runtime_->reconnectTimer.stop();
    this->cancelActiveCall();
    this->dispatchState(state, detail);
}

void YouTubeLiveChatSession::dispatchMessages(
    std::vector<YouTubeMessage> messages, bool historical)
{
    const auto generation = this->runtime_->callbackGeneration;
    QTimer::singleShot(
        0, this,
        [self = QPointer(this), generation, historical,
         messages = std::move(messages)]() mutable {
            if (self && self->runtime_->callbackGeneration == generation)
            {
                self->messagesCallback_(std::move(messages), historical);
            }
        });
}

void YouTubeLiveChatSession::dispatchState(State state, QString detail)
{
    const auto generation = this->runtime_->callbackGeneration;
    QTimer::singleShot(0, this,
                       [self = QPointer(this), generation, state,
                        detail = std::move(detail)] {
                           if (self &&
                               self->runtime_->callbackGeneration ==
                                   generation)
                           {
                               self->stateCallback_(state, detail);
                           }
                       });
}

}
