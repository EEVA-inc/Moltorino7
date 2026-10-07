#include "widgets/settingspages/BackupSharingPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/commands/CommandController.hpp"
#include "providers/moltorino/MoltorinoUpdater.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

using namespace chatterino;

QLabel *description(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void styleSection(QGroupBox *section)
{
    section->setStyleSheet(QStringLiteral(R"(
        QGroupBox {
            background-color: #4a4a4a;
            border: 1px solid #3d3d3d;
            border-radius: 0;
            margin-top: 17px;
            padding: 8px 6px 6px 6px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 2px;
            padding: 0;
            color: palette(window-text);
        }
    )"));
}

QSet<QString> contentCategories(const QJsonObject &bundle)
{
    QSet<QString> present;
    const auto settings = bundle.value(QStringLiteral("settings")).toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it)
    {
        if (it.value().isObject() && !it.value().toObject().isEmpty())
        {
            present.insert(it.key());
        }
    }
    const auto files = bundle.value(QStringLiteral("files")).toObject();
    for (const auto &file : files)
    {
        const auto category =
            file.toObject().value(QStringLiteral("category")).toString();
        if (!category.isEmpty())
        {
            present.insert(category);
        }
    }
    return present;
}

QString normalizedShareCode(QString input)
{
    input = input.trimmed();
    const QUrl url(input);
    if (url.isValid() && !url.host().isEmpty())
    {
        const auto parts = url.path().split('/', Qt::SkipEmptyParts);
        if (!parts.isEmpty())
        {
            input = parts.first();
        }
    }
    static const QRegularExpression codePattern(
        QStringLiteral("^[A-Za-z0-9]{4,16}$"));
    if (!codePattern.match(input).hasMatch())
    {
        return {};
    }

    static const QRegularExpression settingsCodePattern(
        QStringLiteral("^[23456789A-HJ-NP-Z]{6}$"),
        QRegularExpression::CaseInsensitiveOption);
    return settingsCodePattern.match(input).hasMatch() ? input.toUpper()
                                                       : input;
}

void restartAfterStaging(QWidget *parent, const QString &action)
{
    if (getMoltorinoUpdater()->restartApplication())
    {
        return;
    }

    QMessageBox::warning(
        parent, QStringLiteral("Restart needed"),
        QStringLiteral("%1 is ready, but Moltorino couldn't restart. Close and "
                       "reopen Moltorino to finish.")
            .arg(action));
}

}

