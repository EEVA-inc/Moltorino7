#include "providers/translation/Translator.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/translation/TranslationRequest.hpp"
#include "singletons/Settings.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <optional>

namespace chatterino {

namespace {

constexpr int MAX_TRANSLATION_TEXT_LENGTH = 1200;
constexpr qsizetype MAX_TRANSLATION_RESPONSE_BYTES = 1024 * 1024;

const QHash<QChar, QString> &morseAlphabet()
{
    static const QHash<QChar, QString> alphabet{
        {'A', QStringLiteral(".-")},      {'B', QStringLiteral("-...")},
        {'C', QStringLiteral("-.-.")},    {'D', QStringLiteral("-..")},
        {'E', QStringLiteral(".")},       {'F', QStringLiteral("..-.")},
        {'G', QStringLiteral("--.")},     {'H', QStringLiteral("....")},
        {'I', QStringLiteral("..")},      {'J', QStringLiteral(".---")},
        {'K', QStringLiteral("-.-")},     {'L', QStringLiteral(".-..")},
        {'M', QStringLiteral("--")},      {'N', QStringLiteral("-.")},
        {'O', QStringLiteral("---")},     {'P', QStringLiteral(".--.")},
        {'Q', QStringLiteral("--.-")},    {'R', QStringLiteral(".-.")},
        {'S', QStringLiteral("...")},     {'T', QStringLiteral("-")},
        {'U', QStringLiteral("..-")},     {'V', QStringLiteral("...-")},
        {'W', QStringLiteral(".--")},     {'X', QStringLiteral("-..-")},
        {'Y', QStringLiteral("-.--")},    {'Z', QStringLiteral("--..")},
        {'0', QStringLiteral("-----")},   {'1', QStringLiteral(".----")},
        {'2', QStringLiteral("..---")},   {'3', QStringLiteral("...--")},
        {'4', QStringLiteral("....-")},   {'5', QStringLiteral(".....")},
        {'6', QStringLiteral("-....")},   {'7', QStringLiteral("--...")},
        {'8', QStringLiteral("---..")},   {'9', QStringLiteral("----.")},
        {'.', QStringLiteral(".-.-.-")},  {',', QStringLiteral("--..--")},
        {'?', QStringLiteral("..--..")},  {'\'', QStringLiteral(".----.")},
        {'!', QStringLiteral("-.-.--")},  {'/', QStringLiteral("-..-.")},
        {'(', QStringLiteral("-.--.")},   {')', QStringLiteral("-.--.-")},
        {'&', QStringLiteral(".-...")},   {':', QStringLiteral("---...")},
        {';', QStringLiteral("-.-.-.")},  {'=', QStringLiteral("-...-")},
        {'+', QStringLiteral(".-.-.")},   {'-', QStringLiteral("-....-")},
        {'_', QStringLiteral("..--.-")},  {'"', QStringLiteral(".-..-.")},
        {'$', QStringLiteral("...-..-")}, {'@', QStringLiteral(".--.-.")},
    };
    return alphabet;
}

const QHash<QString, QString> &morseDecodingAlphabet()
{
    static const auto alphabet = [] {
        QHash<QString, QString> decoded;
        for (auto it = morseAlphabet().cbegin(); it != morseAlphabet().cend();
             ++it)
        {
            decoded.insert(it.value(), QString(it.key()));
        }
        return decoded;
    }();
    return alphabet;
}

const QRegularExpression &protectedEmotePlaceholder()
{
    static const QRegularExpression placeholder(
        QStringLiteral(R"(^MOLTOEMOTE\d{4}$)"));
    return placeholder;
}

QString normalizeMorseNotation(QString text)
{
    for (qsizetype i = 0; i < text.size(); ++i)
    {
        switch (text.at(i).unicode())
        {
            case 0x00B7:
            case 0x2022:
            case 0x22C5:
                text[i] = QLatin1Char('.');
                break;
            case 0x2212:
            case 0x2013:
            case 0x2014:
            case '_':
                text[i] = QLatin1Char('-');
                break;
            default:
                break;
        }
    }
    return text.trimmed();
}

QString encodeMorseWord(const QString &word)
{
    if (protectedEmotePlaceholder().match(word).hasMatch())
    {
        return word;
    }

    const auto normalized =
        word.normalized(QString::NormalizationForm_D).toUpper();
    QStringList encoded;
    encoded.reserve(normalized.size());
    const auto &alphabet = morseAlphabet();
    for (const auto codePoint : normalized.toUcs4())
    {
        if (QChar::category(static_cast<char32_t>(codePoint)) ==
            QChar::Mark_NonSpacing)
        {
            continue;
        }

        if (codePoint <= 0xFFFF)
        {
            const QChar character(static_cast<char16_t>(codePoint));
            const auto it = alphabet.constFind(character);
            encoded.push_back(it == alphabet.cend() ? QString(character) : *it);
        }
        else
        {
            const auto unicodeCharacter = static_cast<char32_t>(codePoint);
            encoded.push_back(QString::fromUcs4(&unicodeCharacter, 1));
        }
    }
    return encoded.join(QLatin1Char(' '));
}

QString encodeTextToMorse(const QString &text)
{
    QStringList encodedWords;
    for (const auto &word : text.split(
             QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
    {
        encodedWords.push_back(encodeMorseWord(word));
    }
    return encodedWords.join(QStringLiteral(" / "));
}

std::optional<QString> decodeTextFromMorse(const QString &text,
                                           int minimumTokenCount,
                                           bool requireLanguageConfidence)
{
    const auto normalized = normalizeMorseNotation(text);
    if (normalized.isEmpty())
    {
        return std::nullopt;
    }

    static const QRegularExpression separator(QStringLiteral(R"([/|\s]+)"));
    static const QRegularExpression morseToken(QStringLiteral(R"(^[.-]+$)"));

    QString currentWord;
    QStringList decodedWords;
    QSet<QString> distinctMorseTokens;
    int morseTokenCount = 0;
    bool hasExplicitWordSeparator = false;
    bool hasCompactSos = false;

    const auto finishWord = [&] {
        if (!currentWord.isEmpty())
        {
            decodedWords.push_back(std::move(currentWord));
            currentWord.clear();
        }
    };

    const auto decodeToken = [&](const QString &token) {
        if (token.isEmpty())
        {
            return true;
        }

        if (protectedEmotePlaceholder().match(token).hasMatch())
        {
            finishWord();
            decodedWords.push_back(token);
            return true;
        }

        if (morseToken.match(token).hasMatch())
        {
            if (token == QStringLiteral("...---..."))
            {
                currentWord += QStringLiteral("SOS");
                distinctMorseTokens.insert(QStringLiteral("..."));
                distinctMorseTokens.insert(QStringLiteral("---"));
                morseTokenCount += 3;
                hasCompactSos = true;
                return true;
            }

            const auto it = morseDecodingAlphabet().constFind(token);
            if (it == morseDecodingAlphabet().cend())
            {
                return false;
            }

            currentWord += *it;
            distinctMorseTokens.insert(token);
            ++morseTokenCount;
            return true;
        }

        for (const auto codePoint : token.toUcs4())
        {
            if (QChar::isLetterOrNumber(static_cast<char32_t>(codePoint)))
            {
                return false;
            }
        }

        finishWord();
        decodedWords.push_back(token);
        return true;
    };

    qsizetype tokenStart = 0;
    auto separators = separator.globalMatch(normalized);
    while (separators.hasNext())
    {
        const auto match = separators.next();
        if (!decodeToken(
                normalized.mid(tokenStart, match.capturedStart() - tokenStart)))
        {
            return std::nullopt;
        }

        if (match.captured() != QStringLiteral(" "))
        {
            hasExplicitWordSeparator = true;
            finishWord();
        }
        tokenStart = match.capturedEnd();
    }

    if (!decodeToken(normalized.mid(tokenStart)))
    {
        return std::nullopt;
    }
    finishWord();

    if (morseTokenCount < minimumTokenCount || decodedWords.isEmpty())
    {
        return std::nullopt;
    }

    const auto decoded = decodedWords.join(QLatin1Char(' '));
    if (requireLanguageConfidence && !hasCompactSos &&
        !hasExplicitWordSeparator)
    {
        static const QRegularExpression vowel(QStringLiteral("[AEIOU]"));
        if (distinctMorseTokens.size() < 2 || !decoded.contains(vowel))
        {
            return std::nullopt;
        }
    }

    return decoded;
}

QString providerErrorDetail(const NetworkResult &result)
{
    const auto root = result.parseJson();
    auto detail = root.value(QStringLiteral("message")).toString().trimmed();
    if (detail.isEmpty())
    {
        detail = root.value(QStringLiteral("error"))
                     .toObject()
                     .value(QStringLiteral("message"))
                     .toString()
                     .trimmed();
    }
    if (detail.isEmpty())
    {
        detail = root.value(QStringLiteral("error")).toString().trimmed();
    }
    detail.replace(QRegularExpression(QStringLiteral("\\s+")),
                   QStringLiteral(" "));
    return detail.left(240);
}

struct GoogleTranslationCallbacks {
    TranslationSuccessCallback onSuccess;
    TranslationErrorCallback onError;
    TranslationFinishedCallback onFinished;
    QPointer<QObject> caller;
    bool hadCaller = false;
    bool finished = false;
};

void finishGoogleRequest(
    const std::shared_ptr<GoogleTranslationCallbacks> &callbacks,
    const std::optional<TranslationResult> &result)
{
    if (callbacks->finished || (callbacks->hadCaller && !callbacks->caller))
    {
        return;
    }
    callbacks->finished = true;
    if (result && callbacks->onSuccess)
    {
        callbacks->onSuccess(*result);
    }
    else if (!result && callbacks->onError)
    {
        callbacks->onError(translation::detail::failureMessage());
    }
    if ((!callbacks->hadCaller || callbacks->caller) && callbacks->onFinished)
    {
        callbacks->onFinished();
    }
}

void executeGoogleBuiltInRequest(
    const QString &text, const QString &target, bool sourceWasMorse,
    qsizetype clientIndex,
    const std::shared_ptr<GoogleTranslationCallbacks> &callbacks)
{
    if (callbacks->finished || (callbacks->hadCaller && !callbacks->caller))
    {
        return;
    }
    const auto built = translation::detail::makeProviderRequest(
        TranslationProvider::GoogleBuiltIn, text, target, {}, {}, clientIndex);
    if (!built)
    {
        finishGoogleRequest(callbacks, std::nullopt);
        return;
    }

    auto next = [text, target, sourceWasMorse, clientIndex, callbacks] {
        executeGoogleBuiltInRequest(text, target, sourceWasMorse,
                                    clientIndex + 1, callbacks);
    };
    auto request =
        NetworkRequest(built->url, NetworkRequestType::Get)
            .timeout(built->timeoutMs)
            .maximumResponseSize(MAX_TRANSLATION_RESPONSE_BYTES)
            .followRedirects(false)
            .hideRequestBody()
            .onSuccess(
                [sourceWasMorse, callbacks, next](const NetworkResult &result) {
                    auto parsed = translation::detail::parseProviderResponse(
                        TranslationProvider::GoogleBuiltIn, result);
                    if (!parsed)
                    {
                        next();
                        return;
                    }
                    if (sourceWasMorse)
                    {
                        parsed->detectedLanguage = QStringLiteral("morse");
                    }
                    finishGoogleRequest(callbacks, parsed);
                })
            .onError([next](const NetworkResult &) {
                next();
            });
    if (callbacks->caller)
    {
        request = std::move(request).caller(callbacks->caller.data());
    }
    std::move(request).execute();
}

void executeProviderRequest(
    TranslationProvider provider, const QString &text, const QString &target,
    const QString &credential,
    const translation::detail::ConnectionSettings &connection, QObject *caller,
    bool sourceWasMorse, TranslationSuccessCallback onSuccess,
    TranslationErrorCallback onError, TranslationFinishedCallback onFinished)
{
    if (provider == TranslationProvider::GoogleBuiltIn)
    {
        auto callbacks = std::make_shared<GoogleTranslationCallbacks>(
            std::move(onSuccess), std::move(onError), std::move(onFinished),
            QPointer<QObject>(caller), caller != nullptr);
        executeGoogleBuiltInRequest(text, target, sourceWasMorse, 0, callbacks);
        return;
    }

    const auto built = translation::detail::makeProviderRequest(
        provider, text, target, credential, connection);
    if (!built)
    {
        const QPointer<QObject> guard(caller);
        const bool hadCaller = caller != nullptr;
        if (onError)
        {
            onError(built.error());
        }
        if ((!hadCaller || guard) && onFinished)
        {
            onFinished();
        }
        return;
    }
    auto request =
        NetworkRequest(built->url, NetworkRequestType::Post)
            .timeout(built->timeoutMs)
            .maximumResponseSize(MAX_TRANSLATION_RESPONSE_BYTES)
            .followRedirects(false)
            .hideRequestBody()
            .json(built->body)
            .headerList(built->headers)
            .onSuccess([provider, onSuccess, onError,
                        sourceWasMorse](const NetworkResult &result) {
                auto parsed = translation::detail::parseProviderResponse(
                    provider, result);
                if (!parsed)
                {
                    if (onError)
                    {
                        onError(translation::detail::failureMessage());
                    }
                    return;
                }
                if (sourceWasMorse)
                {
                    parsed->detectedLanguage = QStringLiteral("morse");
                }
                if (onSuccess)
                {
                    onSuccess(*parsed);
                }
            })
            .onError([provider, onError](const NetworkResult &result) {
                if (!onError)
                {
                    return;
                }
                const auto detail = providerErrorDetail(result);
                onError(detail.isEmpty()
                            ? translation::detail::failureMessage()
                            : QStringLiteral("%1: %2").arg(
                                  translationProviderName(provider), detail));
            })
            .finally([onFinished] {
                if (onFinished)
                {
                    onFinished();
                }
            });
    if (caller != nullptr)
    {
        request = std::move(request).caller(caller);
    }
    std::move(request).execute();
}

}  // namespace

QString providerTargetLanguage(TranslationProvider provider, QString target)
{
    target = normalizedTranslationTargetLanguage(std::move(target));
    switch (provider)
    {
        case TranslationProvider::DeepL:
            if (target == QStringLiteral("en"))
            {
                return QStringLiteral("EN-US");
            }
            if (target == QStringLiteral("pt"))
            {
                return QStringLiteral("PT-PT");
            }
            if (target == QStringLiteral("zh-cn"))
            {
                return QStringLiteral("ZH-HANS");
            }
            if (target == QStringLiteral("zh-tw"))
            {
                return QStringLiteral("ZH-HANT");
            }
            if (target == QStringLiteral("iw"))
            {
                return QStringLiteral("HE");
            }
            if (target == QStringLiteral("tl"))
            {
                return QStringLiteral("FIL");
            }
            return target.toUpper();
        case TranslationProvider::MicrosoftAzure:
        case TranslationProvider::MicrosoftFree:
            if (target == QStringLiteral("zh-cn"))
            {
                return QStringLiteral("zh-Hans");
            }
            if (target == QStringLiteral("zh-tw"))
            {
                return QStringLiteral("zh-Hant");
            }
            if (target == QStringLiteral("iw"))
            {
                return QStringLiteral("he");
            }
            if (target == QStringLiteral("tl"))
            {
                return QStringLiteral("fil");
            }
            if (target == QStringLiteral("ku"))
            {
                return QStringLiteral("kmr");
            }
            if (target == QStringLiteral("mn"))
            {
                return QStringLiteral("mn-Cyrl");
            }
            if (target == QStringLiteral("no"))
            {
                return QStringLiteral("nb");
            }
            if (target == QStringLiteral("sr"))
            {
                return QStringLiteral("sr-Cyrl");
            }
            return target;
        case TranslationProvider::GoogleCloud:
        case TranslationProvider::LibreTranslate:
            if (target == QStringLiteral("iw"))
            {
                return QStringLiteral("he");
            }
            if (target == QStringLiteral("tl"))
            {
                return provider == TranslationProvider::GoogleCloud
                           ? QStringLiteral("fil")
                           : QStringLiteral("tl");
            }
            return target;
        case TranslationProvider::GoogleBuiltIn:
            return target;
    }
    return target;
}

const QStringList &googleBuiltInClientIds()
{
    static const QStringList clientIds{
        QStringLiteral("dict-chrome-ex"),
        QStringLiteral("it"),
        QStringLiteral("at"),
    };
    return clientIds;
}

const std::vector<TranslationProviderDescriptor> &translationProviders()
{
    static const std::vector<TranslationProviderDescriptor> providers{
        {TranslationProvider::GoogleBuiltIn, QStringLiteral("google"),
         QStringLiteral("Google (free)"), false, false},
        {TranslationProvider::MicrosoftFree, QStringLiteral("microsoft-free"),
         QStringLiteral("Microsoft (free)"), false, false},
        {TranslationProvider::DeepL, QStringLiteral("deepl"),
         QStringLiteral("DeepL"), true, false},
        {TranslationProvider::LibreTranslate, QStringLiteral("libretranslate"),
         QStringLiteral("LibreTranslate"), true, true},
        {TranslationProvider::GoogleCloud, QStringLiteral("google-cloud"),
         QStringLiteral("Google Cloud Translation"), true, false},
        {TranslationProvider::MicrosoftAzure, QStringLiteral("azure"),
         QStringLiteral("Microsoft Azure Translator"), true, false},
    };
    return providers;
}

TranslationProvider translationProviderFromId(const QString &id)
{
    const auto normalized = id.trimmed().toLower();
    const auto &providers = translationProviders();
    const auto it = std::ranges::find_if(
        providers, [&normalized](const TranslationProviderDescriptor &item) {
            return item.id == normalized;
        });
    return it == providers.end() ? TranslationProvider::GoogleBuiltIn
                                 : it->provider;
}

QString translationProviderId(TranslationProvider provider)
{
    const auto &providers = translationProviders();
    const auto it = std::ranges::find_if(
        providers, [provider](const TranslationProviderDescriptor &item) {
            return item.provider == provider;
        });
    return it == providers.end() ? QStringLiteral("google") : it->id;
}

QString translationProviderName(TranslationProvider provider)
{
    const auto &providers = translationProviders();
    const auto it = std::ranges::find_if(
        providers, [provider](const TranslationProviderDescriptor &item) {
            return item.provider == provider;
        });
    return it == providers.end() ? QStringLiteral("Google Translate")
                                 : it->name;
}

QString normalizedLanguageCode(QString language)
{
    language = language.trimmed().toLower();
    language.replace('_', '-');

    return language;
}

const std::vector<TranslationLanguage> &supportedTranslationLanguages()
{
    static const std::vector<TranslationLanguage> languages{
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("es"), QStringLiteral("Spanish")},
        {QStringLiteral("pt"), QStringLiteral("Portuguese")},
        {QStringLiteral("fr"), QStringLiteral("French")},
        {QStringLiteral("de"), QStringLiteral("German")},
        {QStringLiteral("it"), QStringLiteral("Italian")},
        {QStringLiteral("nl"), QStringLiteral("Dutch")},
        {QStringLiteral("pl"), QStringLiteral("Polish")},
        {QStringLiteral("tr"), QStringLiteral("Turkish")},
        {QStringLiteral("ru"), QStringLiteral("Russian")},
        {QStringLiteral("ja"), QStringLiteral("Japanese")},
        {QStringLiteral("ko"), QStringLiteral("Korean")},
        {QStringLiteral("zh-cn"), QStringLiteral("Chinese (Simplified)")},
        {QStringLiteral("zh-tw"), QStringLiteral("Chinese (Traditional)")},
        {QStringLiteral("ar"), QStringLiteral("Arabic")},
        {QStringLiteral("af"), QStringLiteral("Afrikaans")},
        {QStringLiteral("sq"), QStringLiteral("Albanian")},
        {QStringLiteral("am"), QStringLiteral("Amharic")},
        {QStringLiteral("hy"), QStringLiteral("Armenian")},
        {QStringLiteral("az"), QStringLiteral("Azerbaijani")},
        {QStringLiteral("eu"), QStringLiteral("Basque")},
        {QStringLiteral("be"), QStringLiteral("Belarusian")},
        {QStringLiteral("bn"), QStringLiteral("Bengali")},
        {QStringLiteral("bs"), QStringLiteral("Bosnian")},
        {QStringLiteral("bg"), QStringLiteral("Bulgarian")},
        {QStringLiteral("ca"), QStringLiteral("Catalan")},
        {QStringLiteral("ceb"), QStringLiteral("Cebuano")},
        {QStringLiteral("co"), QStringLiteral("Corsican")},
        {QStringLiteral("hr"), QStringLiteral("Croatian")},
        {QStringLiteral("cs"), QStringLiteral("Czech")},
        {QStringLiteral("da"), QStringLiteral("Danish")},
        {QStringLiteral("eo"), QStringLiteral("Esperanto")},
        {QStringLiteral("et"), QStringLiteral("Estonian")},
        {QStringLiteral("fi"), QStringLiteral("Finnish")},
        {QStringLiteral("fy"), QStringLiteral("Frisian")},
        {QStringLiteral("gl"), QStringLiteral("Galician")},
        {QStringLiteral("ka"), QStringLiteral("Georgian")},
        {QStringLiteral("el"), QStringLiteral("Greek")},
        {QStringLiteral("gu"), QStringLiteral("Gujarati")},
        {QStringLiteral("ht"), QStringLiteral("Haitian Creole")},
        {QStringLiteral("ha"), QStringLiteral("Hausa")},
        {QStringLiteral("haw"), QStringLiteral("Hawaiian")},
        {QStringLiteral("iw"), QStringLiteral("Hebrew")},
        {QStringLiteral("hi"), QStringLiteral("Hindi")},
        {QStringLiteral("hu"), QStringLiteral("Hungarian")},
        {QStringLiteral("is"), QStringLiteral("Icelandic")},
        {QStringLiteral("id"), QStringLiteral("Indonesian")},
        {QStringLiteral("ga"), QStringLiteral("Irish")},
        {QStringLiteral("jv"), QStringLiteral("Javanese")},
        {QStringLiteral("kn"), QStringLiteral("Kannada")},
        {QStringLiteral("kk"), QStringLiteral("Kazakh")},
        {QStringLiteral("km"), QStringLiteral("Khmer")},
        {QStringLiteral("ku"), QStringLiteral("Kurdish")},
        {QStringLiteral("ky"), QStringLiteral("Kyrgyz")},
        {QStringLiteral("lo"), QStringLiteral("Lao")},
        {QStringLiteral("la"), QStringLiteral("Latin")},
        {QStringLiteral("lv"), QStringLiteral("Latvian")},
        {QStringLiteral("lt"), QStringLiteral("Lithuanian")},
        {QStringLiteral("lb"), QStringLiteral("Luxembourgish")},
        {QStringLiteral("mk"), QStringLiteral("Macedonian")},
        {QStringLiteral("mg"), QStringLiteral("Malagasy")},
        {QStringLiteral("ms"), QStringLiteral("Malay")},
        {QStringLiteral("ml"), QStringLiteral("Malayalam")},
        {QStringLiteral("mt"), QStringLiteral("Maltese")},
        {QStringLiteral("mi"), QStringLiteral("Maori")},
        {QStringLiteral("mr"), QStringLiteral("Marathi")},
        {QStringLiteral("morse"), QStringLiteral("Morse")},
        {QStringLiteral("mn"), QStringLiteral("Mongolian")},
        {QStringLiteral("my"), QStringLiteral("Myanmar (Burmese)")},
        {QStringLiteral("ne"), QStringLiteral("Nepali")},
        {QStringLiteral("no"), QStringLiteral("Norwegian")},
        {QStringLiteral("ps"), QStringLiteral("Pashto")},
        {QStringLiteral("fa"), QStringLiteral("Persian")},
        {QStringLiteral("ro"), QStringLiteral("Romanian")},
        {QStringLiteral("sr"), QStringLiteral("Serbian")},
        {QStringLiteral("si"), QStringLiteral("Sinhala")},
        {QStringLiteral("sk"), QStringLiteral("Slovak")},
        {QStringLiteral("sl"), QStringLiteral("Slovenian")},
        {QStringLiteral("so"), QStringLiteral("Somali")},
        {QStringLiteral("su"), QStringLiteral("Sundanese")},
        {QStringLiteral("sw"), QStringLiteral("Swahili")},
        {QStringLiteral("sv"), QStringLiteral("Swedish")},
        {QStringLiteral("ta"), QStringLiteral("Tamil")},
        {QStringLiteral("te"), QStringLiteral("Telugu")},
        {QStringLiteral("th"), QStringLiteral("Thai")},
        {QStringLiteral("tl"), QStringLiteral("Tagalog")},
        {QStringLiteral("uk"), QStringLiteral("Ukrainian")},
        {QStringLiteral("ur"), QStringLiteral("Urdu")},
        {QStringLiteral("uz"), QStringLiteral("Uzbek")},
        {QStringLiteral("vi"), QStringLiteral("Vietnamese")},
        {QStringLiteral("cy"), QStringLiteral("Welsh")},
        {QStringLiteral("xh"), QStringLiteral("Xhosa")},
        {QStringLiteral("yi"), QStringLiteral("Yiddish")},
        {QStringLiteral("yo"), QStringLiteral("Yoruba")},
        {QStringLiteral("zu"), QStringLiteral("Zulu")},
    };

    return languages;
}

QString translationLanguageCodeFromInput(QString language)
{
    const auto normalized = normalizedLanguageCode(language);
    if (normalized.isEmpty())
    {
        return {};
    }

    const auto &languages = supportedTranslationLanguages();
    const auto codeIt =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.code == normalized;
        });
    if (codeIt != languages.end())
    {
        return codeIt->code;
    }

