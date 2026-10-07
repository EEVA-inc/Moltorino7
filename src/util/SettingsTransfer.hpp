#pragma once

#include "util/Expected.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace chatterino::settingsbackup {

inline constexpr qsizetype MAX_BUNDLE_BYTES = 32 * 1024 * 1024;

struct Category {
    QString id;
    QString name;
    QString description;
};

enum class RecoveryState : uint8_t {
    Healthy,
    Empty,
    Damaged,
    Unreadable,
};

struct RecoveryFile {
    QString sourcePath;
    QString targetFile;
    QString kind;
    QDateTime lastModified;
    qint64 sizeBytes = 0;
    RecoveryState state = RecoveryState::Unreadable;
    QString summary;
};

struct RevokeKey {
    QString code;
    QString token;
};

const QVector<Category> &categories();
QString categoryForSettingPath(const QString &path);
bool isSafeSettingPath(const QString &path);
std::optional<RevokeKey> parseRevokeKey(const QString &input);

ExpectedStr<QJsonObject> createBundle(const QString &settingsDirectory,
                                      const QSet<QString> &selectedCategories);
ExpectedStr<QJsonObject> parseBundle(const QByteArray &contents);
ExpectedStr<QStringList> stageImport(const QString &settingsDirectory,
                                     const QJsonObject &bundle,
                                     const QSet<QString> &selectedCategories,
                                     bool mergeLists);

bool canApplyImportNow(const QJsonObject &bundle,
                       const QSet<QString> &selectedCategories);

ExpectedStr<QStringList> applyImportNow(const QString &settingsDirectory,
                                        const QJsonObject &bundle,
                                        const QSet<QString> &selectedCategories,
                                        bool mergeLists);

QVector<RecoveryFile> inspectRecoveryFiles(const QString &settingsDirectory);
ExpectedStr<void> stageRecoveryFile(const QString &settingsDirectory,
                                    const RecoveryFile &file);
ExpectedStr<void> stageRecoveryFiles(const QString &settingsDirectory,
                                     const QVector<RecoveryFile> &files);

bool applyPendingRestore(const QString &settingsDirectory,
                         QString *errorMessage = nullptr);

QString recoveryStateText(RecoveryState state);
QString formatFileSize(qint64 bytes);

}