namespace chatterino {

BackupSharingPage::BackupSharingPage()
{
    const QColor disabledText(QStringLiteral("#b8b8b8"));
    auto pagePalette = this->palette();
    pagePalette.setColor(QPalette::Disabled, QPalette::WindowText,
                         disabledText);
    pagePalette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    pagePalette.setColor(QPalette::Disabled, QPalette::ButtonText,
                         disabledText);
    pagePalette.setColor(QPalette::Disabled, QPalette::PlaceholderText,
                         disabledText);
    this->setPalette(pagePalette);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto *tabs = new QTabWidget(this);
    tabs->setStyleSheet(QStringLiteral(R"(
            QCheckBox:disabled {
                color: #b8b8b8;
            }
            QComboBox:disabled {
                color: #dedede;
            }
        )"));
    tabs->addTab(this->createExportTab(), "Export and share");
    tabs->addTab(this->createImportTab(), "Import");
    tabs->addTab(this->createRecoveryTab(), "Recovery");
    root->addWidget(tabs);
    this->refreshRecoveryFiles();
}

BackupSharingPage::~BackupSharingPage()
{
    this->importCancellation_.request_stop();
}

QGridLayout *BackupSharingPage::createCategoryGrid(
    QWidget *parent, QHash<QString, QCheckBox *> &checkboxes)
{
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(20);
    grid->setVerticalSpacing(6);
    int index = 0;
    for (const auto &category : settingsbackup::categories())
    {
        auto *check = new QCheckBox(category.name, parent);
        check->setToolTip(category.description);
        checkboxes.insert(category.id, check);
        grid->addWidget(check, index / 2, index % 2);
        ++index;
    }
    return grid;
}

QWidget *BackupSharingPage::createExportTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    layout->addWidget(
        description("Choose what to export. Accounts, login details, tokens, "
                    "and saved local paths stay out.",
                    page));

    auto *categoriesBox = new QGroupBox("Include", page);
    styleSection(categoriesBox);
    auto *categoriesLayout = new QVBoxLayout(categoriesBox);
    this->exportEverything_ =
        new QCheckBox("Everything except accounts", categoriesBox);
    categoriesLayout->addWidget(this->exportEverything_);
    categoriesLayout->addLayout(
        this->createCategoryGrid(categoriesBox, this->exportChecks_));
    QObject::connect(this->exportEverything_, &QCheckBox::toggled, this,
                     [this](bool enabled) {
                         for (auto *check : this->exportChecks_)
                         {
                             if (enabled)
                             {
                                 check->setChecked(true);
                             }
                             check->setEnabled(!enabled);
                         }
                     });
    this->exportEverything_->setChecked(true);
    layout->addWidget(categoriesBox);

    auto *exportBox = new QGroupBox("Export", page);
    styleSection(exportBox);
    auto *exportGrid = new QGridLayout(exportBox);
    exportGrid->setColumnStretch(1, 1);
    exportGrid->setHorizontalSpacing(10);
    exportGrid->setVerticalSpacing(8);

    this->expiry_ = new QComboBox(exportBox);
    this->expiry_->addItem("Never", QVariant{});
    for (const auto days : {1, 7, 14, 30, 90, 365})
    {
        this->expiry_->addItem(days == 1 ? QStringLiteral("1 day")
                                         : QStringLiteral("%1 days").arg(days),
                               days);
    }
    this->expiry_->setFixedWidth(105);
    this->shareButton_ = new QPushButton("Create sharing code", exportBox);
    auto *save = new QPushButton("Save settings file", exportBox);
    save->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    exportGrid->addWidget(new QLabel("Local backup:", exportBox), 0, 0);
    exportGrid->addWidget(save, 0, 1);
    exportGrid->addWidget(new QLabel("Share online:", exportBox), 0, 2);
    exportGrid->addWidget(new QLabel("Expires:", exportBox), 0, 3);
    exportGrid->addWidget(this->expiry_, 0, 4);
    exportGrid->addWidget(this->shareButton_, 0, 5);
    QObject::connect(save, &QPushButton::clicked, this,
                     &BackupSharingPage::saveBundleToFile);
    QObject::connect(this->shareButton_, &QPushButton::clicked, this,
                     &BackupSharingPage::shareBundle);

    this->shareCode_ = new QLineEdit(exportBox);
    this->shareCode_->setReadOnly(true);
    this->shareCode_->setPlaceholderText("Created settings code");
    auto *copyCode = new QPushButton("Copy", exportBox);
    exportGrid->addWidget(new QLabel("Settings code:", exportBox), 1, 0);
    exportGrid->addWidget(this->shareCode_, 1, 1, 1, 4);
    exportGrid->addWidget(copyCode, 1, 5);
    QObject::connect(copyCode, &QPushButton::clicked, this, [this] {
        if (!this->shareCode_->text().isEmpty())
        {
            crossPlatformCopy(this->shareCode_->text());
        }
    });

    this->shareRevokeKey_ = new QLineEdit(exportBox);
    this->shareRevokeKey_->setEchoMode(QLineEdit::Password);
    this->shareRevokeKey_->setPlaceholderText(
        "Created revoke key or paste a saved key");
    auto *revealRevokeKey = this->shareRevokeKey_->addAction(
        QIcon(QStringLiteral(":/buttons/reveal.svg")),
        QLineEdit::TrailingPosition);
    revealRevokeKey->setCheckable(true);
    revealRevokeKey->setToolTip("Show revoke key");
    QObject::connect(
        revealRevokeKey, &QAction::toggled, this,
        [this, revealRevokeKey](bool revealed) {
            this->shareRevokeKey_->setEchoMode(revealed ? QLineEdit::Normal
                                                        : QLineEdit::Password);
            revealRevokeKey->setIcon(
                QIcon(revealed ? QStringLiteral(":/buttons/conceal.svg")
                               : QStringLiteral(":/buttons/reveal.svg")));
            revealRevokeKey->setToolTip(
                revealed ? QStringLiteral("Hide revoke key")
                         : QStringLiteral("Show revoke key"));
        });
    auto *copyDelete = new QPushButton("Copy", exportBox);
    this->deleteShareButton_ = new QPushButton("Revoke code", exportBox);
    this->deleteShareButton_->setEnabled(false);
    auto *revokeButtons = new QHBoxLayout;
    revokeButtons->setContentsMargins(0, 0, 0, 0);
    revokeButtons->setSpacing(6);
    revokeButtons->addWidget(copyDelete);
    revokeButtons->addWidget(this->deleteShareButton_);
    exportGrid->addWidget(new QLabel("Revoke key:", exportBox), 2, 0);
    exportGrid->addWidget(this->shareRevokeKey_, 2, 1, 1, 4);
    exportGrid->addLayout(revokeButtons, 2, 5);
    QObject::connect(copyDelete, &QPushButton::clicked, this, [this] {
        if (!this->shareRevokeKey_->text().isEmpty())
        {
            crossPlatformCopy(this->shareRevokeKey_->text());
        }
    });
    QObject::connect(this->deleteShareButton_, &QPushButton::clicked, this,
                     &BackupSharingPage::deleteSharedBundle);
    const auto updateDeleteButton = [this, revealRevokeKey] {
        if (this->shareRevokeKey_->text().isEmpty() &&
            revealRevokeKey->isChecked())
        {
            revealRevokeKey->setChecked(false);
        }
        this->deleteShareButton_->setEnabled(
            settingsbackup::parseRevokeKey(this->shareRevokeKey_->text())
                .has_value());
    };
    QObject::connect(this->shareRevokeKey_, &QLineEdit::textChanged, this,
                     updateDeleteButton);

    this->exportStatus_ = description({}, exportBox);
    this->exportStatus_->setToolTip(
        "Sharing codes are public to anyone who knows or guesses the code.");
    exportGrid->addWidget(this->exportStatus_, 3, 0, 1, 6);
    exportGrid->addWidget(
        description("Anyone with the code can open the shared settings.",
                    exportBox),
        4, 0, 1, 6);
    layout->addWidget(exportBox);
    layout->addStretch(1);
    return page;
}

QWidget *BackupSharingPage::createImportTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    layout->addWidget(description(
        "Paste a Moltorino sharing code or choose an exported settings file.",
        page));

