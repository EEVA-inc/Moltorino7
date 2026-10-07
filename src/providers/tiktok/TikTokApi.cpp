#include "providers/tiktok/TikTokApi.hpp"

#include "providers/tiktok/TikTokRequestSigning.hpp"
#include "providers/tiktok/TikTokText.hpp"
#include "providers/tiktok/TikTokTypes.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QUrlQuery>
#include <rapidjson/document.h>

#include <algorithm>
#include <string_view>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;
namespace {
constexpr qsizetype MAX_RESPONSE = 256 * 1024;

QByteArray jsonString(const QString &value)
{
    const auto array =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.sliced(1, array.size() - 2);
}

QString jsonID(const rapidjson::Value &object, const char *key)
{
    if (!object.IsObject() || !object.HasMember(key))
    {
        return {};
    }
    const auto &value = object[key];
    if (value.IsString())
    {
        return QString::fromUtf8(value.GetString(),
                                 qsizetype(value.GetStringLength()));
    }
    if (value.IsUint64())
    {
        return QString::number(value.GetUint64());
    }
    return {};
}

QString plainError(const QJsonObject &data)
{
    auto text = data.value("prompts").toString();
    if (text.isEmpty())
    {
        text = data.value("message").toString();
    }
    text.replace(u'\r', u' ');
    text.replace(u'\n', u' ');
    return text.simplified().left(300);
}

bool usableHeader(const QByteArray &value)
{
    return !value.isEmpty() && value.size() <= 16384 &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) {
               return c < 32 || c == 127;
           });
}
}

QUrl tikTokChatEndpoint(const TikTokSession &session)
{
    const QUrl origin(session.context.value("webcastHost").toString());
    if (!isTikTokAccountUrl(origin) ||
        (origin.host() != u"webcast.tiktok.com" &&
         origin.host() != u"webcast.us.tiktok.com") ||
        origin.hasQuery() || origin.hasFragment() ||
        (!origin.path().isEmpty() && origin.path() != u"/"))
    {
        return {};
    }
    return origin.resolved(QUrl(u"/webcast/room/chat/"_s));
}

QString tikTokSerializeQuery(const QMap<QString, QString> &parameters)
{
    QStringList parts;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
    {
        parts.push_back(QString::fromLatin1(
                            QUrl::toPercentEncoding(it.key(), {}, "!'()*")) +
                        u'=' +
                        QString::fromLatin1(
                            QUrl::toPercentEncoding(it.value(), {}, "!'()*")));
    }
    return parts.join(u'&');
}

QMap<QString, QString> tikTokChatParameters(const TikTokSession &session,
                                            const QString &roomID,
                                            const QString &content)
{
    QMap<QString, QString> params{
        {u"aid"_s, u"1988"_s},
        {u"app_name"_s, u"tiktok_web"_s},
        {u"channel"_s, u"tiktok_web"_s},
        {u"device_platform"_s, u"web_pc"_s},
        {u"cookie_enabled"_s, u"true"_s},
        {u"browser_online"_s, u"true"_s},
        {u"is_page_visible"_s, u"true"_s},
        {u"focus_state"_s, u"true"_s},
        {u"is_fullscreen"_s, u"false"_s},
        {u"history_len"_s, u"1"_s},
        {u"user_is_login"_s, u"true"_s},
        {u"data_collection_enabled"_s, u"true"_s},
        {u"referer"_s, u""_s},
        {u"root_referer"_s, u""_s},
        {u"from_page"_s, u""_s},
        {u"room_id"_s, roomID},
        {u"content"_s, content},
        {u"emotes_with_index"_s, u""_s},
        {u"input_type"_s, u"0"_s},

        {u"client_start_timestamp_millisecond"_s, u"0"_s}};
    const auto &context = session.context;
    for (const auto &[target, source] : QList<QPair<QString, QString>>{
             {u"device_id"_s, u"wid"_s},
             {u"region"_s, u"region"_s},
             {u"os"_s, u"os"_s},
             {u"app_language"_s, u"language"_s},
             {u"webcast_language"_s, u"language"_s},
             {u"browser_language"_s, u"browserLanguage"_s},
             {u"browser_platform"_s, u"browserPlatform"_s},
             {u"browser_name"_s, u"browserName"_s},
             {u"browser_version"_s, u"browserVersion"_s},
             {u"tz_name"_s, u"timezone"_s},
             {u"WebIdLastTime"_s, u"webIdCreatedTime"_s},
             {u"odinId"_s, u"odinId"_s}})
    {
        const auto value = context.value(source);
        if (value.isString())
        {
            params.insert(target, value.toString());
        }
        else if (value.isDouble())
        {
            params.insert(target, QString::number(value.toInteger()));
        }
    }
    params.insert(u"priority_region"_s,
                  context.value("user").toObject().value("region").toString());
    params.insert(u"screen_width"_s,
                  QString::number(context.value("screenWidth").toInt()));
    params.insert(u"screen_height"_s,
                  QString::number(context.value("screenHeight").toInt()));
    const auto fingerprint =
        session.cookieValue("s_v_web_id", QUrl(u"https://www.tiktok.com/"_s));
    if (!fingerprint.isEmpty())
    {
        params.insert(u"verifyFp"_s, QString::fromLatin1(fingerprint));
    }
    return params;
}

