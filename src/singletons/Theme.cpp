// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Theme.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/ThemeWallpaper.hpp"
#include "singletons/WindowManager.hpp"
#include "util/RapidjsonHelpers.hpp"

#include <pajlada/settings/settingmanager.hpp>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif
#include <QApplication>

#include <cmath>
#include <numbers>

namespace {

using namespace chatterino;
using namespace literals;

void parseInto(const QJsonObject &obj, const QJsonObject &fallbackObj,
               QLatin1String key, QColor &color)
{
    auto parseColorFrom = [](const auto &obj,
                             QLatin1String key) -> std::optional<QColor> {
        auto jsonValue = obj[key];
        if (!jsonValue.isString()) [[unlikely]]
        {
            return std::nullopt;
        }
        QColor parsed = {jsonValue.toString()};
        if (!parsed.isValid()) [[unlikely]]
        {
            qCWarning(chatterinoTheme).nospace()
                << "While parsing " << key << ": '" << jsonValue.toString()
                << "' isn't a valid color.";
            return std::nullopt;
        }
        return parsed;
    };

    auto firstColor = parseColorFrom(obj, key);
    if (firstColor.has_value())
    {
        color = firstColor.value();
        return;
    }

    if (!fallbackObj.isEmpty())
    {
        auto fallbackColor = parseColorFrom(fallbackObj, key);
        if (fallbackColor.has_value())
        {
            color = fallbackColor.value();
            return;
        }
    }

    qCWarning(chatterinoTheme) << key
                               << "was expected but not found in the "
                                  "current theme, and no fallback value found.";
}

#define _c2StringLit(s, ty) s##ty
#define parseColor(to, from, key) \
    parseInto(from, from##Fallback, _c2StringLit(#key, _L1), (to).from.key)

void parseWindow(const QJsonObject &window, const QJsonObject &windowFallback,
                 chatterino::Theme &theme)
{
    parseColor(theme, window, background);
    parseColor(theme, window, text);
}

void parseTabs(const QJsonObject &tabs, const QJsonObject &tabsFallback,
               chatterino::Theme &theme)
{
    const auto parseTabColors = [](const auto &json, const auto &jsonFallback,
                                   auto &tab) {
        parseInto(json, jsonFallback, "text"_L1, tab.text);
        {
            const auto backgrounds = json["backgrounds"_L1].toObject();
            const auto backgroundsFallback =
                jsonFallback["backgrounds"_L1].toObject();
            parseColor(tab, backgrounds, regular);
            parseColor(tab, backgrounds, hover);
            parseColor(tab, backgrounds, unfocused);
        }
        {
            const auto line = json["line"_L1].toObject();
            const auto lineFallback = jsonFallback["line"_L1].toObject();
            parseColor(tab, line, regular);
            parseColor(tab, line, hover);
            parseColor(tab, line, unfocused);
        }
    };
    parseColor(theme, tabs, dividerLine);
    parseColor(theme, tabs, liveIndicator);
    parseColor(theme, tabs, rerunIndicator);
    parseTabColors(tabs["regular"_L1].toObject(),
                   tabsFallback["regular"_L1].toObject(), theme.tabs.regular);
    parseTabColors(tabs["newMessage"_L1].toObject(),
                   tabsFallback["newMessage"_L1].toObject(),
                   theme.tabs.newMessage);
    parseTabColors(tabs["highlighted"_L1].toObject(),
                   tabsFallback["highlighted"_L1].toObject(),
                   theme.tabs.highlighted);
    parseTabColors(tabs["selected"_L1].toObject(),
                   tabsFallback["selected"_L1].toObject(), theme.tabs.selected);
}

void parseTextColors(const QJsonObject &textColors,
                     const QJsonObject &textColorsFallback, auto &messages)
{
    parseColor(messages, textColors, regular);
    parseColor(messages, textColors, caret);
    parseColor(messages, textColors, link);
    parseColor(messages, textColors, system);

    messages.textColors.timestamp = messages.textColors.system;
    if (textColors.contains("timestamp"_L1) ||
        textColorsFallback.contains("timestamp"_L1))
    {
        parseColor(messages, textColors, timestamp);
    }
    parseColor(messages, textColors, chatPlaceholder);
}

void parseMessageBackgrounds(const QJsonObject &backgrounds,
                             const QJsonObject &backgroundsFallback,
                             auto &messages)
{
    parseColor(messages, backgrounds, regular);
    parseColor(messages, backgrounds, alternate);
}

void parseMessages(const QJsonObject &messages,
                   const QJsonObject &messagesFallback,
                   chatterino::Theme &theme)
{
    parseTextColors(messages["textColors"_L1].toObject(),
                    messagesFallback["textColors"_L1].toObject(),
                    theme.messages);
    parseMessageBackgrounds(messages["backgrounds"_L1].toObject(),
                            messagesFallback["backgrounds"_L1].toObject(),
                            theme.messages);
    parseColor(theme, messages, disabled);
    parseColor(theme, messages, selection);
    parseColor(theme, messages, highlightAnimationStart);
    parseColor(theme, messages, highlightAnimationEnd);
}

void parseOverlayMessages(const QJsonObject &overlayMessages,
                          const QJsonObject &overlayMessagesFallback,
                          chatterino::Theme &theme)
{
    parseTextColors(overlayMessages["textColors"_L1].toObject(),
                    overlayMessagesFallback["textColors"_L1].toObject(),
                    theme.overlayMessages);
    parseMessageBackgrounds(
        overlayMessages["backgrounds"_L1].toObject(),
        overlayMessagesFallback["backgrounds"_L1].toObject(),
        theme.overlayMessages);
    parseColor(theme, overlayMessages, disabled);
    parseColor(theme, overlayMessages, selection);
    parseColor(theme, overlayMessages, background);
}

void parseScrollbars(const QJsonObject &scrollbars,
                     const QJsonObject &scrollbarsFallback,
                     chatterino::Theme &theme)
{
    parseColor(theme, scrollbars, background);
    parseColor(theme, scrollbars, thumb);
    parseColor(theme, scrollbars, thumbSelected);
}

void parseSplits(const QJsonObject &splits, const QJsonObject &splitsFallback,
                 chatterino::Theme &theme)
{
    parseColor(theme, splits, messageSeperator);
    parseColor(theme, splits, background);
    parseColor(theme, splits, dropPreview);
    parseColor(theme, splits, dropPreviewBorder);
    parseColor(theme, splits, dropTargetRect);
    parseColor(theme, splits, dropTargetRectBorder);
    parseColor(theme, splits, resizeHandle);
    parseColor(theme, splits, resizeHandleBackground);

    {
        const auto header = splits["header"_L1].toObject();
        const auto headerFallback = splitsFallback["header"_L1].toObject();
        parseColor(theme.splits, header, border);
        parseColor(theme.splits, header, focusedBorder);
        parseColor(theme.splits, header, background);
        parseColor(theme.splits, header, focusedBackground);
        parseColor(theme.splits, header, text);
        parseColor(theme.splits, header, focusedText);
    }
    {
        const auto input = splits["input"_L1].toObject();
        const auto inputFallback = splitsFallback["input"_L1].toObject();
        parseColor(theme.splits, input, background);
        parseColor(theme.splits, input, backgroundPulse);
        parseColor(theme.splits, input, searchHighlightBackground);
        parseColor(theme.splits, input, searchFailText);
        parseColor(theme.splits, input, text);
    }
}

void parseColors(const QJsonObject &root, const QJsonObject &fallbackTheme,
                 chatterino::Theme &theme)
{
    const auto colors = root["colors"_L1].toObject();
    const auto fallbackColors = fallbackTheme["colors"_L1].toObject();

    parseInto(colors, fallbackColors, "accent"_L1, theme.accent);

    parseWindow(colors["window"_L1].toObject(),
                fallbackColors["window"_L1].toObject(), theme);
    parseTabs(colors["tabs"_L1].toObject(),
              fallbackColors["tabs"_L1].toObject(), theme);
    parseMessages(colors["messages"_L1].toObject(),
                  fallbackColors["messages"_L1].toObject(), theme);
    parseOverlayMessages(colors["overlayMessages"_L1].toObject(),
                         fallbackColors["overlayMessages"_L1].toObject(),
                         theme);
    parseScrollbars(colors["scrollbars"_L1].toObject(),
                    fallbackColors["scrollbars"_L1].toObject(), theme);
    parseSplits(colors["splits"_L1].toObject(),
                fallbackColors["splits"_L1].toObject(), theme);
}
#undef parseColor
#undef _c2StringLit

std::optional<QJsonObject> loadThemeFromPath(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::ReadOnly))
    {
        qCWarning(chatterinoTheme)
            << "Failed to open" << file.fileName() << "at" << path;
        return std::nullopt;
    }

