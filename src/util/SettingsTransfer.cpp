#include "util/SettingsTransfer.hpp"

#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"

#include <pajlada/settings/signalargs.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <tuple>

namespace {

using namespace chatterino;
using namespace chatterino::settingsbackup;

constexpr auto FORMAT_NAME = "moltorino-settings";
constexpr int FORMAT_VERSION = 1;
constexpr auto STAGING_DIRECTORY = ".moltorino-restore";
constexpr auto MANIFEST_FILE = "pending.json";
constexpr auto SETTINGS_CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr auto BASE64_URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr int MAX_RESCUE_COPIES = 9;

const QSet<QString> ALLOWED_TARGETS{
    QStringLiteral("settings.json"),         QStringLiteral("commands.json"),
    QStringLiteral("chat-automations.json"), QStringLiteral("user-data.json"),
    QStringLiteral("window-layout.json"),
};

const QSet<QString> LIVE_IMPORT_CATEGORIES{
    QStringLiteral("appearance"),
};

const QSet<QString> LOCAL_THEME_STATE_PATHS{
    QStringLiteral("/appearance/theme/bluzyrinomigrated"),
    QStringLiteral("/appearance/theme/moltorinoselection"),
};

const QSet<QString> RESTART_REQUIRED_SETTING_PATHS{
    QStringLiteral("/appearance/useQtSystemStyle"),
};

QSet<QString> allowedCategoryIDs()
{
    QSet<QString> allowed;
    for (const auto &category : categories())
    {
        allowed.insert(category.id);
    }
    return allowed;
}

QString categoryForTarget(const QString &target)
{
    if (target == QStringLiteral("commands.json"))
    {
        return QStringLiteral("commands");
    }
    if (target == QStringLiteral("window-layout.json"))
    {
        return QStringLiteral("layout");
    }
    if (target == QStringLiteral("user-data.json"))
    {
        return QStringLiteral("personal");
    }
    return {};
}

ExpectedStr<QByteArray> readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return makeUnexpected(QStringLiteral("Couldn't read %1.").arg(path));
    }
    const auto contents = file.read(MAX_BUNDLE_BYTES + 1);
    if (contents.size() > MAX_BUNDLE_BYTES || !file.atEnd())
    {
        return makeUnexpected(
            QStringLiteral("This settings file is too large to read."));
    }
    if (file.error() != QFileDevice::NoError)
    {
        return makeUnexpected(QStringLiteral("Couldn't read %1.").arg(path));
    }
    return contents;
}

ExpectedStr<QJsonValue> parseJson(const QByteArray &contents)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || document.isNull())
    {
        return makeUnexpected(
            QStringLiteral("Invalid JSON: %1").arg(error.errorString()));
    }
    if (document.isObject())
    {
        return document.object();
    }
    if (document.isArray())
    {
        return document.array();
    }
    return makeUnexpected(QStringLiteral("The JSON file has no usable data."));
}

bool isValidTargetJson(const QString &target, const QJsonValue &value)
{
    if (target == QStringLiteral("chat-automations.json"))
    {
        const auto object = value.toObject();
        if (!value.isObject() || !object.value("rules").isArray() ||
            (object.contains("enabled") && !object.value("enabled").isBool()))
        {
            return false;
        }
        return std::ranges::all_of(
            object.value("rules").toArray(), [](const auto &rule) {
                const auto fields = rule.toObject();
                return rule.isObject() && fields.value("trigger").isString() &&
                       fields.value("response").isString();
            });
    }
    if (target == QStringLiteral("window-layout.json"))
    {
        return value.isObject() || value.isArray();
    }
    return value.isObject();
}

ExpectedStr<void> writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        return makeUnexpected(QStringLiteral("Couldn't write %1.").arg(path));
    }
    return {};
}

bool isPrivateFieldName(const QString &name)
{
    const auto lower = name.toLower();
    const QStringList privateParts{
        QStringLiteral("token"),       QStringLiteral("secret"),
        QStringLiteral("password"),    QStringLiteral("credential"),
        QStringLiteral("cookie"),      QStringLiteral("oauth"),
        QStringLiteral("clientid"),    QStringLiteral("apikey"),
        QStringLiteral("api_key"),     QStringLiteral("authorization"),
        QStringLiteral("customsound"), QStringLiteral("soundurl"),
        QStringLiteral("filepath"),    QStringLiteral("directory"),
    };
    return std::ranges::any_of(privateParts, [&](const auto &part) {
        return lower.contains(part);
    });
}

bool containsPrivateFields(const QJsonValue &value)
{
    if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (isPrivateFieldName(it.key()) ||
                containsPrivateFields(it.value()))
            {
                return true;
            }
        }
    }
    else if (value.isArray())
    {
        return std::ranges::any_of(value.toArray(), [](const auto &item) {
            return containsPrivateFields(item);
        });
    }
    return false;
}

QJsonValue sanitizedExportValue(const QJsonValue &value)
{
    if (value.isObject())
    {
        QJsonObject sanitized;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (!isPrivateFieldName(it.key()))
            {
                sanitized.insert(it.key(), sanitizedExportValue(it.value()));
            }
        }
        return sanitized;
    }
    if (value.isArray())
    {
        QJsonArray sanitized;
        for (const auto &item : value.toArray())
        {
            sanitized.append(sanitizedExportValue(item));
        }
        return sanitized;
    }
    return value;
}