QByteArray tikTokChatBody(const QString &roomID, const QString &content)
{
    return "{\"room_id\":" + jsonString(roomID) +
           ",\"content\":" + jsonString(content) +
           ",\"emotes_with_index\":\"\",\"input_type\":0,\"client_start_"
           "timestamp_millisecond\":0}";
}

TikTokSendResult classifyTikTokSendResponse(const TikTokApiResponse &response)
{
    using Status = TikTokSendResult::Status;
    if (!response.dispatched)
    {
        return {Status::Rejected,
                {},
                u"The TikTok message could not be prepared."_s};
    }
    if (response.status == 401)
    {
        return {
            Status::LoginRequired,
            {},
            u"Your TikTok login expired. Connect the account again in Settings > Accounts."_s};
    }
    if (response.status == 429)
    {
        return {
            Status::Limited,
            {},
            u"TikTok is limiting messages. Wait a moment before trying again."_s};
    }
    if (response.headers.contains("bdturing-verify") || response.status == 403)
    {
        return {
            Status::Rejected,
            {},
            u"TikTok requires an account check. Connect the account again in Settings > Accounts."_s};
    }
    if ((!response.transportSucceeded &&
         !(response.status >= 400 && response.status < 500)) ||
        response.status >= 500)
    {
        return {
            Status::Unknown,
            {},
            u"Could not confirm whether TikTok received your message. Check the chat before trying again."_s};
    }
    const auto root = QJsonDocument::fromJson(response.body).object();
    const auto code = root.value("status_code");
    if (response.status == 200 && code.isDouble() && code.toInt(-1) == 0)
    {
        const auto id =
            root.value("data").toObject().value("msg_id_str").toString();
        return {Status::Accepted, isTikTokUserID(id) ? id : QString{}, {}};
    }
    if (code.isDouble() || (response.status >= 400 && response.status < 500))
    {
        const auto detail = plainError(root.value("data").toObject());
        return {Status::Rejected,
                {},
                detail.isEmpty() ? u"TikTok did not accept this message."_s
                                 : u"TikTok: "_s + detail};
    }
    return {
        Status::Unknown,
        {},
        u"Could not confirm whether TikTok received your message. Check the chat before trying again."_s};
}

TikTokApi::TikTokApi(QObject *parent, Transport transport)
    : QObject(parent)
    , transport_(std::move(transport))
{
}

