// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/ModerationPage.hpp"

#include "Application.hpp"
#include "controllers/logging/ChannelLoggingModel.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "controllers/moderationactions/ModerationActionModel.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/LoadPixmap.hpp"
#include "util/PostToThread.hpp"
#include "widgets/helper/EditableModelView.hpp"
#include "widgets/helper/IconDelegate.hpp"
#include "widgets/settingspages/SettingWidget.hpp"
#include "widgets/settingspages/UsercardModerationSettings.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPushButton>
#include <QTableView>
#include <QtConcurrent/QtConcurrent>

namespace chatterino {

qint64 dirSize(QString &dirPath)
{
    QDirIterator it(dirPath, QDirIterator::Subdirectories);
    qint64 size = 0;

    while (it.hasNext())
    {
        size += it.fileInfo().size();
        it.next();
    }

    return size;
}

QString formatSize(qint64 size)
{
    QStringList units = {"Bytes", "KB", "MB", "GB", "TB", "PB"};
    int i;
    double outputSize = size;
    for (i = 0; i < units.size() - 1; i++)
    {
        if (outputSize < 1024)
        {
            break;
        }
        outputSize = outputSize / 1024;
    }
    return QString("%0 %1").arg(outputSize, 0, 'f', 2).arg(units[i]);
}

QString fetchLogDirectorySize()
{
    QString logsDirectoryPath = getSettings()->logPath.getValue().isEmpty()
                                    ? getApp()->getPaths().messageLogDirectory
                                    : getSettings()->logPath;

    auto logsSize = dirSize(logsDirectoryPath);

    return QString("Your logs currently take up %1 of space")
        .arg(formatSize(logsSize));
}

ModerationPage::ModerationPage()
{
    LayoutCreator<ModerationPage> layoutCreator(this);

    auto tabs = layoutCreator.emplace<QTabWidget>();
    this->tabWidget_ = tabs.getElement();

    auto logs = tabs.appendTab(new QVBoxLayout, "Logs");
    {
        QCheckBox *enableLogging = this->createCheckBox(
            "Enable logging", getSettings()->enableLogging);
        logs.append(enableLogging);

        auto logsPathLabel = logs.emplace<QLabel>();

        QString logExplanation =
            "<span style=\"color:#bbb\"> They are saved as plain "
            "text files per channel, containing the messages with "
            "timestamps.</span>";

        getSettings()->logPath.connect(
            [logsPathLabel, logExplanation](const QString &logPath,
                                            auto) mutable {
                QString pathOriginal =
                    logPath.isEmpty() ? getApp()->getPaths().messageLogDirectory
                                      : logPath;

                QString pathShortened =
                    "Logs are saved at <a href=\"file:///" + pathOriginal +
                    R"("><span style="color: white;">)" +
                    shortenString(pathOriginal, 50) + ".</span></a>";

                logsPathLabel->setText(pathShortened + logExplanation);
                logsPathLabel->setToolTip(pathOriginal);
                logsPathLabel->setWordWrap(true);
            });

        logsPathLabel->setTextFormat(Qt::RichText);
        logsPathLabel->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                               Qt::LinksAccessibleByKeyboard);
        logsPathLabel->setOpenExternalLinks(true);

        auto buttons = logs.emplace<QHBoxLayout>().withoutMargin();

        auto selectDir = buttons.emplace<QPushButton>("Select log directory ");
        auto resetDir = buttons.emplace<QPushButton>("Reset");

        getSettings()->logPath.connect(
            [element = resetDir.getElement()](const QString &path) {
                element->setEnabled(!path.isEmpty());
            });

        buttons->addStretch();

        auto logsPathSizeLabel = logs.emplace<QLabel>();
        logsPathSizeLabel->setText(QtConcurrent::run([] {
                                       return fetchLogDirectorySize();
                                   }).result());

        QObject::connect(
            selectDir.getElement(), &QPushButton::clicked, this,
            [this, logsPathSizeLabel]() mutable {
                auto dirName = QFileDialog::getExistingDirectory(this);

                getSettings()->logPath = dirName;

                logsPathSizeLabel->setText(QtConcurrent::run([] {
                                               return fetchLogDirectorySize();
                                           }).result());
            });

        buttons->addSpacing(16);

        QObject::connect(
            resetDir.getElement(), &QPushButton::clicked, this,
            [logsPathSizeLabel]() mutable {
                getSettings()->logPath = "";

                logsPathSizeLabel->setText(QtConcurrent::run([] {
                                               return fetchLogDirectorySize();
                                           }).result());
            });

        auto logsTimestampFormatLayout =
            logs.emplace<QHBoxLayout>().withoutMargin();
        auto logsTimestampFormatLabel =
            logsTimestampFormatLayout.emplace<QLabel>();
        logsTimestampFormatLabel->setText(
            QString("Log file timestamp format: "));

        QComboBox *logTimestampFormat = this->createComboBox(
            {"Disable", "h:mm", "hh:mm", "h:mm a", "hh:mm a", "h:mm:ss",
             "hh:mm:ss", "h:mm:ss a", "hh:mm:ss a", "h:mm:ss.zzz",
             "h:mm:ss.zzz a", "hh:mm:ss.zzz", "hh:mm:ss.zzz a"},
            getSettings()->logTimestampFormat);
        logTimestampFormat->setToolTip("a = am/pm, zzz = milliseconds");
        logsTimestampFormatLayout.append(logTimestampFormat);