    auto *sourceRow = new QHBoxLayout;
    this->importSource_ = new QLineEdit(page);
    this->importSource_->setPlaceholderText("Sharing code or file path");
    auto *browse = new QPushButton("Browse...", page);
    auto *load = new QPushButton("Load", page);
    sourceRow->addWidget(this->importSource_, 1);
    sourceRow->addWidget(browse);
    sourceRow->addWidget(load);
    layout->addLayout(sourceRow);
    QObject::connect(browse, &QPushButton::clicked, this,
                     &BackupSharingPage::browseImportFile);
    QObject::connect(load, &QPushButton::clicked, this,
                     &BackupSharingPage::loadImportSource);
    QObject::connect(this->importSource_, &QLineEdit::returnPressed, this,
                     &BackupSharingPage::loadImportSource);

    auto *categoriesBox = new QGroupBox("Import", page);
    styleSection(categoriesBox);
    auto *categoriesLayout = new QVBoxLayout(categoriesBox);
    categoriesLayout->addLayout(
        this->createCategoryGrid(categoriesBox, this->importChecks_));
    for (auto *check : this->importChecks_)
    {
        check->setEnabled(false);
        QObject::connect(check, &QCheckBox::toggled, this,
                         &BackupSharingPage::updateImportActions);
    }
    this->mergeLists_ = new QCheckBox(
        "Merge lists and keep entries that are already here", categoriesBox);
    this->mergeLists_->setChecked(true);
    categoriesLayout->addWidget(this->mergeLists_);
    layout->addWidget(categoriesBox);