void TikTokApi::networkRequest(TikTokApiRequest input,
                               ResponseCallback callback)
{
    QNetworkRequest request(input.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setTransferTimeout(20000);
    for (auto it = input.headers.cbegin(); it != input.headers.cend(); ++it)
    {
        request.setRawHeader(it.key(), it.value());
    }
    QNetworkReply *reply = nullptr;
    if (input.method == "POST")
    {
        reply = this->network_.post(request, input.body);
    }
    else if (input.method == "HEAD")
    {
        reply = this->network_.head(request);
    }
    else
    {
        reply = this->network_.get(request);
    }
    reply->setReadBufferSize(64 * 1024);
    auto body = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [reply, body, oversized] {
                         body->append(reply->readAll());
                         if (body->size() > MAX_RESPONSE)
                         {
                             *oversized = true;
                             reply->abort();
                         }
                     });
    QObject::connect(
        reply, &QNetworkReply::finished, this,
        [reply, body, oversized, callback = std::move(callback)]() mutable {
            body->append(reply->readAll());
            TikTokApiResponse response;
            response.status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            response.transportSucceeded =
                !*oversized && body->size() <= MAX_RESPONSE &&
                reply->error() == QNetworkReply::NoError;
            if (!*oversized && body->size() <= MAX_RESPONSE)
            {
                response.body = std::move(*body);
            }
            for (const auto &[name, value] : reply->rawHeaderPairs())
            {
                if (name.compare("set-cookie", Qt::CaseInsensitive) == 0)
                {
                    response.cookies.append(
                        QNetworkCookie::parseCookies(value));
                }
                else if (value.size() <= 16384)
                {
                    response.headers.insert(name.toLower(), value);
                }
            }
            reply->deleteLater();
            callback(std::move(response));
        });
}

void TikTokApi::request(const std::shared_ptr<TikTokAccount> &account,
                        TikTokApiRequest request, ResponseCallback callback)
{
    if (!account || !account->hasCredentials() ||
        !isTikTokAccountUrl(request.url))
    {
        callback({.dispatched = false});
        return;
    }
    request.headers.insert("User-Agent", account->session().userAgent.toUtf8());
    request.headers.insert("Cookie",
                           account->session().cookieHeader(request.url));
    request.headers.insert("Accept", "application/json");
    request.headers.insert("Origin", "https://www.tiktok.com");
    request.headers.insert("Referer", "https://www.tiktok.com/");
    for (const auto &value : request.headers)
    {
        if (!usableHeader(value))
        {
            callback({.dispatched = false});
            return;
        }
    }
    auto finished =
        [weak = QPointer(this), account, url = request.url,
         tokenStatus = account->session().msTokenStatus,
         callback = std::move(callback)](TikTokApiResponse response) mutable {
            if (!weak)
            {
                return;
            }

            if (account->hasCredentials())
            {
                account->session().updateCookies(response.cookies, url);
                const bool tokenChanged =
                    response.transportSucceeded && response.status >= 200 &&
                    response.status < 300 &&
                    account->session().updateMsToken(
                        response.headers.value("x-ms-token"), tokenStatus);
                if (url.path().startsWith(u"/passport/web/") ||
                    url.path().startsWith(u"/passport/token/beat/web/"))
                {
                    updateTikTokTicketGuard(
                        account->session(),
                        response.headers.value("tt-ticket-guard-server-data"));
                }
                if (tokenChanged || !response.cookies.isEmpty() ||
                    response.headers.contains("tt-ticket-guard-server-data"))
                {
                    weak->sessionChanged.invoke(account);
                }
            }
            callback(std::move(response));
        };
    if (this->transport_)
    {
        this->transport_(std::move(request), std::move(finished));
    }
    else
    {
        this->networkRequest(std::move(request), std::move(finished));
    }
}

void TikTokApi::csrf(const std::shared_ptr<TikTokAccount> &account,
                     const QUrl &url, TokenCallback callback)
{
    using Status = TikTokSendResult::Status;
    auto &session = account->session();
    if (session.csrfOrigin == url.host() &&
        session.csrfExpires > QDateTime::currentDateTimeUtc() &&
        usableHeader(session.csrfToken))
    {
        callback({});
        return;
    }
    TikTokApiRequest request{
        url,
        "HEAD",
        {{"x-secsdk-csrf-request", "1"}, {"x-secsdk-csrf-version", "1.2.22"}},
        {}};
    this->request(
        account, std::move(request),
        [weak = QPointer(this), account, url,
         callback = std::move(callback)](TikTokApiResponse response) mutable {
            if (!weak)
            {
                return;
            }
            if (response.status == 401 || response.status == 429)
            {
                if (response.status == 429)
                {
                    weak->limitedUntil_.insert(
                        account->userID(),
                        QDateTime::currentDateTimeUtc().addSecs(std::clamp(
                            response.headers.value("retry-after").toInt(), 5,
                            3600)));
                }
                callback(makeUnexpected(classifyTikTokSendResponse(response)));
                return;
            }
            const auto fields =
                response.headers.value("x-ware-csrf-token").split(',');
            if (!account->hasCredentials() || !response.transportSucceeded ||
                response.status != 200 || fields.size() < 3 ||
                fields[0] != "0" || !usableHeader(fields[1]))
            {
                callback(makeUnexpected(TikTokSendResult{
                    account->hasCredentials() ? Status::Rejected
                                              : Status::LoginRequired,
                    {},
                    u"TikTok could not authorize messaging. Connect the account again in Settings > Accounts."_s}));
                return;
            }
            bool validAge = false;
            auto age = fields[2].toLongLong(&validAge);
            if (!validAge)
            {
                age = 24 * 60 * 60 * 1000;
            }
            age = std::clamp<qint64>(age, 0, 24 * 60 * 60 * 1000);
            auto &session = account->session();
            session.csrfOrigin = url.host();
            session.csrfToken = fields[1];
            if (fields.size() > 4 && !fields[4].isEmpty())
            {
                if (!usableHeader(fields[4]))
                {
                    callback(makeUnexpected(TikTokSendResult{
                        Status::Rejected,
                        {},
                        u"TikTok returned an invalid messaging token."_s}));
                    return;
                }
                session.csrfToken += ',' + fields[4];
            }
            session.csrfExpires = QDateTime::currentDateTimeUtc().addMSecs(age);
            callback({});
        });
}