    constexpr qint64 maxBytes = 32 * 1024 * 1024;
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || !file.atEnd() ||
        file.error() != QFile::NoError)
    {
        return std::nullopt;
    }
    QJsonParseError error{};
    auto json = QJsonDocument::fromJson(bytes, &error);
    if (!json.isObject())
    {
        qCWarning(chatterinoTheme) << "Failed to parse" << file.fileName()
                                   << "error:" << error.errorString();
        return std::nullopt;
    }

    return json.object();
}

std::optional<QJsonObject> loadTheme(const ThemeDescriptor &theme)
{
    if (theme.path.startsWith(u"builtin:"_s))
    {
        if (auto profile =
                builtInCustomizationProfile(QStringView(theme.path).sliced(8)))
        {
            return buildCustomizedTheme(*profile);
        }
        return std::nullopt;
    }
    return loadThemeFromPath(theme.path);
}

}

namespace chatterino {

const std::vector<ThemeDescriptor> Theme::builtInThemes{
    {
        .key = "White",
        .path = ":/themes/White.json",
        .name = "White",
    },
    {
        .key = "Light",
        .path = ":/themes/Light.json",
        .name = "Light",
    },
    {
        .key = "Dark",
        .path = ":/themes/Dark.json",
        .name = "Dark",
    },
    {
        .key = "Black",
        .path = ":/themes/Black.json",
        .name = "Black",
    },
    {
        .key = "Moltorino Midnight",
        .path = "builtin:moltorino-midnight",
        .name = "Moltorino",
    },
    {
        .key = "Sakura Dream",
        .path = "builtin:sakura-dream",
        .name = "Sakura",
    },
    {
        .key = "Aurora Calm",
        .path = "builtin:aurora-calm",
        .name = "Garden",
    },
    {
        .key = "Ember Arcade",
        .path = "builtin:ember-arcade",
        .name = "Arcade",
    },
    {
        .key = "Violet Dusk",
        .path = "builtin:violet-dusk",
        .name = "Afterhours",
    },
};

const ThemeDescriptor Theme::fallbackTheme = Theme::builtInThemes.at(2);

bool Theme::isLightTheme() const
{
    return this->isLight_;
}

bool Theme::isSystemTheme() const
{
    return this->themeName == u"System"_s;
}

Theme::Theme(const Paths &paths)
    : themesDirectory_(paths.themesDirectory)
{
    const auto settings = pajlada::Settings::SettingManager::getInstance();
    constexpr auto oldMarker =
        "/appearance/theme/moltorinoDefaultMigrationDone";
    if (const auto *old = settings->get(oldMarker))
    {
        if (settings->get(this->defaultApplied.getPath()) == nullptr)
        {
            this->defaultApplied.setValue(old->IsBool() && old->GetBool());
        }
        settings->removeSetting(oldMarker);
    }
    if (!this->defaultApplied.getValue())
    {
        if (this->themeName == u"Dark"_s)
        {
            this->themeName.setValue(u"Moltorino Midnight"_s);
        }
        this->defaultApplied.setValue(true);
    }

    this->loadAvailableThemes(paths);
    const auto savedSelection = this->moltorinoSelection.getValue();
    bool preserveSelection = !savedSelection.isEmpty() &&
                             (savedSelection == u"System" ||
                              this->findThemeByKey(savedSelection).has_value());
    if (preserveSelection)
    {
        this->themeName = savedSelection;
    }
    else if (const auto selected = this->findThemeByKey(this->themeName))
    {
        const auto json =
            selected->custom ? loadTheme(*selected) : std::nullopt;
        preserveSelection =
            (selected->path.startsWith(u"builtin:") &&
             selected->key != u"Moltorino Midnight") ||
            (json && customizationProfileFromTheme(*json).has_value());
        if (preserveSelection && getSettings()->isSaveEnabled())
        {
            this->moltorinoSelection = this->themeName.getValue();
        }
    }

    const bool alreadyMigrated = this->bluzyrinoMigrated.getValue();
    this->migrateBluzyrinoTheme(paths, preserveSelection);
    if (!alreadyMigrated && this->bluzyrinoMigrated.getValue())
    {
        this->reloadAvailableThemes();
    }

    const auto *sharedPalette = settings->get("/bluzyrino/theme");
    const bool pendingMigration = sharedPalette != nullptr &&
                                  sharedPalette->IsObject() &&
                                  !this->bluzyrinoMigrated.getValue();
    if (getSettings()->isSaveEnabled() && !pendingMigration &&
        (this->isSystemTheme() || this->findThemeByKey(this->themeName)))
    {
        this->moltorinoSelection = this->themeName.getValue();
    }

    this->themeName.connect(
        [this](auto themeName) {
            if (getSettings()->isSaveEnabled())
            {
                this->moltorinoSelection = themeName;
            }
            qCInfo(chatterinoTheme) << "Theme updated to" << themeName;
            this->update();
        },
        false);
    auto updateIfSystem = [this](const auto &) {
        if (this->isSystemTheme())
        {
            this->update();
        }
    };
    this->darkSystemThemeName.connect(updateIfSystem, false);
    this->lightSystemThemeName.connect(updateIfSystem, false);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QApplication::styleHints(),
                     &QStyleHints::colorSchemeChanged, &this->lifetime_,
                     [this] {
                         if (this->isSystemTheme())
                         {
                             this->update();
                             getApp()->getWindows()->forceLayoutChannelViews();
                         }
                     });
#endif