    this->importStatus_ =
        description("Load a code or file to preview it.", page);
    layout->addWidget(this->importStatus_);
    auto *importActions = new QHBoxLayout;
    this->applyNowButton_ = new QPushButton("Apply now", page);
    this->importRestartButton_ = new QPushButton("Import and restart", page);
    this->applyNowButton_->setEnabled(false);
    this->importRestartButton_->setEnabled(false);
    importActions->addWidget(this->applyNowButton_);
    importActions->addWidget(this->importRestartButton_);
    importActions->addStretch(1);
    layout->addLayout(importActions);
    QObject::connect(this->applyNowButton_, &QPushButton::clicked, this,
                     &BackupSharingPage::applyLoadedBundleNow);
    QObject::connect(this->importRestartButton_, &QPushButton::clicked, this,
                     &BackupSharingPage::importLoadedBundleAndRestart);
    layout->addStretch(1);
    return page;
}

QWidget *BackupSharingPage::createRecoveryTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    layout->addWidget(description(
        "Choose a backup for each section you want to recover. Moltorino "
        "keeps a rescue copy of your current data before restoring anything.",
        page));

    auto *recoveryBox = new QGroupBox("Available backups", page);
    styleSection(recoveryBox);
    auto *recoveryLayout = new QGridLayout(recoveryBox);
    recoveryLayout->setColumnStretch(1, 1);
    recoveryLayout->setHorizontalSpacing(12);
    recoveryLayout->setVerticalSpacing(5);

    const QVector<std::pair<QString, QString>> sections{
        {QStringLiteral("settings.json"), QStringLiteral("Settings")},
        {QStringLiteral("window-layout.json"),
         QStringLiteral("Tabs and layout")},
        {QStringLiteral("commands.json"), QStringLiteral("Commands")},
        {QStringLiteral("chat-automations.json"),
         QStringLiteral("Chat automations")},
        {QStringLiteral("user-data.json"),
         QStringLiteral("User notes and colors")},
    };
    int gridRow = 0;
    for (const auto &[target, name] : sections)
    {
        RecoveryRow row{
            .targetFile = target,
            .name = name,
            .selected = new QCheckBox(name, recoveryBox),
            .backup = new QComboBox(recoveryBox),
            .details = description({}, recoveryBox),
        };
        row.backup->setEnabled(false);
        row.backup->setMinimumWidth(280);
        row.details->setStyleSheet("QLabel { color: #c8c8c8; }");
        recoveryLayout->addWidget(row.selected, gridRow, 0, Qt::AlignTop);
        recoveryLayout->addWidget(row.backup, gridRow, 1);
        recoveryLayout->addWidget(row.details, gridRow + 1, 1);

        const auto rowIndex = this->recoveryRows_.size();
        QObject::connect(row.selected, &QCheckBox::toggled, this,
                         [this, rowIndex](bool selected) {
                             auto &current = this->recoveryRows_[rowIndex];
                             current.backup->setEnabled(
                                 selected && current.backup->count() > 0);
                             this->updateRecoveryRow(rowIndex);
                             this->updateRestoreButton();
                         });
        QObject::connect(row.backup, &QComboBox::currentIndexChanged, this,
                         [this, rowIndex](int) {
                             this->updateRecoveryRow(rowIndex);
                         });
        this->recoveryRows_.append(std::move(row));
        gridRow += 2;
    }
    layout->addWidget(recoveryBox);

    auto *buttons = new QHBoxLayout;
    auto *refresh = new QPushButton("Refresh", page);
    this->restoreButton_ = new QPushButton("Restore selected", page);
    this->restoreButton_->setEnabled(false);
    buttons->addWidget(refresh);
    buttons->addWidget(this->restoreButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    QObject::connect(refresh, &QPushButton::clicked, this,
                     &BackupSharingPage::refreshRecoveryFiles);
    QObject::connect(this->restoreButton_, &QPushButton::clicked, this,
                     &BackupSharingPage::restoreSelectedFile);
    layout->addStretch(1);
    return page;
}

QSet<QString> BackupSharingPage::selectedCategories(
    const QHash<QString, QCheckBox *> &checkboxes) const
{
    QSet<QString> selected;
    for (auto it = checkboxes.begin(); it != checkboxes.end(); ++it)
    {
        if (it.value()->isChecked())
        {
            selected.insert(it.key());
        }
    }
    return selected;
}

