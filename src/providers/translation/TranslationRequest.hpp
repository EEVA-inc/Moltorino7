#pragma once

#include "providers/translation/Translator.hpp"

#include <QJsonDocument>
#include <QUrl>

namespace chatterino {
class NetworkResult;
}

namespace chatterino::translation::detail {

struct ConnectionSettings {
    QString libreEndpoint;
    QString azureRegion;
    QString azureEndpoint;
};

struct ProviderRequest {
    QUrl url;
    QJsonDocument body;
    std::vector<std::pair<QByteArray, QByteArray>> headers;
    int timeoutMs = 8000;
};

ExpectedStr<QUrl> normalizedServerEndpoint(TranslationProvider provider,
                                           QString endpoint);
ExpectedStr<ProviderRequest> makeProviderRequest(
    TranslationProvider provider, const QString &text, const QString &target,
    const QString &credential, const ConnectionSettings &settings = {},
    qsizetype googleClientIndex = 0);
std::optional<TranslationResult> parseProviderResponse(
    TranslationProvider provider, const NetworkResult &result);
QString failureMessage();

}