    this->update();
}

void Theme::selectTheme(const QString &key)
{
    if (getSettings()->isSaveEnabled())
    {
        this->moltorinoSelection = key;
    }
    this->themeName = key;
}

void Theme::migrateBluzyrinoTheme(const Paths &paths, bool preserveSelection)
{
    if (this->bluzyrinoMigrated.getValue() || !getSettings()->isSaveEnabled())
    {
        return;
    }
    const auto settings = pajlada::Settings::SettingManager::getInstance();

    const auto readObject = [&settings](const char *path) {
        const auto *value = settings->get(path);
        return value != nullptr && value->IsObject()
                   ? QJsonDocument::fromJson(rj::stringify(*value).toUtf8())
                         .object()
                   : QJsonObject{};
    };
    const auto *source = settings->get("/bluzyrino/theme");
    const bool sharedProfile = source != nullptr && source->IsObject();
    QJsonObject palette = readObject("/bluzyrino/theme");
    QJsonObject appearance{
        {u"backgroundImage"_s, readObject("/appearance/backgroundImage")},
        {u"messages"_s, readObject("/appearance/messages")}};
    auto background =
        QDir(this->themesDirectory_).filePath(u"background.png"_s);
    if (!sharedProfile)
    {
        bool found = false;
        const QFileInfo activeSettings(
            QDir(paths.settingsDirectory).filePath(u"settings.json"_s));
        for (const auto &root : paths.bluzyrinoDataDirectories)
        {
            const auto fileName =
                QDir(root).filePath(u"Settings/settings.json"_s);
            if (QFileInfo(fileName) == activeSettings)
            {
                continue;
            }
            QFile file(fileName);
            if (!file.open(QIODevice::ReadOnly) ||
                file.size() > 32 * 1024 * 1024)
            {
                continue;
            }
            const auto bytes = file.read(32 * 1024 * 1024 + 1);
            if (bytes.size() > 32 * 1024 * 1024)
            {
                continue;
            }
            const auto document = QJsonDocument::fromJson(bytes).object();
            const auto theme = document[u"bluzyrino"_s][u"theme"_s];
            if (!theme.isObject() ||
                !customizationProfileFromBluzyrinoSettings(document))
            {
                continue;
            }
            palette = theme.toObject();
            const auto otherAppearance = document[u"appearance"_s].toObject();
            appearance = QJsonObject{
                {u"backgroundImage"_s, otherAppearance[u"backgroundImage"_s]},
                {u"messages"_s, otherAppearance[u"messages"_s]}};
            background = QDir(root).filePath(u"Themes/background.png"_s);
            found = true;
            break;
        }
        if (!found)
        {
            return;
        }
    }
    const QJsonObject input{
        {u"bluzyrino"_s, QJsonObject{{u"theme"_s, palette}}},
        {u"appearance"_s, appearance}};
    auto profile = customizationProfileFromBluzyrinoSettings(input);
    if (!profile)
    {
        qCWarning(chatterinoTheme) << "Cannot migrate invalid Bluzyrino colors";
        return;
    }

    const QDir directory(this->themesDirectory_);
    QString key;
    bool alreadyWritten = false;

    for (int suffix = 1; suffix <= 1000; ++suffix)
    {
        const auto candidate = suffix == 1 ? u"bluzyrino.json"_s
                                           : u"bluzyrino-%1.json"_s.arg(suffix);
        const auto path = directory.filePath(candidate);
        const QFileInfo info(path);
        if (info.isSymLink())
        {
            continue;
        }
        if (!info.exists())
        {
            key = candidate;
            break;
        }
        const auto existing =
            loadTheme({candidate, path, u"Bluzyrino"_s, true});
        if (existing &&
            (*existing)[u"metadata"_s][u"bluzyrinoMigration"_s].toInt() == 1 &&
            customizationProfileFromTheme(*existing))
        {
            key = candidate;
            alreadyWritten = true;
            break;
        }
    }
    if (key.isEmpty())
    {
        qCWarning(chatterinoTheme) << "No free filename for Bluzyrino theme";
        return;
    }
    if (!alreadyWritten)
    {
        if (QFileInfo::exists(background))
        {
            auto data = prepareThemeWallpaper(background);
            if (data.error.isEmpty())
            {
                profile->wallpaperSource = storeThemeWallpaper(
                    data, this->themesDirectory_, &data.error);
            }
            if (!data.error.isEmpty() || profile->wallpaperSource.isEmpty())
            {
                qCWarning(chatterinoTheme)
                    << "Cannot migrate Bluzyrino background:" << data.error;
                return;
            }
        }
        auto theme = buildCustomizedTheme(*profile);
        auto metadata = theme[u"metadata"_s].toObject();
        metadata.insert(u"bluzyrinoMigration"_s, 1);
        theme.insert(u"metadata"_s, metadata);
        const auto bytes = QJsonDocument(theme).toJson();
        QSaveFile file(directory.filePath(key));
        if (!directory.mkpath(u"."_s) || !file.open(QIODevice::WriteOnly) ||
            file.write(bytes) != bytes.size() || !file.commit())
        {
            qCWarning(chatterinoTheme)
                << "Cannot save Bluzyrino theme:" << file.errorString();
            return;
        }
    }
    if (sharedProfile && !preserveSelection &&
        palette[u"customEnabled"_s].toBool(false))
    {
        this->selectTheme(key);
    }
    this->bluzyrinoMigrated = true;
    qCInfo(chatterinoTheme) << "Migrated Bluzyrino appearance to" << key;
}