    const auto nameIt =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.name.compare(language.trimmed(), Qt::CaseInsensitive) ==
                   0;
        });
    if (nameIt != languages.end())
    {
        return nameIt->code;
    }

    static const QHash<QString, QString> aliases{
        {"zh", QStringLiteral("zh-cn")},
        {"zh-hans", QStringLiteral("zh-cn")},
        {"cn", QStringLiteral("zh-cn")},
        {"chinese", QStringLiteral("zh-cn")},
        {"simplified-chinese", QStringLiteral("zh-cn")},
        {"zh-hant", QStringLiteral("zh-tw")},
        {"tw", QStringLiteral("zh-tw")},
        {"traditional-chinese", QStringLiteral("zh-tw")},
        {"he", QStringLiteral("iw")},
        {"fil", QStringLiteral("tl")},
        {"filipino", QStringLiteral("tl")},
        {"kmr", QStringLiteral("ku")},
        {"mn-cyrl", QStringLiteral("mn")},
        {"nb", QStringLiteral("no")},
        {"sr-cyrl", QStringLiteral("sr")},
        {"burmese", QStringLiteral("my")},
        {"br", QStringLiteral("pt")},
        {"pt-br", QStringLiteral("pt")},
        {"jp", QStringLiteral("ja")},
        {"kr", QStringLiteral("ko")},
        {"ua", QStringLiteral("uk")},
        {"morse-code", QStringLiteral("morse")},
        {"morse code", QStringLiteral("morse")},
    };

    return aliases.value(normalized);
}