void BackupSharingPage::saveBundleToFile()
{
    getApp()->getCommands()->save();
    getSettings()->requestSave();
    getApp()->getWindows()->save();
    auto bundle = settingsbackup::createBundle(
        getApp()->getPaths().settingsDirectory,
        this->selectedCategories(this->exportChecks_));
    if (!bundle)
    {
        QMessageBox::warning(this, "Couldn't export settings", bundle.error());
        return;
    }
    const auto path = QFileDialog::getSaveFileName(
        this, "Export Moltorino settings", "moltorino-settings.json",
        "Moltorino settings (*.json)");
    if (path.isEmpty())
    {
        return;
    }
    QSaveFile file(path);
    const auto contents =
        QJsonDocument(bundle.value()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        QMessageBox::warning(this, "Couldn't export settings",
                             "Moltorino couldn't write that file.");
        return;
    }
    this->exportStatus_->setText("Settings exported.");
}

void BackupSharingPage::shareBundle()
{
    getApp()->getCommands()->save();
    getSettings()->requestSave();
    getApp()->getWindows()->save();
    auto bundle = settingsbackup::createBundle(
        getApp()->getPaths().settingsDirectory,
        this->selectedCategories(this->exportChecks_));
    if (!bundle)
    {
        QMessageBox::warning(this, "Couldn't create code", bundle.error());
        return;
    }

    const auto contents = QString::fromUtf8(
        QJsonDocument(bundle.value()).toJson(QJsonDocument::Compact));
    QJsonObject payload{
        {QStringLiteral("source"), QStringLiteral("settings")},
        {QStringLiteral("title"), QStringLiteral("Moltorino settings")},
        {QStringLiteral("language"), QStringLiteral("json")},
        {QStringLiteral("content"), contents},
    };
    const auto expiry = this->expiry_->currentData();
    payload.insert(QStringLiteral("expiresInDays"),
                   expiry.isValid() ? QJsonValue(expiry.toInt())
                                    : QJsonValue(QJsonValue::Null));

    this->shareButton_->setEnabled(false);
    this->shareRevokeKey_->setEnabled(false);
    this->shareCode_->clear();
    this->shareRevokeKey_->clear();
    this->deleteShareButton_->setEnabled(false);
    this->exportStatus_->setText("Creating code...");
    NetworkRequest(QUrl(QStringLiteral("https://h.moltorino.com/api/paste")),
                   NetworkRequestType::Post)
        .timeout(15000)
        .maximumResponseSize(1024 * 1024)
        .hideRequestBody()
        .json(payload)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto code = root.value(QStringLiteral("slug")).toString();
            if (code.isEmpty())
            {
                this->exportStatus_->setText(
                    "The server returned an invalid code.");
                return;
            }
            this->shareCode_->setText(code);
            auto revokeKey = root.value(QStringLiteral("revokeKey")).toString();
            if (revokeKey.isEmpty())
            {
                const auto token =
                    root.value(QStringLiteral("deleteToken")).toString();
                if (!token.isEmpty())
                {
                    revokeKey = code + u'.' + token;
                }
            }
            this->shareRevokeKey_->setText(revokeKey);
            this->exportStatus_->setText(
                root.value(QStringLiteral("expiresAt")).isNull()
                    ? QStringLiteral("Code created. It does not expire.")
                    : QStringLiteral("Code created. Save the revoke key if you "
                                     "may want to remove it."));
            crossPlatformCopy(code);
        })
        .onError([this](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto message = root.value(QStringLiteral("error")).toString();
            this->exportStatus_->setText(
                message.isEmpty()
                    ? QStringLiteral("Couldn't create a sharing code.")
                    : message);
        })
        .finally([this] {
            this->shareButton_->setEnabled(true);
            this->shareRevokeKey_->setEnabled(true);
        })
        .execute();
}

