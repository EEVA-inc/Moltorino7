#include "widgets/helper/TikTokAccountSwitchWidget.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/tiktok/TikTokAccount.hpp"
#include "providers/tiktok/TikTokAccountManager.hpp"

#include <QListWidgetItem>
#include <QSignalBlocker>

namespace chatterino {

TikTokAccountSwitchWidget::TikTokAccountSwitchWidget(QWidget *parent)
    : QListWidget(parent)
{
    auto *manager = &getApp()->getAccounts()->tiktok;
    this->managedConnections_.managedConnect(manager->userListUpdated, [this] {
        this->refreshItems();
        this->refresh();
    });
    this->managedConnections_.managedConnect(manager->currentChanged, [this] {
        this->refresh();
    });
    this->managedConnections_.managedConnect(manager->credentialsChanged,
                                             [this] {
                                                 this->refreshItems();
                                                 this->refresh();
                                             });

    this->refreshItems();
    this->refresh();

    QObject::connect(this, &QListWidget::clicked, this, [this](const auto &) {
        auto *item = this->currentItem();
        if (!item)
        {
            return;
        }
        getApp()->getAccounts()->tiktok.selectAccount(
            item->data(Qt::UserRole).toString());
    });
}

void TikTokAccountSwitchWidget::refresh()
{
    QSignalBlocker blocker(this);
    const auto userID = getApp()->getAccounts()->tiktok.current()->userID();
    for (int i = 0; i < this->count(); ++i)
    {
        if (this->item(i)->data(Qt::UserRole).toString() == userID)
        {
            this->setCurrentRow(i);
            return;
        }
    }
    if (this->count() > 0)
    {
        this->setCurrentRow(0);
    }
}

void TikTokAccountSwitchWidget::refreshItems()
{
    QSignalBlocker blocker(this);
    this->clear();

    auto *anonymous = new QListWidgetItem(ANONYMOUS_USERNAME_LABEL, this);
    anonymous->setData(Qt::UserRole, QString{});

    for (const auto &account : getApp()->getAccounts()->tiktok.accounts)
    {
        auto label = account->displayName();
        if (account->isLoadingCredentials())
        {
            label += QStringLiteral(" (loading...)");
        }
        else if (!account->hasCredentials())
        {
            label += QStringLiteral(" (log in again)");
        }
        auto *item = new QListWidgetItem(label, this);
        item->setData(Qt::UserRole, account->userID());
        item->setToolTip(u'@' + account->handle());
    }
}

}