ExpectedStr<void> validateBundleObject(const QJsonObject &bundle)
{
    if (bundle.value(QStringLiteral("format")).toString() !=
            QString::fromLatin1(FORMAT_NAME) ||
        bundle.value(QStringLiteral("version")).toInt() != FORMAT_VERSION)
    {
        return makeUnexpected(
            QStringLiteral("This settings file uses an unsupported format."));
    }
    if (!bundle.value(QStringLiteral("categories")).isArray() ||
        !bundle.value(QStringLiteral("settings")).isObject() ||
        !bundle.value(QStringLiteral("files")).isObject())
    {
        return makeUnexpected(
            QStringLiteral("This settings file is incomplete."));
    }

    const auto allowed = allowedCategoryIDs();
    QSet<QString> declared;
    for (const auto &value :
         bundle.value(QStringLiteral("categories")).toArray())
    {
        if (!value.isString() || !allowed.contains(value.toString()) ||
            declared.contains(value.toString()))
        {
            return makeUnexpected(QStringLiteral(
                "This settings file contains an invalid category."));
        }
        declared.insert(value.toString());
    }

    const auto grouped = bundle.value(QStringLiteral("settings")).toObject();
    for (auto category = grouped.begin(); category != grouped.end(); ++category)
    {
        if (!allowed.contains(category.key()) ||
            !declared.contains(category.key()) || !category.value().isObject())
        {
            return makeUnexpected(QStringLiteral(
                "This settings file contains an invalid settings group."));
        }
        const auto settings = category.value().toObject();
        for (auto item = settings.begin(); item != settings.end(); ++item)
        {
            if ((!isSafeSettingPath(item.key()) &&
                 !LOCAL_THEME_STATE_PATHS.contains(item.key().toLower())) ||
                containsPrivateFields(item.value()) ||
                categoryForSettingPath(item.key()) != category.key())
            {
                return makeUnexpected(QStringLiteral(
                    "This settings file contains invalid or private settings."));
            }
        }
    }

    QSet<QString> targets;
    const auto files = bundle.value(QStringLiteral("files")).toObject();
    for (const auto &value : files)
    {
        if (!value.isObject())
        {
            return makeUnexpected(QStringLiteral(
                "This settings file contains an invalid data section."));
        }
        const auto file = value.toObject();
        const auto category = file.value(QStringLiteral("category")).toString();
        const auto target = file.value(QStringLiteral("target")).toString();
        const auto data = file.value(QStringLiteral("data"));
        if (!declared.contains(category) || target == "settings.json" ||
            !ALLOWED_TARGETS.contains(target) || targets.contains(target) ||
            categoryForTarget(target) != category ||
            !isValidTargetJson(target, data) || containsPrivateFields(data))
        {
            return makeUnexpected(QStringLiteral(
                "This settings file contains an invalid data section."));
        }
        targets.insert(target);
    }
    return {};
}

ExpectedStr<void> validateSelection(const QJsonObject &bundle,
                                    const QSet<QString> &selectedCategories)
{
    auto valid = validateBundleObject(bundle);
    if (!valid)
    {
        return valid;
    }
    if (selectedCategories.isEmpty())
    {
        return makeUnexpected(QStringLiteral("Choose something to import."));
    }

    QSet<QString> declared;
    for (const auto &value :
         bundle.value(QStringLiteral("categories")).toArray())
    {
        declared.insert(value.toString());
    }
    if (std::ranges::any_of(selectedCategories, [&](const auto &category) {
            return !declared.contains(category);
        }))
    {
        return makeUnexpected(QStringLiteral(
            "The selected category is not included in this settings file."));
    }
    return {};
}

QByteArray mergeIdentity(const QJsonValue &value)
{
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto id =
            object.value(QStringLiteral("id")).toVariant().toString();
        if (!id.isEmpty())
        {
            return QByteArrayLiteral("id:") + id.toCaseFolded().toUtf8();
        }

        const auto name =
            object.value(QStringLiteral("name")).toVariant().toString();
        if (!name.isEmpty() && object.contains(QStringLiteral("replace")))
        {
            const bool regex = object.value("isRegex").toBool(false);
            const bool caseSensitive =
                object.value("isCaseSensitive").toBool(false);
            QJsonObject identity{
                {QStringLiteral("name"),
                 regex || caseSensitive ? name : name.toCaseFolded()},
                {QStringLiteral("isRegex"), regex},
                {QStringLiteral("isCaseSensitive"), caseSensitive},
            };
            return QByteArrayLiteral("nickname:") +
                   QJsonDocument(identity).toJson(QJsonDocument::Compact);
        }
        if (!name.isEmpty())
        {
            return QByteArrayLiteral("name:") + name.toCaseFolded().toUtf8();
        }

        const auto pattern = object.value(QStringLiteral("pattern")).toString();
        if (!pattern.isEmpty())
        {
            const bool regex = object.value("regex").toBool(false);
            const bool caseSensitive =
                object.contains(QStringLiteral("case"))
                    ? object.value(QStringLiteral("case")).toBool(false)
                    : object.value(QStringLiteral("caseSensitive"))
                          .toBool(false);
            QJsonObject identity{
                {QStringLiteral("pattern"),
                 regex || caseSensitive ? pattern : pattern.toCaseFolded()},
                {QStringLiteral("regex"), regex},
                {QStringLiteral("case"), caseSensitive},
                {QStringLiteral("channelScope"),
                 object.value(QStringLiteral("channelScope"))},
                {QStringLiteral("channelTargets"),
                 object.value(QStringLiteral("channelTargets"))},
                {QStringLiteral("global"),
                 object.value(QStringLiteral("global"))},
                {QStringLiteral("channels"),
                 object.value(QStringLiteral("channels"))},
                {QStringLiteral("ExcludedChannels"),
                 object.value(QStringLiteral("ExcludedChannels"))},
            };
            return QByteArrayLiteral("pattern:") +
                   QJsonDocument(identity).toJson(QJsonDocument::Compact);
        }

        for (const auto &key :
             {QStringLiteral("userID"), QStringLiteral("command")})
        {
            const auto identity = object.value(key).toVariant().toString();
            if (!identity.isEmpty())
            {
                return key.toUtf8() + ':' + identity.toCaseFolded().toUtf8();
            }
        }
    }
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
}

QJsonValue mergedValue(const QJsonValue &current, const QJsonValue &incoming,
                       bool mergeLists)
{
    if (!mergeLists)
    {
        return incoming;
    }
    if (current.isObject() && incoming.isObject())
    {
        auto merged = current.toObject();
        const auto incomingObject = incoming.toObject();
        for (auto it = incomingObject.begin(); it != incomingObject.end(); ++it)
        {
            merged.insert(it.key(), mergedValue(merged.value(it.key()),
                                                it.value(), true));
        }
        return merged;
    }
    if (current.isArray() && incoming.isArray())
    {
        auto merged = current.toArray();
        QHash<QByteArray, int> positions;
        for (int i = 0; i < merged.size(); ++i)
        {
            positions.insert(mergeIdentity(merged.at(i)), i);
        }
        for (const auto &value : incoming.toArray())
        {
            const auto key = mergeIdentity(value);
            if (const auto existing = positions.constFind(key);
                existing != positions.cend())
            {
                merged.replace(*existing, value);
            }
            else
            {
                merged.append(value);
                positions.insert(key, merged.size() - 1);
            }
        }
        return merged;
    }
    return incoming;
}

void setAtPath(QJsonObject &root, const QStringList &segments, int index,
               const QJsonValue &value, bool mergeLists)
{
    if (index >= segments.size())
    {
        return;
    }
    const auto &key = segments.at(index);
    if (index == segments.size() - 1)
    {
        root.insert(key, mergedValue(root.value(key), value, mergeLists));
        return;
    }

    auto child = root.value(key).toObject();
    setAtPath(child, segments, index + 1, value, mergeLists);
    root.insert(key, child);
}

