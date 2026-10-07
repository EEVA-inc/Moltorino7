#include "providers/tiktok/TikTokRequestSigning.hpp"

#ifdef MOLTORINO_HAVE_TIKTOK_ACCOUNTS
#    include "providers/tiktok/TikTokSigningState.hpp"
#    include "providers/tiktok/TikTokTicketGuard.hpp"
#endif

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

#include <stdexcept>
#include <tuple>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

ExpectedStr<void> validateTikTokSigningSession(const TikTokSession &session)
{
#ifdef MOLTORINO_HAVE_TIKTOK_ACCOUNTS
    try
    {
        const auto state = tiktok::recoverSigningState(
            session.context.value("signingDynosaur").toString(),
            session.context.value("signingGnarly").toString(),
            session.userAgent);
        const auto now =
            static_cast<quint32>(QDateTime::currentSecsSinceEpoch());

        std::ignore = state.atTimestamp(now, {});
        std::ignore = state.atTimestamp(now + 1, {});
        std::ignore = tiktok::ticketguard::TicketGuard::importStorage(
            QJsonDocument(session.ticketGuardStorage)
                .toJson(QJsonDocument::Compact));
        return {};
    }
    catch (const std::exception &)
    {
        return makeUnexpected(
            u"TikTok returned an unsupported login session. Try connecting the account again."_s);
    }
#else
    return makeUnexpected(
        u"TikTok account support is unavailable in this build."_s);
#endif
}

ExpectedStr<void> signTikTokAccountRequest(const TikTokSession &session,
                                           TikTokApiRequest &request,
                                           bool provider)
{
#ifdef MOLTORINO_HAVE_TIKTOK_ACCOUNTS
    if (!isTikTokAccountUrl(request.url))
    {
        return makeUnexpected(u"The TikTok service address is invalid."_s);
    }
    try
    {
        const auto now =
            static_cast<quint32>(QDateTime::currentSecsSinceEpoch());
        const auto recovered = tiktok::recoverSigningState(
            session.context.value("signingDynosaur").toString(),
            session.context.value("signingGnarly").toString(),
            session.userAgent);
        const auto state = recovered.atTimestamp(now, session.msToken);
        const auto signatures =
            tiktok::signExactQuery(request.url.query(QUrl::FullyEncoded),
                                   QString::fromUtf8(request.body), state);
        const auto guard = tiktok::ticketguard::TicketGuard::importStorage(
            QJsonDocument(session.ticketGuardStorage)
                .toJson(QJsonDocument::Compact));
        const auto headers = guard.headersForRequest(
            request.url.toEncoded(), now,
            provider ? tiktok::ticketguard::EndpointRole::Provider
                     : tiktok::ticketguard::EndpointRole::Consumer);
        for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        {
            request.headers.insert(it.key(), it.value());
        }
        request.url.setQuery(signatures.signedQuery, QUrl::StrictMode);
        if (!request.url.isValid())
        {
            return makeUnexpected(
                u"The TikTok message could not be prepared."_s);
        }
        return {};
    }
    catch (const std::exception &)
    {
        return makeUnexpected(
            u"The TikTok login needs to be refreshed. Connect the account again in Settings > Accounts."_s);
    }
#else
    return makeUnexpected(
        u"TikTok account support is unavailable in this build."_s);
#endif
}

void updateTikTokTicketGuard(TikTokSession &session,
                             const QByteArray &serverData)
{
#ifdef MOLTORINO_HAVE_TIKTOK_ACCOUNTS
    if (serverData.isEmpty())
    {
        return;
    }
    try
    {
        auto guard = tiktok::ticketguard::TicketGuard::importStorage(
            QJsonDocument(session.ticketGuardStorage)
                .toJson(QJsonDocument::Compact));
        if (!guard.applyServerData(serverData,
                                   tiktok::ticketguard::EndpointRole::Provider))
        {
            return;
        }
        const auto entry =
            QJsonDocument::fromJson(QByteArray::fromBase64(serverData))
                .object()
                .value("tickets")
                .toArray()
                .first()
                .toObject();

        const auto data = QJsonDocument(entry).toJson(QJsonDocument::Compact);
        const auto wrapper =
            QJsonDocument(QJsonObject{{u"data"_s, QString::fromUtf8(data)}})
                .toJson(QJsonDocument::Compact);
        session.ticketGuardStorage.insert(
            u"security-sdk/s_sdk_sign_data_key/tt_fetch"_s,
            QString::fromUtf8(wrapper));
    }
    catch (const std::exception &)
    {
    }
#endif
}
}