void Theme::update()
{
    auto currentTheme = [&]() -> QString {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (this->isSystemTheme())
        {
            switch (QApplication::styleHints()->colorScheme())
            {
                case Qt::ColorScheme::Light:
                    return this->lightSystemThemeName;
                case Qt::ColorScheme::Unknown:
                case Qt::ColorScheme::Dark:
                    return this->darkSystemThemeName;
            }
        }
#endif
        return this->themeName;
    };

    auto oTheme = this->findThemeByKey(currentTheme());

    constexpr const double nsToMs = 1.0 / 1000000.0;
    QElapsedTimer timer;
    timer.start();

    std::optional<QJsonObject> themeJSON;
    QString themePath;
    bool isCustomTheme = false;
    if (!oTheme)
    {
        qCWarning(chatterinoTheme)
            << "Theme" << this->themeName
            << "not found, falling back to the fallback theme";

        themeJSON = loadTheme(fallbackTheme);
        themePath = fallbackTheme.path;
    }
    else
    {
        const auto &theme = *oTheme;

        themeJSON = loadTheme(theme);
        themePath = theme.path;

        if (!themeJSON)
        {
            qCWarning(chatterinoTheme)
                << "Theme" << this->themeName
                << "not valid, falling back to the fallback theme";

            themeJSON = loadTheme(fallbackTheme);
            themePath = fallbackTheme.path;
        }
        else
        {
            isCustomTheme = theme.custom;
        }
    }
    auto loadTs = double(timer.nsecsElapsed()) * nsToMs;

    if (!themeJSON)
    {
        qCWarning(chatterinoTheme)
            << "Failed to load" << this->themeName << "or the fallback theme";
        return;
    }

    const auto loadedTheme = *themeJSON;
    if (this->isAutoReloading() && this->currentThemeJson_ == loadedTheme)
    {
        return;
    }

    auto runtimeTheme = loadedTheme;
    const auto metadata = loadedTheme.value(u"metadata"_s).toObject();
    const auto profileColors =
        metadata.value(u"moltorino"_s).toObject().value(u"colors"_s).toObject();
    const auto hasValidSemanticColor = [&profileColors](QStringView key) {
        const auto value = profileColors.value(key);
        return value.isString() && QColor(value.toString()).isValid();
    };
    const bool hasSemanticPalette =
        hasValidSemanticColor(u"background"_s) &&
        hasValidSemanticColor(u"chatBackground"_s) &&
        hasValidSemanticColor(u"surface"_s) &&
        hasValidSemanticColor(u"raisedSurface"_s) &&
        hasValidSemanticColor(u"text"_s) &&
        hasValidSemanticColor(u"mutedText"_s) &&
        hasValidSemanticColor(u"accent"_s);
    if (hasSemanticPalette)
    {
        if (const auto profile = customizationProfileFromTheme(loadedTheme))
        {
            const auto canonical = buildCustomizedTheme(*profile);

            runtimeTheme.insert(u"colors"_s, canonical.value(u"colors"_s));
        }
    }

    this->parseFrom(runtimeTheme, isCustomTheme);
    this->currentThemePath_ = themePath;

    this->currentThemeJson_ = loadedTheme;

    auto parseTs = double(timer.nsecsElapsed()) * nsToMs;

    this->updated.invoke();
    auto updateTs = double(timer.nsecsElapsed()) * nsToMs;
    qCDebug(chatterinoTheme).nospace().noquote()
        << "Updated theme in " << QString::number(updateTs, 'f', 2)
        << "ms (load: " << QString::number(loadTs, 'f', 2)
        << "ms, parse: " << QString::number(parseTs - loadTs, 'f', 2)
        << "ms, update: " << QString::number(updateTs - parseTs, 'f', 2)
        << "ms)";
}