void TikTokApi::verifyAccount(TikTokAccountData data, VerifyCallback callback)
{
    const auto handle = normalizeTikTokHandle(data.handle);
    if (!isTikTokUserID(data.userID) || !handle || data.displayName.isEmpty())
    {
        callback(makeUnexpected(
            u"TikTok did not provide a complete login. Try connecting the account again."_s));
        return;
    }
    const auto stored = encodeTikTokSession(data.session);
    const auto signing = validateTikTokSigningSession(data.session);
    if (!stored || !signing)
    {
        callback(makeUnexpected(!stored ? stored.error() : signing.error()));
        return;
    }
    if (tikTokChatEndpoint(data.session).isEmpty())
    {
        callback(makeUnexpected(
            u"TikTok returned an unsupported live service address. Try connecting the account again."_s));
        return;
    }
    auto account = std::make_shared<TikTokAccount>(std::move(data));
    QMap<QString, QString> params{
        {u"aid"_s, u"1459"_s},
        {u"locale"_s,
         account->session().context.value("browserLanguage").toString()},
        {u"app_language"_s,
         account->session().context.value("language").toString()}};
    TikTokApiRequest request{
        QUrl(u"https://www.tiktok.com/passport/web/account/info/"_s),
        "GET",
        {{"x-tt-passport-force-refresh-cookie", "1"}},
        {}};
    request.url.setQuery(tikTokSerializeQuery(params));
    auto signedRequest =
        signTikTokAccountRequest(account->session(), request, true);
    if (!signedRequest)
    {
        callback(makeUnexpected(signedRequest.error()));
        return;
    }
    this->request(
        account, std::move(request),
        [weak = QPointer(this), account,
         callback = std::move(callback)](TikTokApiResponse response) mutable {
            if (!weak)
            {
                return;
            }
            rapidjson::Document json;
            json.Parse(response.body.constData(),
                       static_cast<size_t>(response.body.size()));
            QString id;
            bool accepted = false;
            if (!json.HasParseError() && json.IsObject() &&
                json.HasMember("data"))
            {
                accepted =
                    (json.HasMember("message") && json["message"].IsString() &&
                     std::string_view(json["message"].GetString(),
                                      json["message"].GetStringLength()) ==
                         "success") ||
                    (json.HasMember("status_code") &&
                     json["status_code"].IsInt() &&
                     json["status_code"].GetInt() == 0);
                id = jsonID(json["data"], "user_id_str");
                if (id.isEmpty())
                {
                    id = jsonID(json["data"], "user_id");
                }
            }
            if (!response.transportSucceeded || response.status != 200 ||
                !accepted || id != account->userID())
            {
                callback(makeUnexpected(
                    u"Moltorino could not verify this TikTok login. Try connecting the account again."_s));
                return;
            }
            weak->csrf(account, tikTokChatEndpoint(account->session()),
                       [account, callback = std::move(callback)](
                           TokenResult result) mutable {
                           if (!result)
                           {
                               callback(makeUnexpected(result.error().message));
                               return;
                           }
                           callback(TikTokAccountData{
                               account->userID(), account->handle(),
                               account->displayName(), account->session()});
                       });
        });
}