bool isSupportedTranslationLanguage(const QString &language)
{
    return !translationLanguageCodeFromInput(language).isEmpty();
}

QString normalizedTranslationTargetLanguage(QString language)
{
    const auto code = translationLanguageCodeFromInput(std::move(language));
    if (code.isEmpty())
    {
        return QStringLiteral("en");
    }

    return code;
}

QString translationLanguageName(const QString &language)
{
    const auto normalized = translationLanguageCodeFromInput(language);
    if (normalized.isEmpty())
    {
        return {};
    }

    const auto &languages = supportedTranslationLanguages();
    const auto it =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.code == normalized;
        });
    if (it != languages.end())
    {
        return it->name;
    }

    return normalized.toUpper();
}

QString trimTextForTranslation(QString text)
{
    text = text.trimmed();
    if (text.size() > MAX_TRANSLATION_TEXT_LENGTH)
    {
        text = text.left(MAX_TRANSLATION_TEXT_LENGTH);
        if (text.back().isHighSurrogate())
        {
            text.chop(1);
        }
    }

    return text;
}

QString encodeMorseText(const QString &text)
{
    return encodeTextToMorse(text);
}

std::optional<QString> decodeMorseText(const QString &text)
{
    return decodeTextFromMorse(text, 2, true);
}

bool isMorseText(const QString &text)
{
    return decodeTextFromMorse(text, 1, false).has_value();
}

