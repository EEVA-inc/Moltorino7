// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UpdateDialog.hpp"

#include "providers/moltorino/MoltorinoUpdater.hpp"
#include "widgets/Label.hpp"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBrowser>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

namespace chatterino {

namespace {

class ReleaseSummaryBrowser final : public QTextBrowser
{
public:
    explicit ReleaseSummaryBrowser(QWidget *parent)
        : QTextBrowser(parent)
    {
        this->setFrameShape(QFrame::NoFrame);
        this->setOpenExternalLinks(false);
        this->setOpenLinks(false);
        this->setMaximumHeight(150);
        QObject::connect(this, &QTextBrowser::anchorClicked, this,
                         [](const QUrl &url) {
                             const auto scheme = url.scheme().toLower();
                             if (url.isValid() &&
                                 (scheme == "https" || scheme == "http"))
                             {
                                 QDesktopServices::openUrl(url);
                             }
                         });
    }

    void setReleaseMarkdown(QString markdown)
    {
        markdown.remove(QRegularExpression(R"(<[^>]*>)"));
        this->document()->setMarkdown(markdown.trimmed());
    }

protected:
    QVariant loadResource(int type, const QUrl &name) override
    {
        if (type == QTextDocument::ImageResource)
        {
            return {};
        }
        return QTextBrowser::loadResource(type, name);
    }
};

}

UpdateDialog::UpdateDialog()
    : BaseWindow({BaseWindow::Frameless, BaseWindow::TopMost,
                  BaseWindow::EnableCustomFrame, BaseWindow::Dialog,
                  BaseWindow::DisableLayoutSave})
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle("Moltorino Update");

    auto *layout = new QVBoxLayout();
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(9);
    this->getLayoutContainer()->setLayout(layout);

    this->ui_.heading =
        new Label(this, "Moltorino Update", FontStyle::UiMediumBold);
    this->ui_.heading->setWordWrap(true);
    layout->addWidget(this->ui_.heading);

    this->ui_.details = new Label(this);
    this->ui_.details->setWordWrap(true);
    layout->addWidget(this->ui_.details);

    this->ui_.status = new Label(this);
    this->ui_.status->setWordWrap(true);
    layout->addWidget(this->ui_.status);

    this->ui_.summary = new ReleaseSummaryBrowser(this);
    layout->addWidget(this->ui_.summary);

    this->ui_.progress = new QProgressBar(this);
    this->ui_.progress->setRange(0, 100);
    this->ui_.progress->setTextVisible(true);
    layout->addWidget(this->ui_.progress);

    auto *buttons = new QDialogButtonBox(this);
    this->ui_.changelogButton = buttons->addButton(
        "View changelog", QDialogButtonBox::ActionRole);
    this->ui_.retryButton =
        buttons->addButton("Retry", QDialogButtonBox::ActionRole);
    this->ui_.restartButton =
        buttons->addButton("Restart and update", QDialogButtonBox::AcceptRole);
    auto *closeButton =
        buttons->addButton("Close", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    QObject::connect(this->ui_.changelogButton, &QPushButton::clicked, this,
                     [] {
                         getMoltorinoUpdater()->showFullChangelog();
                     });
    QObject::connect(this->ui_.retryButton, &QPushButton::clicked, this, [] {
        getMoltorinoUpdater()->retry();
    });
    QObject::connect(this->ui_.restartButton, &QPushButton::clicked, this,
                     [] {
                         getMoltorinoUpdater()->restartToUpdate();
                     });
    QObject::connect(closeButton, &QPushButton::clicked, this,
                     &QWidget::close);

    this->connections_.managedConnect(
        getMoltorinoUpdater()->stateChanged, [this] {
            this->refresh();
        });

    this->setScaleIndependentWidth(440);
    this->refresh();
}

void UpdateDialog::refresh()
{
    const auto *updater = getMoltorinoUpdater();
    const auto status = updater->status();

    QString heading;
    switch (status)
    {
        case MoltorinoUpdateStatus::Disabled:
            heading = "Updates unavailable";
            break;
        case MoltorinoUpdateStatus::Idle:
            heading = "Moltorino updates";
            break;
        case MoltorinoUpdateStatus::Checking:
            heading = "Checking for updates";
            break;
        case MoltorinoUpdateStatus::Downloading:
            heading = updater->isRollback() ? "Downloading the rollback"
                                            : "Downloading Moltorino update";
            break;
        case MoltorinoUpdateStatus::Ready:
            heading = updater->isRollback() ? "Rollback ready"
                                            : "Update ready";
            break;
        case MoltorinoUpdateStatus::Applying:
            heading = updater->isRollback() ? "Finishing the rollback"
                                            : "Finishing the update";
            break;
        case MoltorinoUpdateStatus::UpToDate:
            heading = "You're up to date";
            break;
        case MoltorinoUpdateStatus::Error:
            heading = "The update needs attention";
            break;
    }
    this->ui_.heading->setText(heading);

    QStringList details;
    if (!updater->targetBuild().isEmpty())
    {
        details.append(updater->targetBuild());
    }
    if (!updater->targetPackageVersion().isEmpty() &&
        updater->targetPackageVersion() != updater->targetBuild())
    {
        details.append(
            QString("Version %1").arg(updater->targetPackageVersion()));
    }
    if (!updater->channel().isEmpty())
    {
        details.append(updater->channel() == "internal" ? "Internal channel"
                                                         : "Stable channel");
    }
    if (updater->isRollback())
    {
        details.append("Rollback");
    }
    this->ui_.details->setText(details.join("  ·  "));
    this->ui_.details->setVisible(!details.isEmpty());

    this->ui_.status->setText(updater->statusText());

    const auto summary = updater->summaryMarkdown().trimmed();
    static_cast<ReleaseSummaryBrowser *>(this->ui_.summary)
        ->setReleaseMarkdown(summary);
    this->ui_.summary->setVisible(!summary.isEmpty());

    const bool downloading =
        status == MoltorinoUpdateStatus::Downloading;
    this->ui_.progress->setValue(updater->progress());
    this->ui_.progress->setFormat(QString::number(updater->progress()) + "%");
    this->ui_.progress->setVisible(downloading);

    const bool ready = updater->canRestartToUpdate();
    this->ui_.restartButton->setText(updater->isRollback()
                                         ? "Restart and roll back"
                                         : "Restart and update");
    this->ui_.restartButton->setVisible(ready);
    this->ui_.restartButton->setDefault(ready);
    this->ui_.retryButton->setVisible(status == MoltorinoUpdateStatus::Error);
    this->ui_.changelogButton->setVisible(
        !updater->targetBuild().trimmed().isEmpty());
    if (this->getLayoutContainer()->layout() != nullptr)
    {
        this->getLayoutContainer()->layout()->activate();
    }
    this->adjustSize();
}

}
