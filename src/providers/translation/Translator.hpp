#pragma once

#include "util/Expected.hpp"

#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QObject;

namespace chatterino {

struct TranslationLanguage {
    QString code;
    QString name;
};

struct TranslationResult {
    QString translatedText;
    QString detectedLanguage;
};

enum class TranslationProvider {
    GoogleBuiltIn,
    DeepL,
    LibreTranslate,
    GoogleCloud,
    MicrosoftAzure,
    MicrosoftFree,
};

struct TranslationProviderDescriptor {
    TranslationProvider provider;
    QString id;
    QString name;
    bool usesApiKey;
    bool apiKeyOptional;
};

using TranslationSuccessCallback =
    std::function<void(const TranslationResult &result)>;
using TranslationErrorCallback = std::function<void(const QString &error)>;
using TranslationFinishedCallback = std::function<void()>;

using TranslationCredentialCallback = std::function<void(ExpectedStr<QString>)>;
using TranslationCredentialWriteCallback =
    std::function<void(ExpectedStr<void>)>;

const std::vector<TranslationProviderDescriptor> &translationProviders();
TranslationProvider translationProviderFromId(const QString &id);
QString translationProviderId(TranslationProvider provider);
QString translationProviderName(TranslationProvider provider);
QString translationProviderCredentialKey(TranslationProvider provider);
void readTranslationProviderCredential(TranslationProvider provider,
                                       TranslationCredentialCallback callback,
                                       bool refresh = false);
void writeTranslationProviderCredential(
    TranslationProvider provider, const QString &credential,
    TranslationCredentialWriteCallback callback = {});
void removeTranslationProviderCredential(
    TranslationProvider provider,
    TranslationCredentialWriteCallback callback = {});

QString normalizedLanguageCode(QString language);
QString translationLanguageCodeFromInput(QString language);
QString normalizedTranslationTargetLanguage(QString language);
QString providerTargetLanguage(TranslationProvider provider, QString target);
const QStringList &googleBuiltInClientIds();
QString translationLanguageName(const QString &language);
bool isSupportedTranslationLanguage(const QString &language);
QString trimTextForTranslation(QString text);
const std::vector<TranslationLanguage> &supportedTranslationLanguages();

QString encodeMorseText(const QString &text);
std::optional<QString> decodeMorseText(const QString &text);
bool isMorseText(const QString &text);

void requestTextTranslation(
    const QString &text, const QString &targetLanguage, QObject *caller,
    TranslationSuccessCallback onSuccess, TranslationErrorCallback onError,
    TranslationFinishedCallback onFinished = {},
    std::optional<TranslationProvider> providerOverride = std::nullopt);

}  // namespace chatterino