std::vector<std::pair<QString, QVariant>> Theme::availableThemes() const
{
    std::vector<std::pair<QString, QVariant>> packagedThemes;

    for (const auto &theme : this->availableThemes_)
    {
        if (theme.custom)
        {
            auto p = std::make_pair(
                QStringLiteral("Custom: %1").arg(theme.name), theme.key);

            packagedThemes.emplace_back(p);
        }
        else
        {
            auto p = std::make_pair(theme.name, theme.key);

            packagedThemes.emplace_back(p);
        }
    }

    return packagedThemes;
}

const std::vector<ThemeDescriptor> &Theme::availableThemeDescriptors() const
{
    return this->availableThemes_;
}

void Theme::reloadAvailableThemes()
{
    auto availableThemes = Theme::builtInThemes;

    auto dir = QDir(this->themesDirectory_);
    for (const auto &info :
         dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (!info.isFile() ||
            !info.fileName().endsWith(u".json"_s, Qt::CaseInsensitive))
        {
            continue;
        }
        ThemeDescriptor descriptor{info.fileName(), info.absoluteFilePath(),
                                   info.baseName(), true};
        const auto theme = loadTheme(descriptor);
        if (!theme)
        {
            qCWarning(chatterinoTheme) << "Failed to parse theme at" << info;
            continue;
        }
        const auto displayName =
            (*theme)[u"metadata"_s][u"name"_s].toString().trimmed();
        if (!displayName.isEmpty())
        {
            descriptor.name = displayName;
        }
        availableThemes.emplace_back(std::move(descriptor));
    }

    if (availableThemes == this->availableThemes_)
    {
        return;
    }

    this->availableThemes_ = std::move(availableThemes);
    this->availableThemesChanged.invoke();
}

