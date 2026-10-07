// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    auto *cleanupWarning = layout.emplace<QLabel>().getElement();
    cleanupWarning->setWordWrap(true);
    cleanupWarning->setTextFormat(Qt::RichText);
    cleanupWarning->setOpenExternalLinks(true);
    cleanupWarning->setTextInteractionFlags(Qt::TextBrowserInteraction);
    cleanupWarning->hide();

    auto *dismissCleanupWarning =
        layout.emplace<QPushButton>("I handled this warning").getElement();
    dismissCleanupWarning->setToolTip(
        "Dismiss the warning after resolving the Google account connection. "
        "Pending automatic cleanup will still retry later.");
    dismissCleanupWarning->hide();

    auto refreshCleanupWarning = [app, cleanupWarning, dismissCleanupWarning] {
        const auto errors =
            app->getAccounts()->youtube.credentialCleanupErrors();
        if (errors.empty())
        {
            cleanupWarning->clear();
            cleanupWarning->hide();
            dismissCleanupWarning->hide();
            return;
        }

        QStringList details;
        details.reserve(static_cast<qsizetype>(errors.size()));
        for (const auto &[channelID, error] : errors)
        {
            details.emplace_back(QStringLiteral("%1: %2").arg(
                channelID.toHtmlEscaped(), error.toHtmlEscaped()));
        }
        cleanupWarning->setText(
            QStringLiteral(
                "<b>YouTube credential cleanup needs attention.</b> "
                "The account is disconnected, but Moltorino could not finish "
                "revoking or removing its Google credential. Any token kept "
                "for a retry remains saved locally. Follow the detail below, or "
                "remove Moltorino from "
                "<a href=\"https://myaccount.google.com/connections\">Google "
                "Account connections</a>."
                "<br>%1")
                .arg(details.join(QStringLiteral("<br>"))));
        cleanupWarning->show();
        dismissCleanupWarning->show();
    };
    refreshCleanupWarning();

    QObject::connect(dismissCleanupWarning, &QPushButton::clicked, this, [app] {
        app->getAccounts()->youtube.dismissCredentialCleanupErrors();
    });

    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                app->getAccounts()->createModel(nullptr), false)
            .getElement();

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    this->managedConnections_.managedConnect(
        app->getAccounts()->youtube.credentialCleanupChanged,
        refreshCleanupWarning);

    view->getTableView()->setStyleSheet("background: #333");

    auto *youtubeDisconnectHelp = layout.emplace<QLabel>().getElement();
    youtubeDisconnectHelp->setWordWrap(true);
    youtubeDisconnectHelp->setOpenExternalLinks(true);
    youtubeDisconnectHelp->setTextInteractionFlags(Qt::TextBrowserInteraction);
    youtubeDisconnectHelp->setStyleSheet("color: palette(mid);");
    youtubeDisconnectHelp->setText(QStringLiteral(
        "Removing a YouTube account disconnects it and asks Google to "
        "revoke Moltorino's access. You can also revoke it from your "
        "<a href=\"https://myaccount.google.com/connections\">Google "
        "Account</a>."));

    auto *tiktokWarning = layout.emplace<QLabel>().getElement();
    tiktokWarning->setWordWrap(true);
    tiktokWarning->setTextFormat(Qt::PlainText);
    auto refreshTikTokWarning = [app, tiktokWarning] {
        const auto &error = app->getAccounts()->tiktok.credentialError();
        tiktokWarning->setText(error);
        tiktokWarning->setVisible(!error.isEmpty());
    };
    refreshTikTokWarning();
    this->managedConnections_.managedConnect(
        app->getAccounts()->tiktok.credentialErrorChanged,
        refreshTikTokWarning);
}

}