void BackupSharingPage::deleteSharedBundle()
{
    const auto revokeKey =
        settingsbackup::parseRevokeKey(this->shareRevokeKey_->text());
    if (!revokeKey)
    {
        this->exportStatus_->setText("Paste a valid revoke key first.");
        return;
    }
    if (QMessageBox::question(
            this, "Revoke sharing code",
            QStringLiteral("Revoke settings code %1? Anyone using it will "
                           "lose access.")
                .arg(revokeKey->code)) != QMessageBox::Yes)
    {
        return;
    }

    this->deleteShareButton_->setEnabled(false);
    this->shareRevokeKey_->setEnabled(false);
    this->shareButton_->setEnabled(false);
    this->exportStatus_->setText("Revoking code...");
    NetworkRequest(QUrl(QStringLiteral("https://h.moltorino.com/api/paste/%1")
                            .arg(revokeKey->code)),
                   NetworkRequestType::Delete)
        .timeout(15000)
        .maximumResponseSize(1024 * 1024)
        .hideRequestBody()
        .header("Authorization", QStringLiteral("Bearer ") + revokeKey->token)
        .caller(this)
        .onSuccess([this](const NetworkResult &) {
            this->shareCode_->clear();
            this->shareRevokeKey_->clear();
            this->exportStatus_->setText("Sharing code revoked.");
        })
        .onError([this](const NetworkResult &result) {
            const auto message =
                result.parseJson().value(QStringLiteral("error")).toString();
            this->exportStatus_->setText(
                message.isEmpty() ? QStringLiteral("Couldn't revoke that code.")
                                  : message);
        })
        .finally([this] {
            this->shareRevokeKey_->setEnabled(true);
            this->shareButton_->setEnabled(true);
            this->deleteShareButton_->setEnabled(
                settingsbackup::parseRevokeKey(this->shareRevokeKey_->text())
                    .has_value());
        })
        .execute();
}

void BackupSharingPage::browseImportFile()
{
    const auto path = QFileDialog::getOpenFileName(
        this, "Import Moltorino settings", {}, "Moltorino settings (*.json)");
    if (!path.isEmpty())
    {
        this->importSource_->setText(path);
        this->loadImportSource();
    }
}

void BackupSharingPage::loadImportSource()
{
    this->importCancellation_.request_stop();
    this->importCancellation_ = std::stop_source{};
    const auto generation = ++this->importLoadGeneration_;
    this->loadedBundle_ = {};
    this->applyNowButton_->setEnabled(false);
    this->importRestartButton_->setEnabled(false);
    for (auto *check : this->importChecks_)
    {
        check->setChecked(false);
        check->setEnabled(false);
    }
    const auto input = this->importSource_->text().trimmed();
    if (input.isEmpty())
    {
        this->importStatus_->setText("Enter a sharing code or choose a file.");
        return;
    }
    if (QFileInfo::exists(input))
    {
        QFile file(input);
        if (!file.open(QIODevice::ReadOnly))
        {
            this->importStatus_->setText("Couldn't read that file.");
            return;
        }
        if (file.size() > settingsbackup::MAX_BUNDLE_BYTES)
        {
            this->importStatus_->setText(
                "That settings file is too large to import.");
            return;
        }
        const auto contents = file.read(settingsbackup::MAX_BUNDLE_BYTES + 1);
        if (!file.atEnd() || file.error() != QFileDevice::NoError)
        {
            this->importStatus_->setText("Couldn't read that settings file.");
            return;
        }
        auto bundle = settingsbackup::parseBundle(contents);
        if (!bundle)
        {
            this->importStatus_->setText(bundle.error());
            return;
        }
        this->setLoadedBundle(bundle.value());
        return;
    }

    const auto code = normalizedShareCode(input);
    if (code.isEmpty())
    {
        this->importStatus_->setText(
            "That doesn't look like a sharing code or file.");
        return;
    }
    this->importStatus_->setText("Loading code...");
    NetworkRequest(
        QUrl(QStringLiteral("https://h.moltorino.com/%1/raw").arg(code)))
        .timeout(15000)
        .maximumResponseSize(settingsbackup::MAX_BUNDLE_BYTES)
        .cancelWith(this->importCancellation_.get_token())
        .caller(this)
        .onSuccess([this, generation](const NetworkResult &result) {
            if (generation != this->importLoadGeneration_)
            {
                return;
            }
            auto bundle = settingsbackup::parseBundle(result.getData());
            if (!bundle)
            {
                this->importStatus_->setText(bundle.error());
                return;
            }
            this->setLoadedBundle(bundle.value());
        })
        .onError([this, generation](const NetworkResult &) {
            if (generation != this->importLoadGeneration_)
            {
                return;
            }
            this->importStatus_->setText("Couldn't load that sharing code.");
        })
        .execute();
}

