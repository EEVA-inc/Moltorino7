#include "providers/translation/TranslationRequest.hpp"

#include "common/network/NetworkResult.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QUrlQuery>

namespace chatterino::translation::detail {

QString failureMessage()
{
    return QStringLiteral(
        "Translation failed. Try switching providers in Settings.");
}

ExpectedStr<QUrl> normalizedServerEndpoint(TranslationProvider provider,
                                           QString endpoint)
{
    const bool libre = provider == TranslationProvider::LibreTranslate;
    endpoint = endpoint.trimmed();
    if (endpoint.isEmpty())
    {
        endpoint = libre ? QStringLiteral("https://libretranslate.com")
                         : QStringLiteral(
                               "https://api.cognitive.microsofttranslator.com");
    }
    QUrl url(endpoint);
    const bool loopback = url.host() == QStringLiteral("localhost") ||
                          url.host() == QStringLiteral("127.0.0.1") ||
                          url.host() == QStringLiteral("::1");
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment() ||
        (url.scheme() != QStringLiteral("https") &&
         !(libre && loopback && url.scheme() == QStringLiteral("http"))))
    {
        return makeUnexpected(
            QStringLiteral("Enter a valid HTTPS server address without a "
                           "username, password, query or fragment."));
    }
    auto path = url.path();
    while (path.endsWith(QChar('/')))
    {
        path.chop(1);
    }
    if (path.endsWith(QStringLiteral("/translate"), Qt::CaseInsensitive))
    {
        path.chop(10);
    }
    url.setPath(path);
    return url;
}