void TikTokApi::sendMessage(const std::shared_ptr<TikTokAccount> &account,
                            const QString &roomID, const QString &handle,
                            const QString &content, SendCallback callback,
                            std::function<bool()> isCurrent)
{
    using Status = TikTokSendResult::Status;
    if (isCurrent && !isCurrent())
    {
        callback(
            {Status::Rejected,
             {},
             u"The TikTok channel or account changed before this message was sent."_s});
        return;
    }
    if (!account || !account->hasCredentials())
    {
        callback(
            {Status::LoginRequired,
             {},
             u"Connect a TikTok account in Settings > Accounts to send messages."_s});
        return;
    }
    const auto endpoint = tikTokChatEndpoint(account->session());
    if (content.size() > 32768 ||
        tiktok::livetext::analyzeEditorText(content).count >
            TIKTOK_MESSAGE_LIMIT)
    {
        callback({Status::Rejected,
                  {},
                  u"TikTok messages can contain up to %1 characters."_s.arg(
                      TIKTOK_MESSAGE_LIMIT)});
        return;
    }
    if (!isTikTokUserID(roomID) || !normalizeTikTokHandle(handle) ||
        endpoint.isEmpty() || content.trimmed().isEmpty())
    {
        callback({Status::Rejected,
                  {},
                  u"This TikTok chat is not ready to send messages."_s});
        return;
    }
    const auto id = account->userID();
    if (this->limitedUntil_.value(id) > QDateTime::currentDateTimeUtc())
    {
        callback(
            {Status::Limited,
             {},
             u"TikTok is limiting messages. Wait a moment before trying again."_s});
        return;
    }
    if (this->pending_.value(id) >= 3)
    {
        callback(
            {Status::Rejected,
             {},
             u"Please wait for your pending TikTok messages to finish."_s});
        return;
    }
    ++this->pending_[id];
    auto finished = [weak = QPointer(this), id, callback = std::move(callback)](
                        TikTokSendResult result) mutable {
        if (!weak)
        {
            return;
        }
        if (--weak->pending_[id] <= 0)
        {
            weak->pending_.remove(id);
        }
        callback(std::move(result));
    };
    this->csrf(
        account, endpoint,
        [weak = QPointer(this), account, endpoint, roomID, content,
         isCurrent = std::move(isCurrent),
         finished = std::move(finished)](TokenResult token) mutable {
            if (!weak)
            {
                return;
            }
            if (isCurrent && !isCurrent())
            {
                finished(
                    {Status::Rejected,
                     {},
                     u"The TikTok channel or account changed before this message was sent."_s});
                return;
            }
            if (!token)
            {
                finished(std::move(token.error()));
                return;
            }
            if (!account->hasCredentials())
            {
                finished(
                    {Status::LoginRequired,
                     {},
                     u"The TikTok account was disconnected before this message was sent."_s});
                return;
            }
            TikTokApiRequest request{
                endpoint,
                "POST",
                {{"Content-Type", "application/json; charset=UTF-8"},
                 {"x-secsdk-csrf-token", account->session().csrfToken}},
                tikTokChatBody(roomID, content)};
            request.url.setQuery(tikTokSerializeQuery(
                tikTokChatParameters(account->session(), roomID, content)));
            auto signedRequest =
                signTikTokAccountRequest(account->session(), request, false);
            if (!signedRequest)
            {
                finished({Status::Rejected, {}, signedRequest.error()});
                return;
            }
            weak->request(
                account, std::move(request),
                [weak, account, finished = std::move(finished)](
                    TikTokApiResponse response) mutable {
                    if (!weak)
                    {
                        return;
                    }
                    auto result = classifyTikTokSendResponse(response);
                    if (result.status == Status::Limited)
                    {
                        const auto seconds = std::clamp(
                            response.headers.value("retry-after").toInt(), 5,
                            3600);
                        weak->limitedUntil_.insert(
                            account->userID(),
                            QDateTime::currentDateTimeUtc().addSecs(seconds));
                    }
                    if (result.status != Status::Accepted)
                    {
                        account->session().csrfExpires = {};
                    }
                    finished(std::move(result));
                });
        });
}
}
