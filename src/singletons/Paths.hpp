// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace chatterino {

class Modes;

class Paths
{
public:
    Paths();
    explicit Paths(const Modes &modes);

    QString rootAppDataDirectory;

    QString settingsDirectory;

    QString messageLogDirectory;

    QString miscDirectory;

    QString crashdumpDirectory;

    QString applicationFilePathHash;

    QString twitchProfileAvatars;

    QString pluginsDirectory;

    QString themesDirectory;
    QStringList bluzyrinoDataDirectories;

    QString dictionariesDirectory;

    QString ipcDirectory;

    bool createFolder(const QString &folderPath);
    [[deprecated("use Modes::instance().portable instead")]] bool isPortable()
        const;

    QString cacheDirectory() const;

    QString cacheFilePath(const QString &fileName) const;

private:
    void initAppFilePathHash();
    void initCheckPortable();
    void initRootDirectory(const Modes &modes);
    void initSubDirectories();

    std::optional<bool> portable_;

    QString cacheDirectory_;
};

}