ExpectedStr<ProviderRequest> makeProviderRequest(
    TranslationProvider provider, const QString &text, const QString &target,
    const QString &credential, const ConnectionSettings &settings,
    qsizetype googleClientIndex)
{
    ProviderRequest request;
    const auto language = providerTargetLanguage(provider, target);
    const auto key = credential.trimmed();
    if (key.contains(QChar('\r')) || key.contains(QChar('\n')))
    {
        return makeUnexpected(QStringLiteral("The API key contains a line "
                                             "break. Copy the key again."));
    }
    if (provider == TranslationProvider::GoogleBuiltIn)
    {
        const auto &clients = googleBuiltInClientIds();
        if (googleClientIndex < 0 || googleClientIndex >= clients.size())
        {
            return makeUnexpected(failureMessage());
        }
        request.url = QUrl(QStringLiteral(
            "https://translate.googleapis.com/translate_a/single"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client"),
                           clients.at(googleClientIndex));
        query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
        query.addQueryItem(QStringLiteral("tl"), language);
        query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
        query.addQueryItem(QStringLiteral("q"),
                           QString::fromLatin1(QUrl::toPercentEncoding(text)));
        request.url.setQuery(query);
        request.timeoutMs = 4000;
        return request;
    }
    if (provider == TranslationProvider::MicrosoftFree)
    {
        request.url = QUrl(QStringLiteral(
            "https://edge.microsoft.com/translate/translatetext"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("isEnterpriseClient"),
                           QStringLiteral("false"));
        query.addQueryItem(QStringLiteral("to"), language);
        request.url.setQuery(query);

        request.body = QJsonDocument(QJsonArray{
            text.toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>"))});
        return request;
    }
    if (provider != TranslationProvider::LibreTranslate && key.isEmpty())
    {
        return makeUnexpected(
            QStringLiteral("Set up %1 with an API key in Settings.")
                .arg(translationProviderName(provider)));
    }
    if (provider == TranslationProvider::DeepL)
    {
        request.url =
            QUrl(key.endsWith(QStringLiteral(":fx"), Qt::CaseInsensitive)
                     ? QStringLiteral("https://api-free.deepl.com/v2/translate")
                     : QStringLiteral("https://api.deepl.com/v2/translate"));
        request.body = QJsonDocument(QJsonObject{
            {QStringLiteral("text"), QJsonArray{text}},
            {QStringLiteral("target_lang"), language},
        });
        request.headers.emplace_back("Authorization",
                                     "DeepL-Auth-Key " + key.toUtf8());
    }
    else if (provider == TranslationProvider::LibreTranslate)
    {
        const auto endpoint =
            normalizedServerEndpoint(provider, settings.libreEndpoint);
        if (!endpoint)
        {
            return makeUnexpected(endpoint.error());
        }
        request.url = *endpoint;
        request.url.setPath(request.url.path() + QStringLiteral("/translate"));
        QJsonObject body{
            {QStringLiteral("q"), text},
            {QStringLiteral("source"), QStringLiteral("auto")},
            {QStringLiteral("target"), language},
            {QStringLiteral("format"), QStringLiteral("text")},
        };
        if (!key.isEmpty())
        {
            body.insert(QStringLiteral("api_key"), key);
        }
        request.body = QJsonDocument(body);
    }
    else if (provider == TranslationProvider::GoogleCloud)
    {
        request.url = QUrl(QStringLiteral(
            "https://translation.googleapis.com/language/translate/v2"));
        request.body = QJsonDocument(QJsonObject{
            {QStringLiteral("q"), text},
            {QStringLiteral("target"), language},
            {QStringLiteral("format"), QStringLiteral("text")},
        });
        request.headers.emplace_back("X-Goog-Api-Key", key.toUtf8());
    }
    else if (provider == TranslationProvider::MicrosoftAzure)
    {
        const auto endpoint =
            normalizedServerEndpoint(provider, settings.azureEndpoint);
        if (!endpoint)
        {
            return makeUnexpected(endpoint.error());
        }
        const auto region = settings.azureRegion.trimmed().toLower();
        static const QRegularExpression validRegion(
            QStringLiteral("^[a-z][a-z0-9-]{0,63}$"));
        if (!region.isEmpty() && !validRegion.match(region).hasMatch())
        {
            return makeUnexpected(QStringLiteral(
                "Enter the Azure resource region, for example eastus."));
        }
        request.url = *endpoint;
        auto path = request.url.path();
        if (path.isEmpty() && request.url.host().endsWith(QStringLiteral(
                                  ".cognitiveservices.azure.com")))
        {
            path = QStringLiteral("/translator/text/v3.0");
        }
        request.url.setPath(path + QStringLiteral("/translate"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("api-version"),
                           QStringLiteral("3.0"));
        query.addQueryItem(QStringLiteral("to"), language);
        request.url.setQuery(query);
        request.body = QJsonDocument(
            QJsonArray{QJsonObject{{QStringLiteral("Text"), text}}});
        request.headers.emplace_back("Ocp-Apim-Subscription-Key", key.toUtf8());
        if (!region.isEmpty())
        {
            request.headers.emplace_back("Ocp-Apim-Subscription-Region",
                                         region.toUtf8());
        }
    }
    else
    {
        return makeUnexpected(QStringLiteral("Choose a translation provider "
                                             "in Settings."));
    }
    return request;
}

std::optional<TranslationResult> parseProviderResponse(
    TranslationProvider provider, const NetworkResult &result)
{
    TranslationResult translated;
    if (provider == TranslationProvider::GoogleBuiltIn)
    {
        const auto root = result.parseJsonArray();
        const auto segments =
            root.isEmpty() ? QJsonArray{} : root.at(0).toArray();
        for (const auto &value : segments)
        {
            const auto segment = value.toArray();
            if (segment.isEmpty() || !segment.at(0).isString())
            {
                return std::nullopt;
            }
            translated.translatedText += segment.at(0).toString();
        }
        if (root.size() > 2)
        {
            translated.detectedLanguage = root.at(2).toString();
        }
    }
    else if (provider == TranslationProvider::MicrosoftAzure ||
             provider == TranslationProvider::MicrosoftFree)
    {
        const auto root = result.parseJsonArray();
        const auto first =
            root.isEmpty() ? QJsonObject{} : root.at(0).toObject();
        const auto translations =
            first.value(QStringLiteral("translations")).toArray();
        if (translations.isEmpty())
        {
            return std::nullopt;
        }
        translated.translatedText = translations.at(0)
                                        .toObject()
                                        .value(QStringLiteral("text"))
                                        .toString();
        if (provider == TranslationProvider::MicrosoftFree)
        {
            translated.translatedText =
                QTextDocumentFragment::fromHtml(translated.translatedText)
                    .toPlainText();
        }
        translated.detectedLanguage =
            first.value(QStringLiteral("detectedLanguage"))
                .toObject()
                .value(QStringLiteral("language"))
                .toString();
    }
    else
    {
        const auto root = result.parseJson();
        if (provider == TranslationProvider::DeepL)
        {
            const auto translations =
                root.value(QStringLiteral("translations")).toArray();
            if (translations.isEmpty())
            {
                return std::nullopt;
            }
            const auto first = translations.at(0).toObject();
            translated.translatedText =
                first.value(QStringLiteral("text")).toString();
            translated.detectedLanguage =
                first.value(QStringLiteral("detected_source_language"))
                    .toString();
        }
        else if (provider == TranslationProvider::LibreTranslate)
        {
            translated.translatedText =
                root.value(QStringLiteral("translatedText")).toString();
            translated.detectedLanguage =
                root.value(QStringLiteral("detectedLanguage"))
                    .toObject()
                    .value(QStringLiteral("language"))
                    .toString();
        }
        else if (provider == TranslationProvider::GoogleCloud)
        {
            const auto translations = root.value(QStringLiteral("data"))
                                          .toObject()
                                          .value(QStringLiteral("translations"))
                                          .toArray();
            if (translations.isEmpty())
            {
                return std::nullopt;
            }
            const auto first = translations.at(0).toObject();
            auto text =
                first.value(QStringLiteral("translatedText")).toString();

            text.replace(QChar('<'), QStringLiteral("&lt;"));
            text.replace(QChar('>'), QStringLiteral("&gt;"));
            translated.translatedText =
                QTextDocumentFragment::fromHtml(QStringLiteral("<pre>") + text +
                                                QStringLiteral("</pre>"))
                    .toPlainText();
            translated.detectedLanguage =
                first.value(QStringLiteral("detectedSourceLanguage"))
                    .toString();
        }
        else
        {
            return std::nullopt;
        }
    }
    translated.translatedText = translated.translatedText.trimmed();
    if (translated.translatedText.isEmpty())
    {
        return std::nullopt;
    }
    const auto detected =
        translationLanguageCodeFromInput(translated.detectedLanguage);
    translated.detectedLanguage =
        detected.isEmpty() ? normalizedLanguageCode(translated.detectedLanguage)
                           : detected;
    return translated;
}

}
