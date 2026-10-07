// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SettingsDialog.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/helper/SettingsDialogTab.hpp"
#include "widgets/settingspages/AboutPage.hpp"
#include "widgets/settingspages/AccountsPage.hpp"
#include "widgets/settingspages/BackupSharingPage.hpp"
#include "widgets/settingspages/CommandPage.hpp"
#include "widgets/settingspages/CustomizationPage.hpp"
#include "widgets/settingspages/ExternalToolsPage.hpp"
#include "widgets/settingspages/FiltersPage.hpp"
#include "widgets/settingspages/GeneralPage.hpp"
#include "widgets/settingspages/HighlightingPage.hpp"
#include "widgets/settingspages/IgnoresPage.hpp"
#include "widgets/settingspages/KeyboardSettingsPage.hpp"
#include "widgets/settingspages/ModerationPage.hpp"
#include "widgets/settingspages/MoltorinoPage.hpp"
#include "widgets/settingspages/NicknamesPage.hpp"
#include "widgets/settingspages/NotificationPage.hpp"
#include "widgets/settingspages/PluginsPage.hpp"
#include "widgets/Window.hpp"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPointer>

namespace chatterino {

SettingsDialog::SettingsDialog(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::DisableCustomScaling,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
              BaseWindow::UseSettingsStylesheet,
          },
          parent)
{
    this->setObjectName("SettingsDialog");
    this->setWindowTitle("Moltorino Settings");
    // Disable the ? button in the titlebar until we decide to use it
    this->setWindowFlags(this->windowFlags() &
                         ~Qt::WindowContextHelpButtonHint);

    this->resize(915, 600);
    this->themeChangedEvent();

    this->initUi();
    this->addTabs();

    this->addShortcuts();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });
}

void SettingsDialog::addShortcuts()
{
    this->setSearchPlaceholderText();
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](std::vector<QString>) -> QString {
             this->ui_.search->setFocus();
             this->ui_.search->selectAll();
             return "";
         }},
        {"delete", nullptr},
        {"accept", nullptr},
        {"reject", nullptr},
        {"scrollPage", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}
void SettingsDialog::setSearchPlaceholderText()
{
    QString searchHotkey;
    auto searchSeq = getApp()->getHotkeys()->getDisplaySequence(
        HotkeyCategory::PopupWindow, "search");
    if (!searchSeq.isEmpty())
    {
        searchHotkey =
            "(" + searchSeq.toString(QKeySequence::SequenceFormat::NativeText) +
            ")";
    }
    this->ui_.search->setPlaceholderText("Find in settings... " + searchHotkey);
}

void SettingsDialog::initUi()
{
    auto outerBox = LayoutCreator<QWidget>(this->getLayoutContainer())
                        .setLayoutType<QVBoxLayout>()
                        .withoutSpacing();

    // TOP
    auto title = outerBox.emplace<PageHeader>();
    auto edit = LayoutCreator<PageHeader>(title.getElement())
                    .setLayoutType<QHBoxLayout>()
                    .withoutMargin()
                    .emplace<QLineEdit>()
                    .assign(&this->ui_.search);
    this->setSearchPlaceholderText();
    edit->setClearButtonEnabled(true);
    edit->findChild<QAbstractButton *>()->setIcon(
        QPixmap(":/buttons/clearSearch.png"));
    this->ui_.search->installEventFilter(this);

    QObject::connect(edit.getElement(), &QLineEdit::textChanged, this,
                     &SettingsDialog::filterElements);

    // CENTER
    auto centerBox =
        outerBox.emplace<QHBoxLayout>().withoutMargin().withoutSpacing();

    // left side (tabs)
    centerBox.emplace<QWidget>()
        .assign(&this->ui_.tabContainerContainer)
        .setLayoutType<QVBoxLayout>()
        .withoutMargin()
        .assign(&this->ui_.tabContainer);
    this->ui_.tabContainerContainer->setFixedWidth(
        static_cast<int>(150 * this->dpi_));

    // right side (pages)
    centerBox.emplace<QStackedLayout>()
        .assign(&this->ui_.pageStack)
        .withoutMargin();

    this->ui_.pageStack->setContentsMargins(0, 0, 0, 0);

    outerBox->addSpacing(12);

    // BOTTOM
    auto buttons = outerBox.emplace<QDialogButtonBox>(Qt::Horizontal);
    {
        this->ui_.okButton =
            buttons->addButton("Ok", QDialogButtonBox::YesRole);
        this->ui_.cancelButton =
            buttons->addButton("Cancel", QDialogButtonBox::NoRole);
    }

    // ---- misc
    this->ui_.tabContainerContainer->setObjectName("tabWidget");
    this->ui_.pageStack->setObjectName("pages");

    QObject::connect(this->ui_.okButton, &QPushButton::clicked, this,
                     &SettingsDialog::onOkClicked);
    QObject::connect(this->ui_.cancelButton, &QPushButton::clicked, this,
                     &SettingsDialog::onCancelClicked);
}

