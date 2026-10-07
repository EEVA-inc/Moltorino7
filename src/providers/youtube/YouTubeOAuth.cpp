#include "providers/youtube/YouTubeOAuth.hpp"

#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeCredentialStore.hpp"
#include "providers/youtube/YouTubeCredentials.hpp"
#include "util/HttpServer.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

constexpr auto CALLBACK_PATH = "/oauth2/callback";
constexpr auto OAUTH_SCOPE =
    "https://www.googleapis.com/auth/youtube.force-ssl";

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < count; ++i)
    {
        bytes[i] = static_cast<char>(rng->generate() & 0xffU);
    }
    return bytes;
}

QString base64Url(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

bool includesRequiredScope(const QString &grantedScopes)
{
    if (grantedScopes.isEmpty())
    {
        return true;
    }
    return grantedScopes.split(' ', Qt::SkipEmptyParts)
        .contains(QString::fromLatin1(OAUTH_SCOPE));
}

QString accessDeniedMessage()
{
    return u"Google did not authorize Moltorino. Try again and approve the "
           u"requested YouTube access. If Google says access is blocked, "
           u"YouTube login is not currently available to that account."_s;
}

QByteArray browserPage(QStringView heading, QStringView detail)
{
    const auto html =
        u"<!doctype html><meta charset=utf-8><meta name=color-scheme "
        u"content=\"dark light\"><title>Moltorino YouTube</title>"
        u"<style>body{font:16px system-ui;max-width:560px;margin:12vh auto;"
        u"padding:28px;background:#101010;color:#eee;border:1px solid #333;"
        u"border-radius:12px}h1{font-size:22px}p{line-height:1.5;color:#bbb}"
        u"</style><h1>"_s +
        heading.toString().toHtmlEscaped() + u"</h1><p>"_s +
        detail.toString().toHtmlEscaped() + u"</p>"_s;
    return html.toUtf8();
}

class YouTubeOAuthFlow final : public QObject
{
public:
    YouTubeOAuthFlow(QWidget *parent, YouTubeOAuth::SuccessCallback success,
                     YouTubeOAuth::ErrorCallback error,
                     YouTubeOAuth::AuthorizationAcceptedCallback accepted)
        : QObject(parent)
        , success_(std::move(success))
        , error_(std::move(error))
        , accepted_(std::move(accepted))
    {
    }

    void begin()
    {
        const auto clientID = youtube::credentials::oauthClientID().trimmed();
        const auto clientSecret =
            youtube::credentials::oauthClientSecret().trimmed();
        if (clientID.isEmpty() || clientSecret.isEmpty())
        {
            this->fail(
                u"YouTube login is not configured in this build. "
                u"The builder must supply Google Desktop OAuth credentials."_s);
            return;
        }

        this->server_ = new HttpServer(0, this);
        if (!this->server_->isListening() || this->server_->serverPort() == 0)
        {
            this->fail(u"Could not start the local login callback: "_s +
                       this->server_->errorString());
            return;
        }

        this->verifier_ = base64Url(randomBytes(32));
        this->state_ = base64Url(randomBytes(32));
        const auto challenge = base64Url(QCryptographicHash::hash(
            this->verifier_.toUtf8(), QCryptographicHash::Sha256));
        this->redirectUri_ = u"http://127.0.0.1:"_s +
                             QString::number(this->server_->serverPort()) +
                             QString::fromLatin1(CALLBACK_PATH);

        this->server_->setRequestHandler(
            [self = QPointer(this)](const HttpServer::Request &request) {
                if (!self)
                {
                    return std::pair<unsigned, QByteArray>{404, {}};
                }
                return self->handleCallback(request);
            });

        QUrl authUrl(u"https://accounts.google.com/o/oauth2/v2/auth"_s);
        QUrlQuery query{
            {u"client_id"_s, clientID},
            {u"redirect_uri"_s, this->redirectUri_},
            {u"response_type"_s, u"code"_s},
            {u"scope"_s, QString::fromLatin1(OAUTH_SCOPE)},
            {u"code_challenge"_s, challenge},
            {u"code_challenge_method"_s, u"S256"_s},
            {u"state"_s, this->state_},
            {u"access_type"_s, u"offline"_s},
            {u"prompt"_s, u"consent select_account"_s},
        };
        authUrl.setQuery(query);

        if (!QDesktopServices::openUrl(authUrl))
        {
            this->fail(u"Could not open Google login in your browser."_s);
            return;
        }

        QTimer::singleShot(std::chrono::minutes(5), this, [this] {
            if (!this->handled_ && !this->finished_)
            {
                this->fail(u"YouTube login timed out. Please try again."_s);
            }
        });
    }

    void cancel()
    {
        if (this->handled_)
        {
            return;
        }
        this->fail(u"YouTube login was cancelled."_s);
    }

private:
    std::pair<unsigned, QByteArray> handleCallback(
        const HttpServer::Request &request)
    {
        if (this->handled_ || request.method != u"GET"_s)
        {
            return {405, browserPage(u"Request rejected"_s,
                                     u"Return to Moltorino and try again."_s)};
        }

        const QUrl callbackUrl(u"http://127.0.0.1"_s + request.target);
        if (callbackUrl.path() != QString::fromLatin1(CALLBACK_PATH))
        {
            return {404, {}};
        }
        const QUrlQuery query(callbackUrl);
        if (query.queryItemValue(u"state"_s) != this->state_)
        {
            return {400,
                    browserPage(u"Login rejected"_s,
                                u"The authorization state did not match."_s)};
        }

        this->handled_ = true;
        this->server_->close();
        const auto oauthError = query.queryItemValue(u"error"_s);
        if (!oauthError.isEmpty())
        {
            auto description =
                query.queryItemValue(u"error_description"_s).trimmed();

            description = description.left(512);
            QTimer::singleShot(
                0, this,
                [this, oauthError, description = std::move(description)] {
                    if (oauthError == u"access_denied"_s)
                    {
                        this->fail(accessDeniedMessage());
                        return;
                    }
                    if (!description.isEmpty())
                    {
                        this->fail(u"Google denied YouTube access: "_s +
                                   description);
                        return;
                    }
                    this->fail(u"Google login failed: "_s + oauthError);
                });
            return {200, browserPage(u"Login not completed"_s,
                                     u"You can close this tab."_s)};
        }

        const auto code = query.queryItemValue(u"code"_s);
        if (code.isEmpty())
        {
            QTimer::singleShot(0, this, [this] {
                this->fail(u"Google returned no authorization code."_s);
            });
            return {400, browserPage(u"Login failed"_s,
                                     u"Return to Moltorino and try again."_s)};
        }

        this->setParent(QCoreApplication::instance());
        auto accepted = std::move(this->accepted_);
        if (accepted)
        {
            accepted();
        }
        QTimer::singleShot(0, this, [this, code] {
            this->exchange(code);
        });
        return {200, browserPage(u"Finishing YouTube login"_s,
                                 u"You can close this tab."_s)};
    }

    void exchange(const QString &code)
    {
        YouTubeApi::exchangeCode(
            code, this->verifier_, this->redirectUri_,
            [self = QPointer(this)](
                Expected<YouTubeTokenResponse, YouTubeApiError> result) {
                if (!self)
                {
                    return;
                }
                if (!result)
                {
                    self->fail(u"Could not finish Google login: "_s +
                               result.error().message);
                    return;
                }

                self->issuedTokenToRevoke_ =
                    result->refreshToken.isEmpty() ? result->accessToken
                                                   : result->refreshToken;
                if (result->refreshToken.isEmpty())
                {
                    self->fail(
                        u"Google did not return an offline refresh token. "
                        u"Remove Moltorino from your Google account access "
                        u"and try connecting again."_s);
                    return;
                }
                if (!includesRequiredScope(result->grantedScope))
                {
                    self->fail(
                        u"Google did not grant the YouTube permission "
                        u"Moltorino needs. Try again and approve the requested "
                        u"YouTube access."_s);
                    return;
                }
                self->loadIdentity(std::move(*result));
            });
    }

    void loadIdentity(YouTubeTokenResponse tokens)
    {
        const auto accessToken = tokens.accessToken;
        YouTubeApi::getOwnChannelAuthenticated(
            accessToken, [self = QPointer(this), tokens = std::move(tokens)](
                             Expected<YouTubeOwnChannel, YouTubeApiError>
                                 result) mutable {
                if (!self)
                {
                    return;
                }
                if (!result)
                {
                    self->fail(u"Could not load your YouTube channel: "_s +
                               result.error().message);
                    return;
                }
                if (result->channelID.trimmed().isEmpty() ||
                    result->displayName.trimmed().isEmpty())
                {
                    self->fail(
                        u"Google did not return a usable YouTube channel. "
                        u"Make sure this Google account has a YouTube "
                        u"channel, then try again."_s);
                    return;
                }

                const auto refreshToken = tokens.refreshToken;
                writeYouTubeCredential(
                    result->channelID, refreshToken,
                    [self, identity = *result, tokens = std::move(tokens)](
                        ExpectedStr<void> stored) mutable {
                        if (!self)
                        {
                            return;
                        }
                        if (!stored)
                        {
                            self->fail(
                                u"Could not save YouTube credentials in your "
                                u"settings: "_s +
                                stored.error());
                            return;
                        }
                        self->succeed(YouTubeAccountData{
                            .channelID = identity.channelID,
                            .handle = identity.handle,
                            .displayName = identity.displayName,
                            .avatarUrl = identity.avatarUrl,
                            .accessToken = tokens.accessToken,
                            .refreshToken = tokens.refreshToken,
                            .expiresAt = tokens.expiresAt,
                        });
                    });
            });
    }

    void succeed(YouTubeAccountData data)
    {
        if (this->finished_)
        {
            return;
        }
        this->finished_ = true;
        if (this->server_)
        {
            this->server_->close();
        }
        this->verifier_.clear();
        this->state_.clear();
        this->redirectUri_.clear();
        this->issuedTokenToRevoke_.clear();
        auto success = std::move(this->success_);
        this->error_ = {};
        this->accepted_ = {};
        this->deleteLater();
        if (success)
        {
            success(std::move(data));
        }
    }

    void fail(const QString &message)
    {
        if (this->finished_)
        {
            return;
        }
        this->finished_ = true;
        if (this->server_)
        {
            this->server_->close();
        }
        this->verifier_.clear();
        this->state_.clear();
        this->redirectUri_.clear();
        if (!this->issuedTokenToRevoke_.isEmpty())
        {
            YouTubeApi::revokeToken(this->issuedTokenToRevoke_, [](auto) {});
            this->issuedTokenToRevoke_.clear();
        }
        auto error = std::move(this->error_);
        this->success_ = {};
        this->accepted_ = {};
        this->deleteLater();
        if (error)
        {
            error(message);
        }
    }

    YouTubeOAuth::SuccessCallback success_;
    YouTubeOAuth::ErrorCallback error_;
    YouTubeOAuth::AuthorizationAcceptedCallback accepted_;
    HttpServer *server_ = nullptr;
    QString verifier_;
    QString state_;
    QString redirectUri_;
    QString issuedTokenToRevoke_;
    bool handled_ = false;
    bool finished_ = false;
};

}

namespace chatterino {

YouTubeOAuth::CancelCallback YouTubeOAuth::start(
    QWidget *parent, SuccessCallback success, ErrorCallback error,
    AuthorizationAcceptedCallback accepted)
{
    auto *flow = new YouTubeOAuthFlow(parent, std::move(success),
                                      std::move(error), std::move(accepted));
    flow->begin();
    return [flow = QPointer(flow)] {
        if (flow)
        {
            flow->cancel();
        }
    };
}

}
