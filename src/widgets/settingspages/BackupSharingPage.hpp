#pragma once

#include "util/SettingsTransfer.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QHash>
#include <QJsonObject>

#include <cstdint>
#include <stop_token>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace chatterino {

class BackupSharingPage final : public SettingsPage
{
public:
    BackupSharingPage();
    ~BackupSharingPage() override;

    void onShow() override;

private:
    QWidget *createExportTab();
    QWidget *createImportTab();
    QWidget *createRecoveryTab();
    QGridLayout *createCategoryGrid(QWidget *parent,
                                    QHash<QString, QCheckBox *> &checkboxes);

    QSet<QString> selectedCategories(
        const QHash<QString, QCheckBox *> &checkboxes) const;
    void saveBundleToFile();
    void shareBundle();
    void deleteSharedBundle();
    void browseImportFile();
    void loadImportSource();
    void setLoadedBundle(const QJsonObject &bundle);
    void updateImportActions();
    void applyLoadedBundleNow();
    void importLoadedBundleAndRestart();
    void refreshRecoveryFiles();
    void updateRecoveryRow(int row);
    void updateRestoreButton();
    void restoreSelectedFile();

    QHash<QString, QCheckBox *> exportChecks_;
    QHash<QString, QCheckBox *> importChecks_;
    QCheckBox *exportEverything_ = nullptr;
    QComboBox *expiry_ = nullptr;
    QPushButton *shareButton_ = nullptr;
    QLineEdit *shareCode_ = nullptr;
    QLineEdit *shareRevokeKey_ = nullptr;
    QPushButton *deleteShareButton_ = nullptr;
    QLabel *exportStatus_ = nullptr;

    QLineEdit *importSource_ = nullptr;
    QLabel *importStatus_ = nullptr;
    QCheckBox *mergeLists_ = nullptr;
    QPushButton *applyNowButton_ = nullptr;
    QPushButton *importRestartButton_ = nullptr;
    QJsonObject loadedBundle_;
    std::uint64_t importLoadGeneration_ = 0;
    std::stop_source importCancellation_;

    struct RecoveryRow {
        QString targetFile;
        QString name;
        QCheckBox *selected = nullptr;
        QComboBox *backup = nullptr;
        QLabel *details = nullptr;
    };
    QVector<RecoveryRow> recoveryRows_;
    QPushButton *restoreButton_ = nullptr;
    QVector<settingsbackup::RecoveryFile> recoveryFiles_;
};

}