void BackupSharingPage::setLoadedBundle(const QJsonObject &bundle)
{
    this->loadedBundle_ = bundle;
    const auto present = contentCategories(bundle);
    for (auto it = this->importChecks_.begin(); it != this->importChecks_.end();
         ++it)
    {
        const bool included = present.contains(it.key());
        it.value()->setEnabled(included);
        it.value()->setChecked(included);
    }
    this->updateImportActions();
}

void BackupSharingPage::updateImportActions()
{
    const auto selected = this->selectedCategories(this->importChecks_);
    const bool hasSelection =
        !this->loadedBundle_.isEmpty() && !selected.isEmpty();
    const bool canApplyNow = hasSelection && settingsbackup::canApplyImportNow(
                                                 this->loadedBundle_, selected);

    this->applyNowButton_->setEnabled(canApplyNow);
    this->importRestartButton_->setEnabled(hasSelection);
    this->applyNowButton_->setToolTip(
        canApplyNow
            ? QStringLiteral("Apply these settings without restarting.")
            : QStringLiteral(
                  "Only compatible Appearance and UI settings can apply "
                  "without restarting."));

    if (this->loadedBundle_.isEmpty())
    {
        return;
    }
    if (!hasSelection)
    {
        this->importStatus_->setText("Choose what to import.");
    }
    else if (canApplyNow)
    {
        this->importStatus_->setText("Ready to apply without restarting.");
    }
    else
    {
        this->importStatus_->setText("This selection needs a restart.");
    }
}

void BackupSharingPage::applyLoadedBundleNow()
{
    getApp()->getCommands()->save();
    getSettings()->requestSave();
    getApp()->getWindows()->save();
    const auto selected = this->selectedCategories(this->importChecks_);
    auto result = settingsbackup::applyImportNow(
        getApp()->getPaths().settingsDirectory, this->loadedBundle_, selected,
        this->mergeLists_->isChecked());
    if (!result)
    {
        QMessageBox::warning(this, "Couldn't apply settings", result.error());
        this->updateImportActions();
        return;
    }

    getSettings()->requestSave();
    getSettings()->saveSnapshot();
    getApp()->getWindows()->forceLayoutChannelViews();
    this->importStatus_->setText("Settings applied.");
}

void BackupSharingPage::importLoadedBundleAndRestart()
{
    if (QMessageBox::question(
            this, "Import settings",
            "Import the selected settings and restart Moltorino now?\n\n"
            "Close any other Moltorino windows first so they cannot save over "
            "the imported settings.") != QMessageBox::Yes)
    {
        return;
    }

    getApp()->getCommands()->save();
    getSettings()->requestSave();
    getApp()->getWindows()->save();
    const auto selected = this->selectedCategories(this->importChecks_);
    auto result = settingsbackup::stageImport(
        getApp()->getPaths().settingsDirectory, this->loadedBundle_, selected,
        this->mergeLists_->isChecked());
    if (!result)
    {
        QMessageBox::warning(this, "Couldn't import settings", result.error());
        return;
    }
    restartAfterStaging(this, QStringLiteral("Your settings import"));
}