void SettingsDialog::filterElements(const QString &text)
{
    // filter elements and hide pages
    for (auto &&tab : this->tabs_)
    {
        // filterElements returns true if anything on the page matches the search query
        tab->setVisible(tab->page()->filterElements(text) ||
                        tab->name().contains(text, Qt::CaseInsensitive));
    }

    // find next visible page
    if (this->lastSelectedByUser_ && this->lastSelectedByUser_->isVisible())
    {
        this->selectTab(this->lastSelectedByUser_, false);
    }
    else if (!this->selectedTab_->isVisible())
    {
        for (auto &&tab : this->tabs_)
        {
            if (tab->isVisible())
            {
                this->selectTab(tab, false);
                break;
            }
        }
    }

    // remove duplicate spaces
    bool shouldShowSpace = false;

    for (int i = 0; i < this->ui_.tabContainer->count(); i++)
    {
        auto *item = this->ui_.tabContainer->itemAt(i);
        if (auto *x = dynamic_cast<QSpacerItem *>(item); x)
        {
            x->changeSize(10, shouldShowSpace ? 16 : 0);
            shouldShowSpace = false;
        }
        else if (item->widget())
        {
            shouldShowSpace |= item->widget()->isVisible();
        }
    }
}

void SettingsDialog::setElementFilter(const QString &query)
{
    this->ui_.search->setText(query);
}

bool SettingsDialog::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->ui_.search && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
        if (keyEvent == QKeySequence::DeleteStartOfWord &&
            this->ui_.search->selectionLength() > 0)
        {
            this->ui_.search->backspace();
            return true;
        }
    }
    return false;
}

void SettingsDialog::addTabs()
{
    this->ui_.tabContainer->setSpacing(0);
    this->ui_.tabContainer->setContentsMargins(0, 20, 0, 20);

    // Constructors are wrapped in std::function to remove some strain from first time loading.

    // clang-format off
    this->addTab([]{return new GeneralPage;},          "General",        ":/settings/about.svg", SettingsTabId::General, 0.88F);
    this->addTab([]{return new CustomizationPage;},    "Customization",  ":/settings/customization.svg", SettingsTabId::Customization, 1.09F);
    this->addTab([]{return new MoltorinoPage;},        "Moltorino",      ":/settings/moltorino.svg", SettingsTabId::Moltorino, 0.90F);
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new AccountsPage;},         "Accounts",       ":/settings/accounts.svg", SettingsTabId::Accounts, 1.06F);
    this->addTab([]{return new NicknamesPage;},        "Nicknames",      ":/settings/nicknames.svg", {}, 1.12F);
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new CommandPage;},          "Commands",       ":/settings/commands.svg", {}, 1.10F);
    this->addTab([]{return new HighlightingPage;},     "Highlights",     ":/settings/highlights.svg", SettingsTabId::Highlights, 1.01F);
    this->addTab([]{return new IgnoresPage;},          "Ignores",        ":/settings/ignores.svg", {}, 1.08F);
    this->addTab([]{return new FiltersPage;},          "Filters",        ":/settings/filters.svg", {}, 1.04F);
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new KeyboardSettingsPage;}, "Hotkeys",        ":/settings/hotkeys.svg", {}, 1.05F);
    this->addTab([]{return new ModerationPage;},       "Moderation",     ":/settings/moderation.svg", SettingsTabId::Moderation, 1.00F);
    this->addTab([]{return new NotificationPage;},     "Live Notifications",  ":/settings/live-notifications.svg", {}, 1.15F);
    this->addTab([]{return new ExternalToolsPage;},    "External tools", ":/settings/external-tools.svg", {}, 1.05F);
#ifdef CHATTERINO_HAVE_PLUGINS
    this->addTab([]{return new PluginsPage;},          "Plugins",        ":/settings/plugins.svg", {}, 1.04F);
#endif
    this->ui_.tabContainer->addStretch(1);
    this->addTab([]{return new BackupSharingPage;},    "Settings Manager", ":/settings/settings-manager.svg", SettingsTabId::SettingsManager, 1.12F, Qt::AlignBottom);
    this->addTab([]{return new AboutPage;},            "About",          ":/settings/about.svg", SettingsTabId::About, 0.88F, Qt::AlignBottom);
    // clang-format on
}

void SettingsDialog::addTab(std::function<SettingsPage *()> page,
                            const QString &name, const QString &iconPath,
                            SettingsTabId id, float iconOpticalScale,
                            Qt::Alignment alignment)
{
    auto *tab = new SettingsDialogTab(this, std::move(page), name, iconPath, id,
                                      iconOpticalScale);
    tab->setFixedHeight(static_cast<int>(30 * this->dpi_));

    this->ui_.tabContainer->addWidget(tab, 0, alignment);
    this->tabs_.push_back(tab);

    if (this->tabs_.size() == 1)
    {
        this->selectTab(tab);
    }
}

