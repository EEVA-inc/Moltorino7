#include "widgets/helper/YouTubeAccountSwitchWidget.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeAccountManager.hpp"

#include <QListWidgetItem>
#include <QSignalBlocker>

namespace chatterino {

YouTubeAccountSwitchWidget::YouTubeAccountSwitchWidget(QWidget *parent)
    : QListWidget(parent)
{
    auto *manager = &getApp()->getAccounts()->youtube;
    this->managedConnections_.managedConnect(manager->userListUpdated, [this] {
        this->refreshItems();
        this->refresh();
    });
    this->managedConnections_.managedConnect(manager->currentChanged, [this] {
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
        getApp()->getAccounts()->youtube.selectAccount(
            item->data(Qt::UserRole).toString());
    });
}

void YouTubeAccountSwitchWidget::refresh()
{
    QSignalBlocker blocker(this);
    const auto channelID =
        getApp()->getAccounts()->youtube.current()->channelID();
    for (int i = 0; i < this->count(); ++i)
    {
        if (this->item(i)->data(Qt::UserRole).toString() == channelID)
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

void YouTubeAccountSwitchWidget::refreshItems()
{
    QSignalBlocker blocker(this);
    this->clear();

    auto *anonymous = new QListWidgetItem(ANONYMOUS_USERNAME_LABEL, this);
    anonymous->setData(Qt::UserRole, QString{});

    for (const auto &account : getApp()->getAccounts()->youtube.accountList())
    {
        auto *item = new QListWidgetItem(account->displayName(), this);
        item->setData(Qt::UserRole, account->channelID());
        item->setToolTip(account->channelID());
    }
}

}