std::optional<QJsonValue> valueAtPath(const QJsonObject &root,
                                      const QString &path)
{
    QJsonValue current(root);
    for (const auto &segment : path.split('/', Qt::SkipEmptyParts))
    {
        if (!current.isObject())
        {
            return std::nullopt;
        }
        const auto object = current.toObject();
        if (!object.contains(segment))
        {
            return std::nullopt;
        }
        current = object.value(segment);
    }
    return current;
}

ExpectedStr<QStringList> liveImportPaths(
    const QJsonObject &bundle, const QSet<QString> &selectedCategories)
{
    auto valid = validateSelection(bundle, selectedCategories);
    if (!valid)
    {
        return makeUnexpected(valid.error());
    }
    if (std::ranges::any_of(selectedCategories, [](const auto &category) {
            return !LIVE_IMPORT_CATEGORIES.contains(category);
        }))
    {
        return makeUnexpected(
            QStringLiteral("This selection needs a restart."));
    }

    const auto grouped = bundle.value(QStringLiteral("settings")).toObject();
    QStringList paths;
    for (const auto &category : selectedCategories)
    {
        const auto values = grouped.value(category).toObject();
        for (auto item = values.begin(); item != values.end(); ++item)
        {
            if (!isSafeSettingPath(item.key()) ||
                categoryForSettingPath(item.key()) != category)
            {
                continue;
            }
            if (!getSettings()->isSettingRegistered(item.key()))
            {
                return makeUnexpected(QStringLiteral(
                    "One selected setting needs a restart in this version."));
            }
            if (!getSettings()->isValidSettingValue(item.key(), item.value()))
            {
                return makeUnexpected(QStringLiteral(
                    "One selected setting has an invalid value."));
            }
            paths.append(item.key());
        }
    }

    const auto files = bundle.value(QStringLiteral("files")).toObject();
    for (const auto &file : files)
    {
        const auto category =
            file.toObject().value(QStringLiteral("category")).toString();
        if (selectedCategories.contains(category))
        {
            return makeUnexpected(
                QStringLiteral("This selection needs a restart."));
        }
    }
    if (paths.isEmpty())
    {
        return makeUnexpected(
            QStringLiteral("This selection has no settings to apply."));
    }

    if (std::ranges::any_of(paths, [](const auto &path) {
            return RESTART_REQUIRED_SETTING_PATHS.contains(path);
        }))
    {
        return makeUnexpected(
            QStringLiteral("This selection needs a restart."));
    }

    paths.removeDuplicates();
    return paths;
}

ExpectedStr<QJsonObject> readObjectOrEmpty(const QString &path)
{
    if (!QFileInfo::exists(path))
    {
        return QJsonObject{};
    }
    auto bytes = readFile(path);
    if (!bytes)
    {
        return makeUnexpected(bytes.error());
    }
    auto json = parseJson(bytes.value());
    if (!json || !json->isObject())
    {
        return makeUnexpected(
            json ? QStringLiteral("Expected a JSON object in %1.").arg(path)
                 : json.error());
    }
    return json->toObject();
}

void flattenSettings(const QJsonValue &value, const QString &path,
                     const QJsonObject &registered, QJsonObject &flat)
{
    if (!path.isEmpty() && registered.contains(path))
    {
        flat.insert(path, registered.value(path));
        return;
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            flattenSettings(it.value(), path + u'/' + it.key(), registered,
                            flat);
        }
        return;
    }
    if (!path.isEmpty())
    {
        flat.insert(path, value);
    }
}