        SettingWidget::checkbox("Use Twitch's timestamps",
                                getSettings()->tryUseTwitchTimestamps)
            ->setTooltip(
                "Try to use Twitch's timestamp (the time when the message was "
                "received by Twitch's chat server), rather than your "
                "computer's local timestamp.\nNote that using this setting can "
                "result in out-of-order timestamps in the log files, and that "
                "if Twitch's timestamp was unavailable for a message, it will "
                "fall back to your computer's local timestamp.")
            ->conditionallyEnabledBy(getSettings()->enableLogging)
            ->addToLayout(logs->layout());

        QCheckBox *onlyLogListedChannels =
            this->createCheckBox("Only log channels listed below",
                                 getSettings()->onlyLogListedChannels);

        onlyLogListedChannels->setEnabled(getSettings()->enableLogging);
        logs.append(onlyLogListedChannels);

        auto *separatelyStoreStreamLogs =
            this->createCheckBox("Store live stream logs as separate files",
                                 getSettings()->separatelyStoreStreamLogs);

        separatelyStoreStreamLogs->setEnabled(getSettings()->enableLogging);
        logs.append(separatelyStoreStreamLogs);

        QObject::connect(
            enableLogging, &QCheckBox::stateChanged, this,
            [enableLogging, onlyLogListedChannels,
             separatelyStoreStreamLogs]() mutable {
                onlyLogListedChannels->setEnabled(enableLogging->isChecked());
                separatelyStoreStreamLogs->setEnabled(
                    getSettings()->enableLogging);
            });

        EditableModelView *view =
            logs.emplace<EditableModelView>(
                    (new ChannelLoggingModel(nullptr))
                        ->initialized(&getSettings()->loggedChannels))
                .getElement();

        view->setTitles({"Twitch channels"});
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            QHeaderView::Fixed);
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);

        std::ignore = view->addButtonPressed.connect([] {
            getSettings()->loggedChannels.append(ChannelLog("channel"));
        });

    }

    auto modMode = tabs.appendTab(new QVBoxLayout, "Moderation buttons");
    {

        auto label = modMode.emplace<QLabel>(
            "Moderation mode is enabled by clicking <img width='18' height='18' src=':/buttons/moderationDisabledDarkMode18x18.png'> in a channel that you moderate.<br><br>"
            "Moderation buttons can be bound to chat commands such as \"/ban {user.name}\", \"/timeout {user.name} 1000\", \"/w someusername !report {user.name} was bad in channel {channel.name}\" or any other custom text commands.<br>"
            "For deleting messages use /delete {msg.id}.<br><br>"
            "More information can be found <a href='https://wiki.chatterino.com/Moderation/#moderation-mode'>here</a>.");
        label->setOpenExternalLinks(true);
        label->setWordWrap(true);
        label->setStyleSheet("color: #bbb");

        SettingWidget::checkbox(
            "Show buttons in channels I do not moderate",
            getSettings()->showModerationButtonsWithoutPermission)
            ->setTooltip(
                "When off, inline moderation buttons only appear where your "
                "current account can moderate.")
            ->addToLayout(modMode->layout());

        EditableModelView *view =
            modMode
                .emplace<EditableModelView>(
                    (new ModerationActionModel(nullptr))
                        ->initialized(&getSettings()->moderationActions))
                .getElement();

        view->setTitles({"Action", "Icon"});
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            QHeaderView::Fixed);
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);
        view->getTableView()->setItemDelegateForColumn(
            ModerationActionModel::Column::Icon, new IconDelegate(view));
        QObject::connect(
            view->getTableView(), &QTableView::clicked,
            [this, view](const QModelIndex &clicked) {
                if (clicked.column() == ModerationActionModel::Column::Icon)
                {
                    const QPointer<EditableModelView> guardedView(view);
                    const QPersistentModelIndex target(clicked);
                    auto fileUrl = QFileDialog::getOpenFileUrl(
                        this, "Open Image", QUrl(),
                        "Image Files (*.png *.jpg *.jpeg)");
                    if (!guardedView || !target.isValid())
                    {
                        return;
                    }
                    view->getModel()->setData(target, fileUrl, Qt::UserRole);
                    view->getModel()->setData(target, fileUrl.fileName(),
                                              Qt::DisplayRole);

                    if (fileUrl.isEmpty())
                    {
                        view->getModel()->setData(target, QVariant(),
                                                  Qt::DecorationRole);
                    }
                    else
                    {
                        loadPixmapFromUrl(
                            {fileUrl.toString()},
                            [target, view = guardedView](const QPixmap &pixmap) {
                                postToThread([target, view, pixmap]() {
                                    if (view.isNull() || !target.isValid())
                                    {
                                        return;
                                    }

                                    view->getModel()->setData(
                                        target, pixmap, Qt::DecorationRole);
                                });
                            });
                    }
                }
            });

        std::ignore = view->addButtonPressed.connect([] {
            getSettings()->moderationActions.append(
                ModerationAction("/timeout {user.name} 300"));
        });

        auto *addPin = new QPushButton("Add Pin");
        view->addCustomButton(addPin);
        QObject::connect(addPin, &QPushButton::clicked, [] {
            getSettings()->moderationActions.append(
                ModerationAction("/pin {msg.id}"));
        });
    }

    this->addModerationButtonSettings(tabs.getElement());

    this->itemsChangedTimer_.setSingleShot(true);
}

void ModerationPage::addModerationButtonSettings(QTabWidget *tabs)
{
    auto page =
        LayoutCreator{tabs}.appendTab(new QVBoxLayout, "Usercard actions");
    page.emplace<UsercardModerationSettings>();
}

void ModerationPage::selectModerationActions()
{
    this->tabWidget_->setCurrentIndex(1);
}

}