void requestTextTranslation(const QString &text, const QString &targetLanguage,
                            QObject *caller,
                            TranslationSuccessCallback onSuccess,
                            TranslationErrorCallback onError,
                            TranslationFinishedCallback onFinished,
                            std::optional<TranslationProvider> providerOverride)
{
    const QPointer<QObject> finishCaller(caller);
    const bool hasFinishCaller = caller != nullptr;
    onFinished = [finishCaller, hasFinishCaller,
                  callback = std::move(onFinished)] {
        if ((!hasFinishCaller || finishCaller) && callback)
        {
            callback();
        }
    };
    const auto requestText = trimTextForTranslation(text);
    if (requestText.isEmpty())
    {
        if (onError)
        {
            onError(QStringLiteral("There is no text to translate."));
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto target = normalizedTranslationTargetLanguage(targetLanguage);
    const auto decodedMorse = decodeMorseText(requestText);
    if (target == QStringLiteral("morse"))
    {
        const auto permissiveMorse = decodeTextFromMorse(requestText, 1, false);
        const auto translated = encodeMorseText(
            permissiveMorse.has_value() ? *permissiveMorse : requestText);
        if (translated.isEmpty())
        {
            if (onError)
            {
                onError(
                    QStringLiteral("Translation failed: unsupported text."));
            }
        }
        else if (onSuccess)
        {
            onSuccess({
                .translatedText = translated,
                .detectedLanguage = permissiveMorse.has_value()
                                        ? QStringLiteral("morse")
                                        : QString{},
            });
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    if (decodedMorse.has_value() && target == QStringLiteral("en"))
    {
        if (onSuccess)
        {
            onSuccess({
                .translatedText = *decodedMorse,
                .detectedLanguage = QStringLiteral("morse"),
            });
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto textForTranslation =
        decodedMorse.has_value() ? *decodedMorse : requestText;
    const bool sourceWasMorse = decodedMorse.has_value();
    auto *settings = getSettings();
    const auto &providers = translationProviders();
    const auto configuredId =
        settings->translationProvider.getValue().trimmed().toLower();
    const auto descriptor = std::ranges::find_if(
        providers, [&](const TranslationProviderDescriptor &item) {
            return providerOverride ? item.provider == *providerOverride
                                    : item.id == configuredId;
        });
    if (descriptor == providers.end())
    {
        if (onError)
        {
            onError(
                QStringLiteral("Choose a translation provider in Settings."));
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto provider = descriptor->provider;
    const translation::detail::ConnectionSettings connection{
        .libreEndpoint = settings->translationLibreEndpoint.getValue(),
        .azureRegion = settings->translationAzureRegion.getValue(),
        .azureEndpoint = settings->translationAzureEndpoint.getValue(),
    };
    if (!descriptor->usesApiKey)
    {
        executeProviderRequest(provider, textForTranslation, target, {},
                               connection, caller, sourceWasMorse,
                               std::move(onSuccess), std::move(onError),
                               std::move(onFinished));
        return;
    }

    const bool hadCaller = caller != nullptr;
    const QPointer<QObject> callerGuard(caller);
    readTranslationProviderCredential(
        provider,
        [provider, descriptor = *descriptor, textForTranslation, target,
         connection, callerGuard, hadCaller, sourceWasMorse,
         onSuccess = std::move(onSuccess), onError = std::move(onError),
         onFinished =
             std::move(onFinished)](ExpectedStr<QString> credential) mutable {
            if (hadCaller && !callerGuard)
            {
                return;
            }

            if (!credential)
            {
                if (onError)
                {
                    onError(credential.error());
                }
                if (onFinished)
                {
                    onFinished();
                }
                return;
            }
            const auto key = credential->trimmed();
            if (key.isEmpty() && !descriptor.apiKeyOptional)
            {
                if (onError)
                {
                    onError(
                        QStringLiteral("Set up %1 with an API key in Settings.")
                            .arg(descriptor.name));
                }
                if ((!hadCaller || callerGuard) && onFinished)
                {
                    onFinished();
                }
                return;
            }

            executeProviderRequest(provider, textForTranslation, target, key,
                                   connection, callerGuard.data(),
                                   sourceWasMorse, std::move(onSuccess),
                                   std::move(onError), std::move(onFinished));
        });
}

}  // namespace chatterino