void SettingsDialog::selectTab(SettingsDialogTab *tab, bool byUser)
{
    // add page if it's not been added yet
    [&] {
        for (int i = 0; i < this->ui_.pageStack->count(); i++)
        {
            if (this->ui_.pageStack->itemAt(i)->widget() == tab->page())
            {
                return;
            }
        }

        this->ui_.pageStack->addWidget(tab->page());
    }();

    this->ui_.pageStack->setCurrentWidget(tab->page());

    if (this->selectedTab_ != nullptr)
    {
        this->selectedTab_->setSelected(false);
    }

    tab->setSelected(true);
    this->selectedTab_ = tab;
    if (byUser)
    {
        this->lastSelectedByUser_ = tab;
    }
}

void SettingsDialog::selectTab(SettingsTabId id)
{
    auto *t = this->tab(id);
    assert(t);
    if (!t)
    {
        return;
    }

    this->selectTab(t);
}

SettingsDialogTab *SettingsDialog::tab(SettingsTabId id)
{
    for (auto &&tab : this->tabs_)
    {
        if (tab->id() == id)
        {
            return tab;
        }
    }

    assert(false);
    return nullptr;
}

void SettingsDialog::showDialog(SettingsDialogPreference preferredTab)
{
    static QPointer<SettingsDialog> instance;
    if (instance == nullptr)
    {
        instance = new SettingsDialog(&getApp()->getWindows()->getMainWindow());
    }
    else
    {
        instance->refresh();
    }

    // Resets the cancel button.
    getSettings()->saveSnapshot();

    switch (preferredTab)
    {
        case SettingsDialogPreference::Accounts:
            instance->selectTab(SettingsTabId::Accounts);
            break;

        case SettingsDialogPreference::Highlights:
            instance->selectTab(SettingsTabId::Highlights);
            break;

        case SettingsDialogPreference::ModerationActions:
            if (auto *tab = instance->tab(SettingsTabId::Moderation))
            {
                instance->selectTab(tab);
                if (auto *page = dynamic_cast<ModerationPage *>(tab->page()))
                {
                    page->selectModerationActions();
                }
            }
            break;

        case SettingsDialogPreference::StreamerMode: {
            instance->selectTab(SettingsTabId::General);
        }
        break;

        case SettingsDialogPreference::About: {
            instance->selectTab(SettingsTabId::About);
        }
        break;

        case SettingsDialogPreference::Moltorino: {
            instance->selectTab(SettingsTabId::Moltorino);
        }
        break;

        case SettingsDialogPreference::MoltorinoAccounts: {
            if (auto *tab = instance->tab(SettingsTabId::Moltorino))
            {
                instance->selectTab(tab);
                if (auto *page = dynamic_cast<MoltorinoPage *>(tab->page()))
                {
                    page->showAccountSetup();
                }
            }
        }
        break;

        case SettingsDialogPreference::BotBadge: {
            if (auto *tab = instance->tab(SettingsTabId::Moltorino))
            {
                instance->selectTab(tab);
                if (auto *page = dynamic_cast<MoltorinoPage *>(tab->page()))
                {
                    page->showBotBadgeSetup();
                }
            }
        }
        break;

        default:;
    }

    if (instance->isMinimized())
    {
        instance->setWindowState((instance->windowState() &
                                  ~Qt::WindowMinimized) |
                                 Qt::WindowActive);
    }
    if (instance->width() < 200 || instance->height() < 120)
    {
        instance->resize(915, 600);
    }
    instance->show();
    if (preferredTab == SettingsDialogPreference::StreamerMode)
    {
        // this is needed because each time the settings are opened, the query is reset
        instance->setElementFilter("Streamer Mode");
    }
    instance->activateWindow();
    instance->raise();
    instance->setFocus();
}

void SettingsDialog::refresh()
{
    // Update tabs that have already been opened. Calling page() here would
    // construct every settings page on repeated opens, which is rough during
    // startup and defeats the lazy loading above.
    for (auto *tab : this->tabs_)
    {
        if (auto *page = tab->createdPage())
        {
            page->onShow();
        }
    }
}

void SettingsDialog::scaleChangedEvent(float newScale)
{
    assert(newScale == 1.F &&
           "Scaling is disabled for the settings dialog - its scale should "
           "always be 1");

    for (SettingsDialogTab *tab : this->tabs_)
    {
        tab->setFixedHeight(30);
    }

    if (this->ui_.tabContainerContainer)
    {
        this->ui_.tabContainerContainer->setFixedWidth(150);
    }
}

void SettingsDialog::showEvent(QShowEvent *e)
{
    this->ui_.search->setText("");
    BaseWindow::showEvent(e);
}

///// Widget creation helpers
void SettingsDialog::onOkClicked()
{
    if (!getApp()->getArgs().dontSaveSettings)
    {
        getApp()->getCommands()->save();
    }

    getSettings()->requestSave();

    this->close();
}

void SettingsDialog::onCancelClicked()
{
    getSettings()->restoreSnapshot();

    this->close();
}

}  // namespace chatterino