std::optional<QJsonObject> Theme::themeJson(const QString &key) const
{
    for (const auto &descriptor : this->availableThemes_)
    {
        if (descriptor.key == key)
        {
            return loadTheme(descriptor);
        }
    }
    return std::nullopt;
}

const QString &Theme::themesDirectory() const
{
    return this->themesDirectory_;
}

void Theme::loadAvailableThemes(const Paths &paths)
{
    this->themesDirectory_ = paths.themesDirectory;
    this->reloadAvailableThemes();
}

std::optional<ThemeDescriptor> Theme::findThemeByKey(const QString &key)
{
    for (const auto &theme : this->availableThemes_)
    {
        if (theme.key == key)
        {
            return theme;
        }
    }

    return std::nullopt;
}

void Theme::parseFrom(const QJsonObject &root, bool isCustomTheme)
{
    this->isLight_ =
        root["metadata"_L1]["iconTheme"_L1].toString() == u"dark"_s;

    std::optional<QJsonObject> fallbackTheme;
    if (isCustomTheme)
    {

        auto fallbackThemeName =
            root["metadata"_L1]["fallbackTheme"_L1].toString(
                this->isLightTheme() ? "Light" : "Dark");
        for (const auto &theme : Theme::builtInThemes)
        {
            if (fallbackThemeName.compare(theme.key, Qt::CaseInsensitive) == 0)
            {
                fallbackTheme = loadTheme(theme);
                break;
            }
        }
    }

    parseColors(root, fallbackTheme.value_or(QJsonObject()), *this);

    if (auto customization = customizationProfileFromTheme(root))
    {
        this->customization = std::move(*customization);
        this->customization.clearDisabledFontOverrides();
    }
    else
    {
        this->customization = ThemeCustomizationProfile{};
        this->customization.useThemeFonts = false;
        this->customization.useThemeFontSizes = false;
        this->customization.name = u"Classic Chatterino"_s;
        this->customization.background = this->window.background;
        this->customization.chatBackground = this->splits.background;
        this->customization.surface = this->tabs.regular.backgrounds.regular;
        this->customization.raisedSurface =
            this->tabs.selected.backgrounds.regular;
        this->customization.text = this->window.text;
        this->customization.chatText = this->messages.textColors.regular;
        this->customization.separateChatText =
            this->customization.chatText != this->customization.text;
        this->customization.mutedText = this->tabs.regular.text;
        this->customization.systemText = this->messages.textColors.system;
        this->customization.timestampText = this->messages.textColors.timestamp;
        this->customization.accent = this->accent;
        this->customization.foundation = ThemeFoundation::ChatterinoClassic;
        this->customization.useThemeMessageRows = false;
        this->customization.cornerStyle = ThemeCornerStyle::Classic;
        this->customization.tabCornerRadius = 0;
        this->customization.chatCornerRadius = 0;
    }

    this->splits.input.styleSheet = uR"(
        background: %1;
        border: %2;
        color: %3;
        selection-background-color: %4;
    )"_s.arg(
        this->splits.input.background.name(QColor::HexArgb),
        this->tabs.selected.backgrounds.regular.name(QColor::HexArgb),
        this->splits.input.text.name(QColor::HexArgb),
        this->isLightTheme()
            ? u"#68B1FF"_s
            : this->tabs.selected.backgrounds.regular.name(QColor::HexArgb));

    if (this->isLightTheme())
    {
        this->buttons.copy = getResources().buttons.copyDark;
    }
    else
    {
        this->buttons.copy = getResources().buttons.copyLight;
    }

    auto palette = QApplication::palette();

    if (this->isLightTheme())
    {
        palette.setColor(QPalette::Window, this->window.background);
        palette.setColor(QPalette::Base, {0xe9, 0xe9, 0xe9});
        palette.setColor(QPalette::AlternateBase, {0xe0, 0xe0, 0xe0});
        palette.setColor(QPalette::Button, {0xd9, 0xd9, 0xd9});
        palette.setColor(QPalette::Text, this->window.text);
        palette.setColor(QPalette::WindowText, this->window.text);
        palette.setColor(QPalette::ButtonText, this->window.text);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::ToolTipBase, Qt::white);
        palette.setColor(QPalette::ToolTipText, Qt::black);
        palette.setColor(QPalette::Link, Qt::blue);
        palette.setColor(QPalette::LinkVisited, Qt::magenta);
        palette.setColor(QPalette::Highlight, {42, 130, 218});
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::PlaceholderText, {0x90, 0x90, 0x90});
    }
    this->palette = palette;
}