void BackupSharingPage::refreshRecoveryFiles()
{
    this->recoveryFiles_ = settingsbackup::inspectRecoveryFiles(
        getApp()->getPaths().settingsDirectory);
    for (int rowIndex = 0; rowIndex < this->recoveryRows_.size(); ++rowIndex)
    {
        auto &row = this->recoveryRows_[rowIndex];
        const QSignalBlocker blocker(row.backup);
        row.backup->clear();
        int backupNumber = 0;
        for (int fileIndex = 0; fileIndex < this->recoveryFiles_.size();
             ++fileIndex)
        {
            const auto &file = this->recoveryFiles_.at(fileIndex);
            const QFileInfo info(file.sourcePath);
            if (file.targetFile != row.targetFile ||
                info.fileName() == file.targetFile ||
                file.state == settingsbackup::RecoveryState::Damaged ||
                file.state == settingsbackup::RecoveryState::Unreadable)
            {
                continue;
            }

            ++backupNumber;
            const bool rescue =
                info.fileName().startsWith(file.targetFile + ".rescue-");
            const auto version =
                rescue ? QStringLiteral("Rescue copy")
                : backupNumber == 1
                    ? QStringLiteral("Latest backup")
                    : QStringLiteral("Older backup %1").arg(backupNumber);
            const auto label =
                QStringLiteral("%1  |  %2")
                    .arg(version, QLocale().toString(file.lastModified,
                                                     QLocale::ShortFormat));
            row.backup->addItem(label, fileIndex);
        }
        const bool available = row.backup->count() > 0;
        row.selected->setEnabled(available);
        if (!available)
        {
            row.selected->setChecked(false);
        }
        row.backup->setEnabled(available && row.selected->isChecked());
        this->updateRecoveryRow(rowIndex);
    }
    this->updateRestoreButton();
}

void BackupSharingPage::updateRecoveryRow(int rowIndex)
{
    auto &row = this->recoveryRows_[rowIndex];
    const auto selectedFileIndex = row.backup->currentData().toInt();
    if (row.backup->count() == 0 || selectedFileIndex < 0 ||
        selectedFileIndex >= this->recoveryFiles_.size())
    {
        row.details->setText("No usable backups found.");
        return;
    }

    const auto &backup = this->recoveryFiles_.at(selectedFileIndex);
    QString currentText = QStringLiteral("Current data unavailable");
    for (const auto &file : this->recoveryFiles_)
    {
        if (file.targetFile == row.targetFile &&
            QFileInfo(file.sourcePath).fileName() == file.targetFile)
        {
            currentText =
                QStringLiteral("Current: %1, %2, %3")
                    .arg(settingsbackup::recoveryStateText(file.state),
                         file.summary,
                         settingsbackup::formatFileSize(file.sizeBytes));
            break;
        }
    }
    row.details->setText(
        QStringLiteral("%1\nBackup: %2, %3, %4")
            .arg(currentText, settingsbackup::recoveryStateText(backup.state),
                 backup.summary,
                 settingsbackup::formatFileSize(backup.sizeBytes)));
}

void BackupSharingPage::updateRestoreButton()
{
    const bool selected =
        std::ranges::any_of(this->recoveryRows_, [](const auto &row) {
            return row.selected->isEnabled() && row.selected->isChecked() &&
                   row.backup->count() > 0;
        });
    this->restoreButton_->setEnabled(selected);
}

void BackupSharingPage::restoreSelectedFile()
{
    QVector<settingsbackup::RecoveryFile> selectedFiles;
    QStringList selectedNames;
    for (const auto &row : this->recoveryRows_)
    {
        if (!row.selected->isEnabled() || !row.selected->isChecked() ||
            row.backup->count() == 0)
        {
            continue;
        }
        const auto index = row.backup->currentData().toInt();
        if (index >= 0 && index < this->recoveryFiles_.size())
        {
            selectedFiles.append(this->recoveryFiles_.at(index));
            selectedNames.append(row.name);
        }
    }
    if (selectedFiles.isEmpty())
    {
        return;
    }

    if (QMessageBox::question(
            this, "Restore backup",
            QStringLiteral("Restore %1 and restart Moltorino now? Moltorino "
                           "will keep rescue copies of your current data.\n\n"
                           "Close any other Moltorino windows first so they "
                           "cannot save over the restored data.")
                .arg(selectedNames.join(", "))) != QMessageBox::Yes)
    {
        return;
    }
    auto staged = settingsbackup::stageRecoveryFiles(
        getApp()->getPaths().settingsDirectory, selectedFiles);
    if (!staged)
    {
        QMessageBox::warning(this, "Couldn't restore backup", staged.error());
        return;
    }
    restartAfterStaging(this, QStringLiteral("Your backup restore"));
}

void BackupSharingPage::onShow()
{
    this->refreshRecoveryFiles();
}

}
