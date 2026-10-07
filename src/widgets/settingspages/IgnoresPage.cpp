// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/IgnoresPage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/ignores/HiddenUser.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "controllers/ignores/IgnoreModel.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

using namespace literals;

static void addPhrasesTab(LayoutCreator<QVBoxLayout> box);
static void addUsersTab(IgnoresPage &page, LayoutCreator<QVBoxLayout> box,
                        QStringListModel &blockedModel,
                        QStringListModel &hiddenModel, QListView *&hiddenList,
                        std::vector<HiddenUser> &hiddenUsers,
                        pajlada::Signals::SignalHolder &connections);

IgnoresPage::IgnoresPage()
{
    LayoutCreator<IgnoresPage> layoutCreator(this);
    auto layout = layoutCreator.setLayoutType<QVBoxLayout>();
    auto tabs = layout.emplace<QTabWidget>();

    addPhrasesTab(tabs.appendTab(new QVBoxLayout, "Messages"));
    addUsersTab(*this, tabs.appendTab(new QVBoxLayout, "Users"),
                this->userListModel_, this->hiddenUserListModel_,
                this->hiddenUserList_, this->hiddenUsers_,
                this->managedConnections_);
    this->managedConnections_.managedConnect(
        getSettings()->hiddenUsers.delayedItemsChanged, [this] {
            this->onShow();
        });
    this->onShow();
}

void addPhrasesTab(LayoutCreator<QVBoxLayout> layout)
{
    layout.emplace<QLabel>("Ignore messages based certain patterns.");
    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                (new IgnoreModel(nullptr))
                    ->initialized(&getSettings()->ignoredMessages))
            .getElement();
    view->setTitles(
        {"Pattern", "Regex", "Case-sensitive", "Block", "Replacement"});
    view->getTableView()->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Fixed);
    view->getTableView()->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    view->addRegexHelpLink();

    QTimer::singleShot(1, view, [view] {
        view->getTableView()->resizeColumnsToContents();
        view->getTableView()->setColumnWidth(0, 200);
    });

    std::ignore = view->addButtonPressed.connect([] {
        getSettings()->ignoredMessages.append(IgnorePhrase{
            "my pattern",
            false,
            false,
            DEFAULT_IGNORE_PHRASE_REPLACE.toString(),
            true,
        });
    });
}

void addUsersTab(IgnoresPage &page, LayoutCreator<QVBoxLayout> users,
                 QStringListModel &blockedModel, QStringListModel &hiddenModel,
                 QListView *&hiddenList, std::vector<HiddenUser> &hiddenUsers,
                 pajlada::Signals::SignalHolder &connections)
{
    auto label = users.emplace<QLabel>(
        u"/block <user> in chat blocks a user.\n/unblock <user> in chat unblocks a user.\nYou can also click on a user to open the usercard."_s);
    label->setWordWrap(true);
    users.append(page.createCheckBox("Enable Twitch blocked users",
                                     getSettings()->enableTwitchBlockedUsers));

    auto anyways = users.emplace<QHBoxLayout>().withoutMargin();
    {
        anyways.emplace<QLabel>("Show messages from blocked users:");

        auto *combo = anyways.emplace<QComboBox>().getElement();
        combo->addItems(
            {"Never", "If you are Moderator", "If you are Broadcaster"});

        auto &setting = getSettings()->showBlockedUsersMessages;

        setting.connect(
            [combo](const int value) {
                combo->setCurrentIndex(value);
            },
            connections);

        QObject::connect(combo,
                         QOverload<int>::of(&QComboBox::currentIndexChanged),
                         [&setting](int index) {
                             if (index != -1)
                             {
                                 setting = index;
                             }
                         });

        anyways->addStretch(1);
    }


    users.emplace<QLabel>("Blocked on Twitch");
    users.emplace<QListView>()->setModel(&blockedModel);

    auto hiddenDescription = users.emplace<QLabel>(
        "Hidden users disappear from chat, including messages that tag or "
        "reply to them. This only affects Moltorino.");
    hiddenDescription->setWordWrap(true);
    users.emplace<QLabel>("Hidden in Moltorino");
    hiddenList = users.emplace<QListView>().getElement();
    hiddenList->setModel(&hiddenModel);
    hiddenList->setSelectionMode(QAbstractItemView::SingleSelection);

    auto hiddenActions = users.emplace<QHBoxLayout>().withoutMargin();
    auto *unhide =
        hiddenActions.emplace<QPushButton>("Unhide selected").getElement();
    unhide->setEnabled(false);
    hiddenActions->addStretch(1);
    QObject::connect(
        hiddenList->selectionModel(), &QItemSelectionModel::selectionChanged,
        unhide, [hiddenList, unhide] {
            unhide->setEnabled(hiddenList->currentIndex().isValid());
        });
    QObject::connect(
        unhide, &QPushButton::clicked, &page, [hiddenList, &hiddenUsers] {
            if (hiddenList == nullptr)
            {
                return;
            }
            const auto row = hiddenList->currentIndex().row();
            if (row < 0 || row >= static_cast<int>(hiddenUsers.size()))
            {
                return;
            }
            const auto target = hiddenUsers.at(static_cast<std::size_t>(row));
            if (auto *controller = getApp()->getHiddenUsers())
            {
                controller->setHidden(target.platform(), target.userID(),
                                      target.login(), target.displayName(),
                                      false);
            }
        });
}

void IgnoresPage::onShow()
{
    auto *app = getApp();

    auto user = app->getAccounts()->twitch.getCurrent();

    if (user->isAnon())
    {
        this->userListModel_.setStringList({});
    }
    else
    {
        QStringList users;
        users.reserve(user->blocks().size());
        for (const auto &blockedUser : user->blocks())
        {
            users << blockedUser.name;
        }
        users.sort(Qt::CaseInsensitive);
        this->userListModel_.setStringList(users);
    }

    this->hiddenUsers_ = *getSettings()->hiddenUsers.readOnly();
    std::ranges::sort(this->hiddenUsers_, [](const HiddenUser &left,
                                             const HiddenUser &right) {
        return left.displayLabel().compare(right.displayLabel(),
                                           Qt::CaseInsensitive) < 0;
    });
    QStringList hiddenLabels;
    hiddenLabels.reserve(static_cast<qsizetype>(this->hiddenUsers_.size()));
    for (const auto &hidden : this->hiddenUsers_)
    {
        hiddenLabels.push_back(hidden.displayLabel());
    }
    this->hiddenUserListModel_.setStringList(hiddenLabels);
}

}