bool Theme::isAutoReloading() const
{
    return this->themeReloadTimer_ != nullptr;
}

void Theme::setAutoReload(bool autoReload)
{
    if (autoReload == this->isAutoReloading())
    {
        return;
    }

    if (!autoReload)
    {
        this->themeReloadTimer_.reset();
        return;
    }

    this->themeReloadTimer_ = std::make_unique<QTimer>();
    QObject::connect(this->themeReloadTimer_.get(), &QTimer::timeout, [this]() {
        this->update();
    });
    this->themeReloadTimer_->setInterval(Theme::AUTO_RELOAD_INTERVAL_MS);
    this->themeReloadTimer_->start();

    qCDebug(chatterinoTheme) << "Enabled theme watcher";
}

void Theme::normalizeColor(QColor &color) const
{
    if (this->isLightTheme())
    {
        if (color.lightnessF() > 0.5)
        {
            color.setHslF(color.hueF(), color.saturationF(), 0.5);
        }

        if (color.lightnessF() > 0.4 && color.hueF() > 0.1 &&
            color.hueF() < 0.33333)
        {
            color.setHslF(
                color.hueF(), color.saturationF(),
                color.lightnessF() - sin((color.hueF() - 0.1) / (0.3333 - 0.1) *
                                         std::numbers::pi) *
                                         color.saturationF() * 0.4);
        }
    }
    else
    {
        if (color.lightnessF() < 0.5)
        {
            color.setHslF(color.hueF(), color.saturationF(), 0.5);
        }

        if (color.lightnessF() < 0.6 && color.hueF() > 0.54444 &&
            color.hueF() < 0.83333)
        {
            color.setHslF(color.hueF(), color.saturationF(),
                          color.lightnessF() +
                              sin((color.hueF() - 0.54444) /
                                  (0.8333 - 0.54444) * std::numbers::pi) *
                                  color.saturationF() * 0.4);
        }
    }
}

Theme *getTheme()
{
    return getApp()->getThemes();
}

}