ExpectedStr<void> stageFiles(const QString &settingsDirectory,
                             const QHash<QString, QByteArray> &files)
{
    if (files.isEmpty())
    {
        return makeUnexpected(QStringLiteral("Nothing was selected."));
    }
    const auto staging = combinePath(settingsDirectory, STAGING_DIRECTORY);
    if (!QDir().mkpath(staging))
    {
        return makeUnexpected(
            QStringLiteral("Couldn't prepare the settings restore."));
    }

    QJsonArray manifestFiles;
    QStringList newlyStagedFiles;
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        if (!ALLOWED_TARGETS.contains(it.key()))
        {
            return makeUnexpected(
                QStringLiteral("Unsupported restore target."));
        }
        auto parsed = parseJson(it.value());
        if (!parsed || !isValidTargetJson(it.key(), parsed.value()))
        {
            return makeUnexpected(
                QStringLiteral("%1 is damaged: %2")
                    .arg(it.key(), parsed
                                       ? QStringLiteral("unexpected file format")
                                       : parsed.error()));
        }
        const auto stagedName =
            it.key() + u'.' +
            QUuid::createUuid().toString(QUuid::WithoutBraces) +
            QStringLiteral(".pending");
        auto write = writeFile(combinePath(staging, stagedName), it.value());
        if (!write)
        {
            for (const auto &created : newlyStagedFiles)
            {
                QFile::remove(combinePath(staging, created));
            }
            return write;
        }
        newlyStagedFiles.append(stagedName);
        manifestFiles.append(QJsonObject{
            {QStringLiteral("staged"), stagedName},
            {QStringLiteral("target"), it.key()},
        });
    }

    const QJsonObject manifest{
        {QStringLiteral("version"), 1},
        {QStringLiteral("createdAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("files"), manifestFiles},
    };
    auto manifestWrite =
        writeFile(combinePath(staging, MANIFEST_FILE),
                  QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    if (!manifestWrite)
    {
        for (const auto &created : newlyStagedFiles)
        {
            QFile::remove(combinePath(staging, created));
        }
        return manifestWrite;
    }

    const QSet<QString> activeFiles(newlyStagedFiles.begin(),
                                    newlyStagedFiles.end());
    const QDir stagingDirectory(staging);
    for (const auto &entry :
         stagingDirectory.entryList({QStringLiteral("*.pending")}, QDir::Files))
    {
        if (!activeFiles.contains(entry))
        {
            QFile::remove(stagingDirectory.filePath(entry));
        }
    }
    return {};
}

QString summaryFor(const QString &target, const QJsonValue &value)
{
    if (target == QStringLiteral("chat-automations.json"))
    {
        const auto count = value.toObject().value("rules").toArray().size();
        return QStringLiteral("%1 %2").arg(count).arg(
            count == 1 ? QStringLiteral("rule") : QStringLiteral("rules"));
    }
    if (target == QStringLiteral("commands.json"))
    {
        const auto count =
            value.toObject().value(QStringLiteral("commands")).toArray().size();
        return QStringLiteral("%1 custom %2").arg(count).arg(
            count == 1 ? QStringLiteral("command") : QStringLiteral("commands"));
    }
    if (target == QStringLiteral("user-data.json"))
    {
        const auto count =
            value.toObject().value(QStringLiteral("users")).toObject().size();
        return QStringLiteral("%1 saved %2").arg(count).arg(
            count == 1 ? QStringLiteral("user") : QStringLiteral("users"));
    }
    if (target == QStringLiteral("window-layout.json"))
    {
        const auto countSplits = [](const auto &self,
                                    const QJsonValue &node) -> int {
            if (!node.isObject())
            {
                return 0;
            }
            const auto object = node.toObject();
            if (object.value(QStringLiteral("type")).toString() ==
                QStringLiteral("split"))
            {
                return 1;
            }
            int count = 0;
            for (const auto &child :
                 object.value(QStringLiteral("items")).toArray())
            {
                count += self(self, child);
            }
            return count;
        };

        const auto windows =
            value.isArray()
                ? value.toArray()
                : value.toObject().value(QStringLiteral("windows")).toArray();
        int tabs = 0;
        int splits = 0;
        for (const auto &windowValue : windows)
        {
            const auto tabValues =
                windowValue.toObject().value(QStringLiteral("tabs")).toArray();
            tabs += tabValues.size();
            for (const auto &tabValue : tabValues)
            {
                splits += countSplits(
                    countSplits,
                    tabValue.toObject().value(QStringLiteral("splits2")));
            }
        }

        auto summary =
            QStringLiteral("%1 %2, %3 %4")
                .arg(tabs)
                .arg(tabs == 1 ? QStringLiteral("tab") : QStringLiteral("tabs"))
                .arg(splits)
                .arg(splits == 1 ? QStringLiteral("chat")
                                 : QStringLiteral("chats"));
        if (windows.size() > 1)
        {
            summary += QStringLiteral(", %1 windows").arg(windows.size());
        }
        return summary;
    }

    const auto object = value.toObject();
    return QStringLiteral("%1 setting %2").arg(object.size()).arg(
        object.size() == 1 ? QStringLiteral("group") : QStringLiteral("groups"));
}

}

namespace chatterino::settingsbackup {

const QVector<Category> &categories()
{
    static const QVector<Category> values{
        {QStringLiteral("appearance"), QStringLiteral("Appearance and UI"),
         QStringLiteral("Theme, fonts, message layout, badges, and emotes")},
        {QStringLiteral("chat"), QStringLiteral("Chat and input"),
         QStringLiteral("Chat behavior, input, links, and translation")},
        {QStringLiteral("layout"), QStringLiteral("Tabs and layout"),
         QStringLiteral("Open windows, tabs, splits, and tab preferences")},
        {QStringLiteral("highlights"), QStringLiteral("Highlights"),
         QStringLiteral("Message highlights, word lists, users, and badges")},
        {QStringLiteral("commands"), QStringLiteral("Hotkeys and commands"),
         QStringLiteral("Keyboard shortcuts and custom commands")},
        {QStringLiteral("lists"), QStringLiteral("Filters and personal lists"),
         QStringLiteral("Filters, ignores, nicknames, and muted channels")},
        {QStringLiteral("moltorino-general"),
         QStringLiteral("Moltorino General"),
         QStringLiteral("General Moltorino features")},
        {QStringLiteral("moltorino-moderation"),
         QStringLiteral("Moltorino Moderation"),
         QStringLiteral("Moderator tools, repeated messages, nukes, and logs")},
        {QStringLiteral("personal"), QStringLiteral("User notes and colors"),
         QStringLiteral("Personal notes and custom user colors")},
        {QStringLiteral("other"), QStringLiteral("Everything else"),
         QStringLiteral("Other preferences")},
    };
    return values;
}

bool isSafeSettingPath(const QString &path)
{
    const auto lower = path.toLower();
    const auto segments = path.split(u'/', Qt::KeepEmptyParts);
    if (!path.startsWith(u'/') || path.endsWith(u'/') || path.size() > 512 ||
        path.contains("//") || segments.size() > 33 ||
        std::ranges::any_of(segments, [](const auto &segment) {
            return segment == u"." || segment == u"..";
        }))
    {
        return false;
    }
    for (const auto &local : LOCAL_THEME_STATE_PATHS)
    {
        if (lower == local || lower.startsWith(local + u'/') ||
            local.startsWith(lower + u'/'))
        {
            return false;
        }
    }
    const QStringList blockedPrefixes{
        QStringLiteral("/moltorino/dailymessage/lastshowndate"),
        QStringLiteral("/moltorino/dailymessage/lastquote"),
        QStringLiteral("/moltorino/dailymessage/lastleadin"),
        QStringLiteral("/accounts"),
        QStringLiteral("/kickaccounts"),
        QStringLiteral("/youtubeaccounts"),
        QStringLiteral("/tiktokaccounts"),
        QStringLiteral("/accountcredentials"),
        QStringLiteral("/moltorino/auth"),
        QStringLiteral("/moltorino/botbadge"),
        QStringLiteral("/moltorino/vanity/seventv"),
        QStringLiteral("/moltorino/presence"),
        QStringLiteral("/moltorino/client/runtime"),
        QStringLiteral("/moltorino/client/sendheartbeattelemetry"),
        QStringLiteral("/moltorino/client/anonymizeheartbeattelemetry"),
        QStringLiteral("/moltorino/client/sendactivityheartbeats"),
        QStringLiteral("/moltorino/client/hideaccountinheartbeats"),
        QStringLiteral("/external/imageuploader"),
        QStringLiteral("/plugins"),
        QStringLiteral("/proxy"),
    };
    for (const auto &prefix : blockedPrefixes)
    {
        if (lower.startsWith(prefix) || prefix.startsWith(lower + u'/'))
        {
            return false;
        }
    }

    const QStringList blockedParts{
        QStringLiteral("token"),         QStringLiteral("secret"),
        QStringLiteral("password"),      QStringLiteral("credential"),
        QStringLiteral("cookie"),        QStringLiteral("oauth"),
        QStringLiteral("clientid"),      QStringLiteral("customsound"),
        QStringLiteral("soundurl"),      QStringLiteral("path"),
        QStringLiteral("apikey"),        QStringLiteral("api_key"),
        QStringLiteral("authorization"),
    };
    return std::ranges::none_of(blockedParts, [&](const auto &part) {
        return lower.contains(part);
    });
}

std::optional<RevokeKey> parseRevokeKey(const QString &input)
{
    const auto trimmed = input.trimmed();
    static const QRegularExpression legacyPattern(
        QStringLiteral("^([A-Za-z0-9]{4,16})\\.([A-Za-z0-9_-]{16,64})$"));
    if (const auto legacy = legacyPattern.match(trimmed); legacy.hasMatch())
    {
        return RevokeKey{legacy.captured(1), legacy.captured(2)};
    }

    static const QRegularExpression opaquePattern(
        QStringLiteral("^[A-Za-z0-9_-]{22}$"));
    if (!opaquePattern.match(trimmed).hasMatch())
    {
        return std::nullopt;
    }

    const auto token = trimmed.left(8) + trimmed.mid(14, 8);
    const auto shiftedCode = trimmed.mid(8, 6);
    const auto codeAlphabet = QString::fromLatin1(SETTINGS_CODE_ALPHABET);
    const auto tokenAlphabet = QString::fromLatin1(BASE64_URL_ALPHABET);
    QString code;
    code.reserve(6);
    for (int index = 0; index < shiftedCode.size(); ++index)
    {
        const auto shiftedIndex = codeAlphabet.indexOf(shiftedCode.at(index));
        const auto tokenIndex = tokenAlphabet.indexOf(token.at(index));
        if (shiftedIndex < 0 || tokenIndex < 0)
        {
            return std::nullopt;
        }
        code +=
            codeAlphabet.at((shiftedIndex - tokenIndex % codeAlphabet.size() +
                             codeAlphabet.size()) %
                            codeAlphabet.size());
    }
    return RevokeKey{code, token};
}

QString categoryForSettingPath(const QString &path)
{
    const auto lower = path.toLower();
    if (lower.startsWith(QStringLiteral("/misc/")))
    {
        if (lower == QStringLiteral("/misc/displayseventvpaints") ||
            lower == QStringLiteral("/misc/displayseventvpaintshadows") ||
            lower == QStringLiteral("/misc/largeseventvpaintshadows") ||
            lower == QStringLiteral("/misc/displayseventvanimatedprofile") ||
            lower == QStringLiteral("/misc/emotestooltippreview"))
        {
            return QStringLiteral("appearance");
        }
        if (lower.startsWith(QStringLiteral("/misc/twitch/")) ||
            lower.startsWith(QStringLiteral("/misc/scrollback/")) ||
            lower == QStringLiteral("/misc/chatsendprotocol") ||
            lower == QStringLiteral("/misc/openlinksincognito") ||
            lower == QStringLiteral("/misc/askonimageupload") ||
            lower == QStringLiteral("/misc/showpronouns") ||
            lower == QStringLiteral("/misc/showpronounsinchat") ||
            lower == QStringLiteral("/misc/hideemojibutton"))
        {
            return QStringLiteral("chat");
        }
        if (lower == QStringLiteral("/misc/askontabvisibilitytoggle") ||
            lower == QStringLiteral("/misc/locknotebooklayout"))
        {
            return QStringLiteral("layout");
        }
        if (lower == QStringLiteral("/misc/hidemodactionsonmodusercards"))
        {
            return QStringLiteral("moltorino-moderation");
        }
    }
    if (lower.startsWith(QStringLiteral("/usercard/")))
    {
        return QStringLiteral("moltorino-general");
    }
    if (lower.contains(QStringLiteral("highlight")) ||
        lower.contains(QStringLiteral("blacklisted")))
    {
        return QStringLiteral("highlights");
    }
    if (lower == QStringLiteral("/moltorino/hiddenusers"))
    {
        return QStringLiteral("lists");
    }
    if (lower.startsWith(QStringLiteral("/moltorino/")))
    {
        const QStringList moderationWords{
            QStringLiteral("moder"),   QStringLiteral("nuke"),
            QStringLiteral("spam"),    QStringLiteral("pyramid"),
            QStringLiteral("repeat"),  QStringLiteral("pin"),
            QStringLiteral("poll"),    QStringLiteral("prediction"),
            QStringLiteral("logs"),    QStringLiteral("editor"),
            QStringLiteral("leadmod"), QStringLiteral("automod"),
        };
        return std::ranges::any_of(moderationWords,
                                   [&](const auto &word) {
                                       return lower.contains(word);
                                   })
                   ? QStringLiteral("moltorino-moderation")
                   : QStringLiteral("moltorino-general");
    }
    if (lower.contains(QStringLiteral("hotkey")) ||
        lower.contains(QStringLiteral("command")))
    {
        return QStringLiteral("commands");
    }
    if (lower.contains(QStringLiteral("filter")) ||
        lower.contains(QStringLiteral("ignore")) ||
        lower.contains(QStringLiteral("nickname")) ||
        lower.contains(QStringLiteral("muted")))
    {
        return QStringLiteral("lists");
    }
    if (lower.startsWith(QStringLiteral("/appearance")) ||
        lower.startsWith(QStringLiteral("/emotes")) ||
        lower.startsWith(QStringLiteral("/theme")))
    {
        return lower.contains(QStringLiteral("tab"))
                   ? QStringLiteral("layout")
                   : QStringLiteral("appearance");
    }
    if (lower.contains(QStringLiteral("tab")) ||
        lower.contains(QStringLiteral("windowlayout")))
    {
        return QStringLiteral("layout");
    }
    if (lower.startsWith(QStringLiteral("/behaviour")) ||
        lower.startsWith(QStringLiteral("/links")) ||
        lower.contains(QStringLiteral("translation")) ||
        lower.contains(QStringLiteral("input")))
    {
        return QStringLiteral("chat");
    }
    return QStringLiteral("other");
}

ExpectedStr<QJsonObject> createBundle(const QString &settingsDirectory,
                                      const QSet<QString> &selectedCategories)
{
    if (selectedCategories.isEmpty())
    {
        return makeUnexpected(QStringLiteral("Choose at least one category."));
    }
    const auto allowed = allowedCategoryIDs();
    if (std::ranges::any_of(selectedCategories, [&](const auto &category) {
            return !allowed.contains(category);
        }))
    {
        return makeUnexpected(
            QStringLiteral("Choose only supported settings categories."));
    }

    QJsonObject grouped;
    const auto registered = getSettings()->registeredSettingsSnapshot();
    QJsonObject snapshot;
    const auto settingsPath =
        combinePath(settingsDirectory, QStringLiteral("settings.json"));
    if (QFileInfo::exists(settingsPath))
    {
        auto stored = readObjectOrEmpty(settingsPath);
        if (!stored)
        {
            return makeUnexpected(stored.error());
        }
        flattenSettings(stored.value(), {}, registered, snapshot);
    }
    for (auto it = registered.begin(); it != registered.end(); ++it)
    {
        if (!snapshot.contains(it.key()))
        {
            snapshot.insert(it.key(), it.value());
        }
    }
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it)
    {
        if (!isSafeSettingPath(it.key()))
        {
            continue;
        }
        const auto category = categoryForSettingPath(it.key());
        if (!selectedCategories.contains(category))
        {
            continue;
        }
        auto settings = grouped.value(category).toObject();
        settings.insert(it.key(), sanitizedExportValue(it.value()));
        grouped.insert(category, settings);
    }

    QJsonObject files;
    const auto addFile = [&](const QString &category, const QString &key,
                             const QString &fileName) -> ExpectedStr<void> {
        if (!selectedCategories.contains(category))
        {
            return {};
        }
        const auto path = combinePath(settingsDirectory, fileName);
        if (!QFileInfo::exists(path))
        {
            return {};
        }
        auto bytes = readFile(path);
        if (!bytes)
        {
            return makeUnexpected(bytes.error());
        }
        auto value = parseJson(bytes.value());
        if (!value)
        {
            return makeUnexpected(QStringLiteral("%1 is damaged: %2")
                                      .arg(fileName, value.error()));
        }
        files.insert(key, QJsonObject{
                              {QStringLiteral("category"), category},
                              {QStringLiteral("target"), fileName},
                              {QStringLiteral("data"),
                               sanitizedExportValue(value.value())},
                          });
        return {};
    };

    for (const auto &[category, key, fileName] :
         std::initializer_list<std::tuple<QString, QString, QString>>{
             {QStringLiteral("commands"), QStringLiteral("commands"),
              QStringLiteral("commands.json")},
             {QStringLiteral("layout"), QStringLiteral("layout"),
              QStringLiteral("window-layout.json")},
             {QStringLiteral("personal"), QStringLiteral("userData"),
              QStringLiteral("user-data.json")},
         })
    {
        auto result = addFile(category, key, fileName);
        if (!result)
        {
            return makeUnexpected(result.error());
        }
    }

    QJsonArray selected;
    for (const auto &category : categories())
    {
        if (selectedCategories.contains(category.id))
        {
            selected.append(category.id);
        }
    }

    const QJsonObject bundle{
        {QStringLiteral("format"), QString::fromLatin1(FORMAT_NAME)},
        {QStringLiteral("version"), FORMAT_VERSION},
        {QStringLiteral("createdAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("categories"), selected},
        {QStringLiteral("settings"), grouped},
        {QStringLiteral("files"), files},
    };
    if (QJsonDocument(bundle).toJson(QJsonDocument::Compact).size() >
        MAX_BUNDLE_BYTES)
    {
        return makeUnexpected(
            QStringLiteral("These settings are too large to export together."));
    }
    return bundle;
}

ExpectedStr<QJsonObject> parseBundle(const QByteArray &contents)
{
    if (contents.size() > settingsbackup::MAX_BUNDLE_BYTES)
    {
        return makeUnexpected(
            QStringLiteral("This settings file is too large to import."));
    }
    auto value = parseJson(contents);
    if (!value || !value->isObject())
    {
        return makeUnexpected(
            value ? QStringLiteral("This is not a Moltorino settings file.")
                  : value.error());
    }
    const auto root = value->toObject();
    auto valid = validateBundleObject(root);
    if (!valid)
    {
        return makeUnexpected(valid.error());
    }
    return root;
}

ExpectedStr<QStringList> stageImport(const QString &settingsDirectory,
                                     const QJsonObject &bundle,
                                     const QSet<QString> &selectedCategories,
                                     bool mergeLists)
{
    auto valid = validateSelection(bundle, selectedCategories);
    if (!valid)
    {
        return makeUnexpected(valid.error());
    }

    QHash<QString, QByteArray> staged;
    QStringList changed;

    auto currentSettings =
        readObjectOrEmpty(combinePath(settingsDirectory, "settings.json"));
    if (!currentSettings)
    {
        return makeUnexpected(currentSettings.error());
    }
    auto settings = currentSettings.value();
    bool settingsChanged = false;
    const auto grouped = bundle.value(QStringLiteral("settings")).toObject();
    for (auto category = grouped.begin(); category != grouped.end(); ++category)
    {
        if (!selectedCategories.contains(category.key()) ||
            !category.value().isObject())
        {
            continue;
        }
        const auto categorySettings = category.value().toObject();
        for (auto item = categorySettings.begin();
             item != categorySettings.end(); ++item)
        {
            if (!isSafeSettingPath(item.key()) ||
                categoryForSettingPath(item.key()) != category.key())
            {
                continue;
            }
            auto segments = item.key().split('/', Qt::SkipEmptyParts);
            if (segments.isEmpty())
            {
                continue;
            }
            setAtPath(settings, segments, 0, item.value(), mergeLists);
            if (item.key() == QStringLiteral("/appearance/theme/name") &&
                item.value().isString())
            {
                setAtPath(
                    settings,
                    {QStringLiteral("appearance"), QStringLiteral("theme"),
                     QStringLiteral("moltorinoSelection")},
                    0, item.value(), false);
            }
            settingsChanged = true;
        }
        changed.append(category.key());
    }
    if (settingsChanged)
    {
        staged.insert(QStringLiteral("settings.json"),
                      QJsonDocument(settings).toJson(QJsonDocument::Indented));
    }

    const auto files = bundle.value(QStringLiteral("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const auto spec = it.value().toObject();
        const auto category = spec.value(QStringLiteral("category")).toString();
        const auto target = spec.value(QStringLiteral("target")).toString();
        const auto data = spec.value(QStringLiteral("data"));
        if (!selectedCategories.contains(category) ||
            !ALLOWED_TARGETS.contains(target) || target == "settings.json" ||
            categoryForTarget(target) != category ||
            (!data.isObject() && !data.isArray()))
        {
            continue;
        }

        QJsonValue finalData = data;
        if (mergeLists && target != QStringLiteral("window-layout.json"))
        {
            const auto currentPath = combinePath(settingsDirectory, target);
            if (QFileInfo::exists(currentPath))
            {
                auto currentBytes = readFile(currentPath);
                if (!currentBytes)
                {
                    return makeUnexpected(currentBytes.error());
                }
                auto currentData = parseJson(currentBytes.value());
                if (!currentData)
                {
                    return makeUnexpected(currentData.error());
                }
                finalData = mergedValue(currentData.value(), data, true);
            }
        }
        staged.insert(target, finalData.isObject()
                                  ? QJsonDocument(finalData.toObject())
                                        .toJson(QJsonDocument::Indented)
                                  : QJsonDocument(finalData.toArray())
                                        .toJson(QJsonDocument::Indented));
        changed.append(category);
    }

    auto stagedResult = stageFiles(settingsDirectory, staged);
    if (!stagedResult)
    {
        return makeUnexpected(stagedResult.error());
    }
    changed.removeDuplicates();
    return changed;
}

bool canApplyImportNow(const QJsonObject &bundle,
                       const QSet<QString> &selectedCategories)
{
    return liveImportPaths(bundle, selectedCategories).has_value();
}

ExpectedStr<QStringList> applyImportNow(const QString &settingsDirectory,
                                        const QJsonObject &bundle,
                                        const QSet<QString> &selectedCategories,
                                        bool mergeLists)
{
    auto paths = liveImportPaths(bundle, selectedCategories);
    if (!paths)
    {
        return makeUnexpected(paths.error());
    }
    auto staged =
        stageImport(settingsDirectory, bundle, selectedCategories, mergeLists);
    if (!staged)
    {
        return makeUnexpected(staged.error());
    }
    QString restoreError;
    if (!applyPendingRestore(settingsDirectory, &restoreError))
    {
        return makeUnexpected(restoreError);
    }
    auto stored = readObjectOrEmpty(
        combinePath(settingsDirectory, QStringLiteral("settings.json")));
    if (!stored)
    {
        return makeUnexpected(stored.error());
    }
    QJsonObject finalValues;
    for (const auto &path : *paths)
    {
        const auto value = valueAtPath(stored.value(), path);
        if (!value)
        {
            return makeUnexpected(QStringLiteral(
                "A selected setting was missing after the import."));
        }
        finalValues.insert(path, value.value());
    }

    const auto encoded =
        QJsonDocument(finalValues).toJson(QJsonDocument::Compact);
    rapidjson::Document values;
    values.Parse(encoded.constData(), static_cast<size_t>(encoded.size()));
    if (!values.IsObject())
    {
        return makeUnexpected(
            QStringLiteral("The imported settings could not be applied."));
    }
    auto manager = pajlada::Settings::SettingManager::getInstance();
    for (const auto &path : *paths)
    {
        const auto key = path.toUtf8();
        const auto member = values.FindMember(key.constData());
        const auto setting = pajlada::Settings::SettingManager::getSetting(
                                 path.toUtf8().toStdString(), manager)
                                 .lock();
        if (member == values.MemberEnd() || !setting)
        {
            return makeUnexpected(
                QStringLiteral("A selected setting is no longer available."));
        }

        pajlada::Settings::SignalArgs args;
        args.source = pajlada::Settings::SignalArgs::Source::External;
        args.compareBeforeSet = true;
        setting->marshalJSON(member->value, std::move(args));
    }
    return staged;
}

QVector<RecoveryFile> inspectRecoveryFiles(const QString &settingsDirectory)
{
    struct Kind {
        QString file;
        QString name;
    };
    const QVector<Kind> kinds{
        {QStringLiteral("settings.json"), QStringLiteral("Settings")},
        {QStringLiteral("window-layout.json"),
         QStringLiteral("Tabs and layout")},
        {QStringLiteral("commands.json"), QStringLiteral("Commands")},
        {QStringLiteral("chat-automations.json"),
         QStringLiteral("Chat automations")},
        {QStringLiteral("user-data.json"),
         QStringLiteral("User notes and colors")},
    };

    QVector<RecoveryFile> result;
    const QDir directory(settingsDirectory);
    for (const auto &kind : kinds)
    {
        const auto entries = directory.entryInfoList(
            QStringList{kind.file, kind.file + QStringLiteral(".bkp-*"),
                        kind.file + QStringLiteral(".rescue-*")},
            QDir::Files, QDir::Time);
        for (const auto &entry : entries)
        {
            RecoveryFile file{
                .sourcePath = entry.absoluteFilePath(),
                .targetFile = kind.file,
                .kind = kind.name,
                .lastModified = entry.lastModified(),
                .sizeBytes = entry.size(),
            };
            auto bytes = readFile(file.sourcePath);
            if (!bytes)
            {
                file.state = RecoveryState::Unreadable;
                file.summary = QStringLiteral("Could not read file");
            }
            else if (bytes->trimmed().isEmpty())
            {
                file.state = RecoveryState::Damaged;
                file.summary = QStringLiteral("File is empty");
            }
            else
            {
                auto json = parseJson(bytes.value());
                if (!json)
                {
                    file.state = RecoveryState::Damaged;
                    file.summary = json.error();
                }
                else
                {
                    file.summary = summaryFor(kind.file, json.value());
                    const bool logicallyEmpty =
                        (json->isObject() && json->toObject().isEmpty()) ||
                        (json->isArray() && json->toArray().isEmpty());
                    file.state = logicallyEmpty ? RecoveryState::Empty
                                                : RecoveryState::Healthy;
                }
            }
            result.append(std::move(file));
        }
    }
    std::ranges::sort(result, [](const auto &a, const auto &b) {
        if (a.kind != b.kind)
        {
            return a.kind < b.kind;
        }
        return a.lastModified > b.lastModified;
    });
    return result;
}

ExpectedStr<void> stageRecoveryFile(const QString &settingsDirectory,
                                    const RecoveryFile &file)
{
    return stageRecoveryFiles(settingsDirectory, {file});
}

ExpectedStr<void> stageRecoveryFiles(const QString &settingsDirectory,
                                     const QVector<RecoveryFile> &files)
{
    if (files.isEmpty())
    {
        return makeUnexpected(
            QStringLiteral("Choose at least one backup to restore."));
    }

    QHash<QString, QByteArray> contentsByTarget;
    for (const auto &file : files)
    {
        if (!ALLOWED_TARGETS.contains(file.targetFile) ||
            contentsByTarget.contains(file.targetFile) ||
            file.state == RecoveryState::Damaged ||
            file.state == RecoveryState::Unreadable)
        {
            return makeUnexpected(QStringLiteral(
                "Choose one readable backup for each selected section."));
        }
        const QFileInfo sourceInfo(file.sourcePath);
        const QFileInfo settingsInfo(settingsDirectory);
        const auto backupPrefix = file.targetFile + QStringLiteral(".bkp-");
        const auto rescuePrefix = file.targetFile + QStringLiteral(".rescue-");
        if (sourceInfo.absolutePath() != settingsInfo.absoluteFilePath() ||
            sourceInfo.fileName() == file.targetFile ||
            (!sourceInfo.fileName().startsWith(backupPrefix) &&
             !sourceInfo.fileName().startsWith(rescuePrefix)))
        {
            return makeUnexpected(QStringLiteral(
                "That file is not a Moltorino backup or rescue copy."));
        }
        auto contents = readFile(file.sourcePath);
        if (!contents)
        {
            return makeUnexpected(contents.error());
        }
        contentsByTarget.insert(file.targetFile, contents.value());
    }
    return stageFiles(settingsDirectory, contentsByTarget);
}

bool applyPendingRestore(const QString &settingsDirectory,
                         QString *errorMessage)
{
    const auto staging = combinePath(settingsDirectory, STAGING_DIRECTORY);
    const auto manifestPath = combinePath(staging, MANIFEST_FILE);
    if (!QFileInfo::exists(manifestPath))
    {
        return true;
    }

    const auto fail = [&](const QString &message) {
        if (errorMessage)
        {
            *errorMessage = message;
        }
        return false;
    };
    auto manifestBytes = readFile(manifestPath);
    if (!manifestBytes)
    {
        return fail(manifestBytes.error());
    }
    auto parsed = parseJson(manifestBytes.value());
    if (!parsed || !parsed->isObject() ||
        parsed->toObject().value(QStringLiteral("version")).toInt() != 1 ||
        !parsed->toObject().value(QStringLiteral("files")).isArray())
    {
        return fail(
            parsed ? QStringLiteral("The pending settings restore is invalid.")
                   : parsed.error());
    }

    struct Pending {
        QString stagedPath;
        QString target;
        QByteArray contents;
    };
    QVector<Pending> pending;
    QSet<QString> pendingTargets;
    for (const auto &value :
         parsed->toObject().value(QStringLiteral("files")).toArray())
    {
        const auto spec = value.toObject();
        const auto stagedName = spec.value(QStringLiteral("staged")).toString();
        const auto target = spec.value(QStringLiteral("target")).toString();
        if (!ALLOWED_TARGETS.contains(target) ||
            pendingTargets.contains(target) ||
            QFileInfo(stagedName).fileName() != stagedName ||
            !stagedName.endsWith(QStringLiteral(".pending")))
        {
            return fail(QStringLiteral(
                "The pending restore contains an unsupported file."));
        }
        const auto stagedPath = combinePath(staging, stagedName);
        auto contents = readFile(stagedPath);
        if (!contents)
        {
            return fail(contents.error());
        }
        auto stagedJson = parseJson(contents.value());
        if (!stagedJson || !isValidTargetJson(target, stagedJson.value()))
        {
            return fail(QStringLiteral(
                "A staged restore file is damaged or has the wrong format."));
        }
        pendingTargets.insert(target);
        pending.append({stagedPath, target, contents.value()});
    }
    if (pending.isEmpty())
    {
        return fail(QStringLiteral("The pending restore contains no files."));
    }

    struct Rescue {
        QString targetPath;
        QString rescuePath;
        bool targetExisted = false;
    };
    QVector<Rescue> rescues;
    rescues.reserve(pending.size());
    const auto stamp = QDateTime::currentDateTimeUtc().toString(
                           QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
                       u'-' +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (const auto &file : pending)
    {
        const auto targetPath = combinePath(settingsDirectory, file.target);
        Rescue rescue{
            .targetPath = targetPath,
            .rescuePath = targetPath + QStringLiteral(".rescue-") + stamp,
            .targetExisted = QFileInfo::exists(targetPath),
        };
        if (rescue.targetExisted &&
            !QFile::copy(rescue.targetPath, rescue.rescuePath))
        {
            for (const auto &created : rescues)
            {
                if (created.targetExisted)
                {
                    QFile::remove(created.rescuePath);
                }
            }
            return fail(QStringLiteral("Couldn't create a rescue copy of %1.")
                            .arg(file.target));
        }
        rescues.append(std::move(rescue));
    }

    for (int index = 0; index < pending.size(); ++index)
    {
        const auto &file = pending.at(index);
        const auto &rescue = rescues.at(index);
        auto write = writeFile(rescue.targetPath, file.contents);
        if (!write)
        {
            QString rollbackError;
            for (int rollback = index - 1; rollback >= 0; --rollback)
            {
                const auto &previous = rescues.at(rollback);
                if (!previous.targetExisted)
                {
                    if (!QFile::remove(previous.targetPath) &&
                        QFileInfo::exists(previous.targetPath))
                    {
                        rollbackError = QStringLiteral(
                            " An earlier settings file could not be restored.");
                    }
                    continue;
                }
                auto original = readFile(previous.rescuePath);
                if (!original ||
                    !writeFile(previous.targetPath, original.value()))
                {
                    rollbackError = QStringLiteral(
                        " An earlier settings file could not be restored.");
                }
            }
            return fail(write.error() + rollbackError);
        }
    }

    const auto completedManifestPath = combinePath(
        staging, QStringLiteral("applied-%1.json")
                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QFile::rename(manifestPath, completedManifestPath) &&
        QFileInfo::exists(manifestPath) && !QFile::remove(manifestPath))
    {
        return fail(QStringLiteral(
            "The settings were restored, but Moltorino couldn't mark the "
            "restore as complete. Close Moltorino and try again before "
            "changing any settings."));
    }

    for (const auto &file : pending)
    {
        QFile::remove(file.stagedPath);
    }
    QFile::remove(completedManifestPath);
    QDir(staging).removeRecursively();

    const QDir settingsDir(settingsDirectory);
    for (const auto &file : pending)
    {
        const auto rescueFiles = settingsDir.entryInfoList(
            {file.target + QStringLiteral(".rescue-*")}, QDir::Files,
            QDir::Time);
        for (int index = MAX_RESCUE_COPIES; index < rescueFiles.size(); ++index)
        {
            QFile::remove(rescueFiles.at(index).absoluteFilePath());
        }
    }
    return true;
}

QString recoveryStateText(RecoveryState state)
{
    switch (state)
    {
        case RecoveryState::Healthy:
            return QStringLiteral("Ready");
        case RecoveryState::Empty:
            return QStringLiteral("Empty");
        case RecoveryState::Damaged:
            return QStringLiteral("Damaged");
        case RecoveryState::Unreadable:
            return QStringLiteral("Unreadable");
    }
    return QStringLiteral("Unknown");
}

QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)
    {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024)
    {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

}
