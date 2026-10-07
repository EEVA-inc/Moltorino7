#include "widgets/dialogs/UserRolesView.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Clipboard.hpp"
#include "util/IncognitoBrowser.hpp"
#include "widgets/Label.hpp"

#include <QAbstractItemView>
#include <QAbstractListModel>
#include <QComboBox>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

constexpr qsizetype MAX_ROLE_ENTRIES = 10000;

QString compactRoleName(twitch_roles::Role role)
{
    switch (role)
    {
        case twitch_roles::Role::Moderator:
            return QStringLiteral("Mods");
        case twitch_roles::Role::Vip:
            return QStringLiteral("VIPs");
        case twitch_roles::Role::Founder:
            return QStringLiteral("Founders");
        case twitch_roles::Role::Artist:
            return QStringLiteral("Artists");
    }
    return {};
}

QString roleTabText(twitch_roles::Role role, qint64 count)
{
    return QStringLiteral("%1 %2")
        .arg(compactRoleName(role), QLocale().toString(count));
}

QString roleSince(const QDate &date)
{
    if (!date.isValid())
    {
        return {};
    }
    return QStringLiteral("Since %1")
        .arg(QLocale().toString(date, QStringLiteral("MMM d, yyyy")));
}

void openExternalUrl(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
    {
        return;
    }
    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(url.toString());
        return;
    }
    QDesktopServices::openUrl(url);
}

QUrl twitchUserUrl(const QString &login)
{
    if (login.isEmpty())
    {
        return {};
    }
    QUrl url(QStringLiteral("https://www.twitch.tv"));
    url.setPath(u'/' + login.toLower());
    return url;
}

}

class UserRolesModel final : public QAbstractListModel
{
public:
    enum Role {
        Login = Qt::UserRole + 1,
        GrantedDate,
        Partner,
        Affiliate,
    };

    UserRolesModel(QVector<twitch_roles::Entry> *entries, QObject *parent)
        : QAbstractListModel(parent)
        , entries_(entries)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() || this->entries_ == nullptr
                   ? 0
                   : this->entries_->size();
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || this->entries_ == nullptr || index.row() < 0 ||
            index.row() >= this->entries_->size())
        {
            return {};
        }
        const auto &entry = this->entries_->at(index.row());
        switch (role)
        {
            case Qt::DisplayRole:
                return entry.displayName;
            case Login:
                return entry.login;
            case GrantedDate:
                return entry.grantedDate;
            case Partner:
                return entry.partner;
            case Affiliate:
                return entry.affiliate;
            case Qt::ToolTipRole: {
                QStringList lines{entry.displayName + QStringLiteral(" (@") +
                                  entry.login + u')'};
                const auto since = roleSince(entry.grantedDate);
                if (!since.isEmpty())
                {
                    lines.push_back(since);
                }
                return lines.join(u'\n');
            }
            default:
                return {};
        }
    }

    void clear()
    {
        if (this->entries_->isEmpty())
        {
            return;
        }
        this->beginResetModel();
        this->entries_->clear();
        this->endResetModel();
    }

    void mergeSorted(QVector<twitch_roles::Entry> entries)
    {
        if (entries.isEmpty())
        {
            return;
        }
        this->beginResetModel();
        this->entries_->reserve(this->entries_->size() + entries.size());
        for (auto &entry : entries)
        {
            this->entries_->push_back(std::move(entry));
        }
        twitch_roles::detail::sortNewestFirst(*this->entries_);
        this->endResetModel();
    }

private:
    QVector<twitch_roles::Entry> *entries_{};
};

namespace {

class UserRoleDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setColors(const QColor &background, const QColor &alternate,
                   const QColor &hover, const QColor &selected,
                   const QColor &text, const QColor &muted,
                   const QColor &badge)
    {
        this->background_ = background;
        this->alternate_ = alternate;
        this->hover_ = hover;
        this->selected_ = selected;
        this->text_ = text;
        this->muted_ = muted;
        this->badge_ = badge;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(),
                std::max(42, QFontMetrics(option.font).height() * 2 + 10)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        const auto background =
            option.state & QStyle::State_Selected    ? this->selected_
            : option.state & QStyle::State_MouseOver ? this->hover_
            : index.row() % 2 == 0                   ? this->background_
                                                     : this->alternate_;
        painter->fillRect(option.rect, background);

        const QFontMetrics metrics(option.font);
        const auto left = option.rect.left() + 9;
        const auto right = option.rect.right() - 9;
        const auto lineHeight = metrics.height();
        const auto firstBaseline =
            option.rect.top() + (option.rect.height() - lineHeight * 2) / 2 +
            metrics.ascent();
        const auto secondBaseline = firstBaseline + lineHeight;

        QString badge;
        if (index.data(UserRolesModel::Partner).toBool())
        {
            badge = QStringLiteral("Partner");
        }
        else if (index.data(UserRolesModel::Affiliate).toBool())
        {
            badge = QStringLiteral("Affiliate");
        }
        const auto badgeWidth = badge.isEmpty()
                                    ? 0
                                    : metrics.horizontalAdvance(badge) + 12;

        auto titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(this->text_);
        const QFontMetrics titleMetrics(titleFont);
        const auto titleWidth = std::max(0, right - left - badgeWidth - 8);
        painter->drawText(
            left, firstBaseline,
            titleMetrics.elidedText(index.data().toString(), Qt::ElideRight,
                                    titleWidth));

        if (!badge.isEmpty())
        {
            painter->setFont(option.font);
            painter->setPen(this->badge_);
            painter->drawText(right - badgeWidth + 12, firstBaseline, badge);
        }

        const auto login = index.data(UserRolesModel::Login).toString();
        const auto displayName = index.data().toString();
        QStringList details;
        if (displayName.compare(login, Qt::CaseInsensitive) != 0)
        {
            details.push_back(u'@' + login);
        }
        const auto since =
            roleSince(index.data(UserRolesModel::GrantedDate).toDate());
        if (!since.isEmpty())
        {
            details.push_back(since);
        }
        painter->setFont(option.font);
        painter->setPen(this->muted_);
        painter->drawText(
            left, secondBaseline,
            metrics.elidedText(details.join(QStringLiteral(" · ")),
                               Qt::ElideRight, std::max(0, right - left)));
        painter->restore();
    }

private:
    QColor background_;
    QColor alternate_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor muted_;
    QColor badge_;
};

}

UserRolesView::UserRolesView(QWidget *parent)
    : QWidget(parent)
{
    this->setAttribute(Qt::WA_NoMousePropagation);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    auto *viewRow = new QHBoxLayout;
    viewRow->setContentsMargins(0, 0, 0, 0);
    viewRow->setSpacing(5);
    this->perspectiveBox_ = new QComboBox(this);
    this->perspectiveBox_->addItem("Their roles",
                                  static_cast<int>(
                                      twitch_roles::Perspective::User));
    this->perspectiveBox_->addItem("Their channel",
                                  static_cast<int>(
                                      twitch_roles::Perspective::Channel));
    this->perspectiveBox_->setToolTip(
        "Switch between roles this user holds and roles in their channel");
    this->perspectiveBox_->setAccessibleName("Role view");
    viewRow->addWidget(this->perspectiveBox_);
    viewRow->addStretch(1);
    this->manageButton_ = new QPushButton("Manage", this);
    this->manageButton_->setAutoDefault(false);
    this->manageButton_->setToolTip(
        "Manage this user's roles in the current chat");
    this->manageButton_->hide();
    viewRow->addWidget(this->manageButton_);
    this->webButton_ = new QPushButton("Open on web", this);
    this->webButton_->setAutoDefault(false);
    viewRow->addWidget(this->webButton_);
    layout->addLayout(viewRow);

    this->roleTabs_ = new QTabBar(this);
    this->roleTabs_->setDrawBase(false);
    this->roleTabs_->setExpanding(true);
    this->roleTabs_->setUsesScrollButtons(false);
    this->roleTabs_->setAccessibleName("Role type");
    this->roleTabs_->hide();
    layout->addWidget(this->roleTabs_);

    this->list_ = new QListView(this);
    this->model_ = new UserRolesModel(&this->entries_, this);
    this->list_->setModel(this->model_);
    this->list_->setItemDelegate(new UserRoleDelegate(this->list_));
    this->list_->setFrameShape(QFrame::NoFrame);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->list_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->list_->setMouseTracking(true);
    this->list_->setUniformItemSizes(true);
    this->list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(this->list_, 1);

    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(5);
    this->statusLabel_ = new Label;
    this->statusLabel_->setPadding({9, 0, 0, 0});
    this->statusLabel_->setShouldElide(true);
    this->statusLabel_->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Preferred);
    statusRow->addWidget(this->statusLabel_, 1);
    this->retryButton_ = new QPushButton("Retry", this);
    this->retryButton_->setAutoDefault(false);
    this->retryButton_->hide();
    statusRow->addWidget(this->retryButton_);
    this->loadMoreButton_ = new QPushButton("Load more", this);
    this->loadMoreButton_->setAutoDefault(false);
    this->loadMoreButton_->hide();
    statusRow->addWidget(this->loadMoreButton_);
    layout->addLayout(statusRow);

    QObject::connect(
        this->perspectiveBox_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int index) {
            const auto perspective = static_cast<twitch_roles::Perspective>(
                this->perspectiveBox_->itemData(index).toInt());
            if (perspective == this->perspective_)
            {
                return;
            }
            this->perspective_ = perspective;
            this->resetPerspective();
            if (this->activated_)
            {
                this->loadSummary();
            }
        });
    QObject::connect(this->roleTabs_, &QTabBar::currentChanged, this,
                     [this](int index) {
                         if (index < 0)
                         {
                             return;
                         }
                         const auto role = static_cast<twitch_roles::Role>(
                             this->roleTabs_->tabData(index).toInt());
                         this->selectRole(role);
                     });
    QObject::connect(this->manageButton_, &QPushButton::clicked, this,
                     [this] {
                         if (this->roleManagementCallback_)
                         {
                             this->roleManagementCallback_(
                                 this->manageButton_);
                         }
                     });
    QObject::connect(this->webButton_, &QPushButton::clicked, this,
                     [this] { this->openOnWeb(); });
    QObject::connect(this->retryButton_, &QPushButton::clicked, this, [this] {
        this->error_.clear();
        if (!this->summaryLoaded_)
        {
            this->loadSummary();
            return;
        }
        this->requestPage(this->retryCursor_);
    });
    QObject::connect(this->loadMoreButton_, &QPushButton::clicked, this,
                     [this] { this->loadMore(); });
    QObject::connect(this->list_, &QListView::activated, this,
                     [this](const QModelIndex &index) {
                         this->openEntry(index.row());
                     });
    QObject::connect(this->list_, &QListView::customContextMenuRequested, this,
                     [this](const QPoint &position) {
                         this->showEntryMenu(position);
                     });
    QObject::connect(this->list_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         auto *scrollbar = this->list_->verticalScrollBar();
                         if (!this->loadingPage_ && !this->nextCursor_.isEmpty() &&
                             scrollbar->maximum() - value <= 80)
                         {
                             this->loadMore();
                         }
                     });

    this->setMinimumSize(430, 275);
    this->updateControls();
    this->updateStatus();
}

void UserRolesView::setContext(const QString &userID, const QString &login,
                               bool userLookupFinished)
{
    const auto normalizedID = userID.trimmed();
    const auto normalizedLogin = login.trimmed().toLower();
    if (this->userID_ == normalizedID && this->login_ == normalizedLogin &&
        this->userLookupFinished_ == userLookupFinished)
    {
        return;
    }
    this->userID_ = normalizedID;
    this->login_ = normalizedLogin;
    this->userLookupFinished_ = userLookupFinished;
    this->perspective_ = twitch_roles::Perspective::User;
    const QSignalBlocker blocker(this->perspectiveBox_);
    this->perspectiveBox_->setCurrentIndex(0);
    this->resetPerspective();
    if (this->activated_ && !this->userID_.isEmpty())
    {
        this->loadSummary();
    }
}

void UserRolesView::setRoleManagementAvailable(bool available)
{
    this->manageButton_->setVisible(available);
}

void UserRolesView::setRoleManagementCallback(
    std::function<void(QWidget *)> callback)
{
    this->roleManagementCallback_ = std::move(callback);
}

void UserRolesView::activate()
{
    this->activated_ = true;
    if (!this->summaryLoaded_ && !this->loadingSummary_ &&
        !this->userID_.isEmpty())
    {
        this->loadSummary();
    }
}

void UserRolesView::deactivate()
{
    this->activated_ = false;
}

void UserRolesView::refreshStyle(float scale)
{
    const auto font = getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
    const auto *theme = getTheme();
    auto palette = this->palette();
    palette.setColor(QPalette::Window, theme->window.background);
    palette.setColor(QPalette::WindowText, theme->window.text);
    palette.setColor(QPalette::Base, theme->messages.backgrounds.regular);
    palette.setColor(QPalette::AlternateBase,
                     theme->messages.backgrounds.alternate);
    palette.setColor(QPalette::Text, theme->messages.textColors.regular);
    palette.setColor(QPalette::Button,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ButtonText, theme->window.text);
    palette.setColor(QPalette::Highlight, theme->messages.selection);
    palette.setColor(QPalette::HighlightedText,
                     theme->messages.textColors.regular);
    palette.setColor(QPalette::PlaceholderText, theme->window.text.darker(160));
    this->setPalette(palette);
    this->setAutoFillBackground(true);

    for (auto *widget : this->findChildren<QWidget *>())
    {
        widget->setFont(font);
        widget->setPalette(palette);
    }
    this->setFont(font);

    if (auto *delegate =
            static_cast<UserRoleDelegate *>(this->list_->itemDelegate()))
    {
        delegate->setColors(
            theme->messages.backgrounds.regular,
            theme->messages.backgrounds.alternate,
            theme->messages.backgrounds.regular.lighter(115),
            theme->messages.selection, theme->messages.textColors.regular,
            theme->messages.textColors.regular.darker(145),
            theme->tabs.selected.text);
    }
    this->list_->doItemsLayout();
    this->list_->viewport()->update();
}

void UserRolesView::resetPerspective()
{
    ++this->generation_;
    this->error_.clear();
    this->summary_ = {};
    this->model_->clear();
    this->nextCursor_.clear();
    this->retryCursor_.clear();
    this->total_ = 0;
    this->summaryLoaded_ = false;
    this->loadingSummary_ = false;
    this->loadingPage_ = false;
    const QSignalBlocker blocker(this->roleTabs_);
    while (this->roleTabs_->count() > 0)
    {
        this->roleTabs_->removeTab(0);
    }
    this->roleTabs_->hide();
    this->updateControls();
    this->updateStatus();
}

void UserRolesView::loadSummary()
{
    if (this->userID_.isEmpty() || this->loadingSummary_)
    {
        return;
    }
    this->error_.clear();
    this->loadingSummary_ = true;
    this->summaryLoaded_ = false;
    const auto generation = this->generation_;
    const QPointer<UserRolesView> self(this);
    this->updateControls();
    this->updateStatus();
    twitch_roles::loadSummary(
        this->perspective_, this->userID_,
        [self, generation](twitch_roles::Summary summary) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingSummary_ = false;
            self->summaryLoaded_ = true;
            self->summary_ = std::move(summary);
            self->rebuildRoleTabs();
            self->updateControls();
            self->updateStatus();
        },
        [self, generation](QString error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingSummary_ = false;
            self->summaryLoaded_ = false;
            self->error_ = std::move(error);
            self->updateControls();
            self->updateStatus();
        });
}

void UserRolesView::rebuildRoleTabs()
{
    const QSignalBlocker blocker(this->roleTabs_);
    while (this->roleTabs_->count() > 0)
    {
        this->roleTabs_->removeTab(0);
    }
    if (!this->summary_.found)
    {
        return;
    }

    int selectedIndex = -1;
    for (const auto role : twitch_roles::ROLES)
    {
        const auto count = this->summary_.count(role);
        if (count <= 0)
        {
            continue;
        }
        const auto index = this->roleTabs_->addTab(roleTabText(role, count));
        this->roleTabs_->setTabData(index, static_cast<int>(role));
        if (selectedIndex < 0)
        {
            selectedIndex = index;
            this->selectedRole_ = role;
        }
    }
    if (selectedIndex >= 0)
    {
        this->roleTabs_->show();
        this->roleTabs_->setCurrentIndex(selectedIndex);
        this->loadFirstPage();
    }
}

void UserRolesView::selectRole(twitch_roles::Role role)
{
    if (!this->summaryLoaded_ || role == this->selectedRole_)
    {
        return;
    }
    this->selectedRole_ = role;
    ++this->generation_;
    this->loadingSummary_ = false;
    this->loadingPage_ = false;
    this->loadFirstPage();
}

void UserRolesView::loadFirstPage()
{
    this->model_->clear();
    this->nextCursor_.clear();
    this->retryCursor_.clear();
    this->total_ = this->summary_.count(this->selectedRole_);
    this->error_.clear();
    this->requestPage({});
}

void UserRolesView::loadMore()
{
    if (!this->nextCursor_.isEmpty())
    {
        this->requestPage(this->nextCursor_);
    }
}

void UserRolesView::requestPage(const QString &cursor)
{
    if (this->userID_.isEmpty() || this->loadingPage_ ||
        !this->summaryLoaded_)
    {
        return;
    }
    this->loadingPage_ = true;
    this->error_.clear();
    this->retryCursor_ = cursor;
    const auto generation = this->generation_;
    const auto role = this->selectedRole_;
    const QPointer<UserRolesView> self(this);
    this->updateControls();
    this->updateStatus();
    twitch_roles::loadPage(
        this->perspective_, role, this->userID_, cursor,
        [self, generation, role, cursor](twitch_roles::Page page) mutable {
            if (!self || generation != self->generation_ ||
                role != self->selectedRole_)
            {
                return;
            }
            if (twitch_roles::detail::repeatsCursor(cursor,
                                                    page.nextCursor))
            {
                self->loadingPage_ = false;
                self->error_ = "Roles.tv returned an invalid page.";
                self->nextCursor_.clear();
                self->updateControls();
                self->updateStatus();
                return;
            }
            QSet<QString> known;
            known.reserve(self->entries_.size() + page.entries.size());
            for (const auto &entry : self->entries_)
            {
                known.insert(entry.userID);
            }
            QVector<twitch_roles::Entry> additions;
            additions.reserve(page.entries.size());
            for (auto &entry : page.entries)
            {
                if (!known.contains(entry.userID) &&
                    self->entries_.size() + additions.size() < MAX_ROLE_ENTRIES)
                {
                    known.insert(entry.userID);
                    additions.push_back(std::move(entry));
                }
            }
            const auto topIndex = self->list_->indexAt(QPoint(0, 0));
            const auto selectedIndex = self->list_->currentIndex();
            const auto topUserID = topIndex.isValid()
                                       ? self->entries_.at(topIndex.row()).userID
                                       : QString{};
            const auto selectedUserID =
                selectedIndex.isValid()
                    ? self->entries_.at(selectedIndex.row()).userID
                    : QString{};
            const auto topOffset = topIndex.isValid()
                                       ? self->list_->visualRect(topIndex).top()
                                       : 0;

            self->total_ = page.total;
            self->model_->mergeSorted(std::move(additions));
            const auto rowForUser = [self](const QString &userID) {
                if (userID.isEmpty())
                {
                    return -1;
                }
                for (int row = 0; row < self->entries_.size(); ++row)
                {
                    if (self->entries_.at(row).userID == userID)
                    {
                        return row;
                    }
                }
                return -1;
            };
            if (const auto selectedRow = rowForUser(selectedUserID);
                selectedRow >= 0)
            {
                self->list_->setCurrentIndex(
                    self->model_->index(selectedRow));
            }
            if (const auto topRow = rowForUser(topUserID); topRow >= 0)
            {
                self->list_->scrollTo(self->model_->index(topRow),
                                      QAbstractItemView::PositionAtTop);
                self->list_->verticalScrollBar()->setValue(
                    self->list_->verticalScrollBar()->value() - topOffset);
            }
            self->nextCursor_ =
                self->entries_.size() >= self->total_ ||
                        self->entries_.size() >= MAX_ROLE_ENTRIES
                    ? QString{}
                    : page.nextCursor;
            self->retryCursor_.clear();
            self->loadingPage_ = false;
            self->updateControls();
            self->updateStatus();
        },
        [self, generation, role](QString error) {
            if (!self || generation != self->generation_ ||
                role != self->selectedRole_)
            {
                return;
            }
            self->loadingPage_ = false;
            self->error_ = std::move(error);
            self->updateControls();
            self->updateStatus();
        });
}

void UserRolesView::updateControls()
{
    const auto ready = !this->userID_.isEmpty();
    this->perspectiveBox_->setEnabled(ready);
    this->roleTabs_->setEnabled(this->summaryLoaded_);
    this->webButton_->setEnabled(!this->login_.isEmpty());
    this->retryButton_->setVisible(!this->error_.isEmpty());
    this->loadMoreButton_->setVisible(
        this->error_.isEmpty() && !this->loadingPage_ &&
        !this->nextCursor_.isEmpty());
}

void UserRolesView::updateStatus()
{
    this->statusLabel_->setToolTip({});
    if (this->userID_.isEmpty())
    {
        this->statusLabel_->setText(
            this->userLookupFinished_
                ? "Roles are unavailable for this user."
                : "Loading user details...");
        return;
    }
    if (this->loadingSummary_)
    {
        this->statusLabel_->setText("Loading roles...");
        return;
    }
    if (!this->error_.isEmpty())
    {
        this->statusLabel_->setText(this->error_);
        this->statusLabel_->setToolTip(this->error_);
        return;
    }
    if (this->summaryLoaded_ &&
        (!this->summary_.found || this->roleTabs_->count() == 0))
    {
        this->statusLabel_->setText(
            this->perspective_ == twitch_roles::Perspective::User
                ? "No tracked roles found for this user."
                : "No tracked roles found in this channel.");
        return;
    }
    if (this->loadingPage_)
    {
        if (!this->entries_.isEmpty())
        {
            this->statusLabel_->setText("Loading more...");
        }
        else if (this->perspective_ == twitch_roles::Perspective::User)
        {
            this->statusLabel_->setText("Loading channels...");
        }
        else
        {
            this->statusLabel_->setText("Loading users...");
        }
        return;
    }
    if (!this->summaryLoaded_)
    {
        this->statusLabel_->setText("Roles are ready.");
        return;
    }
    if (this->entries_.isEmpty())
    {
        this->statusLabel_->setText("No active roles found.");
        return;
    }

    if (this->entries_.size() >= MAX_ROLE_ENTRIES &&
        this->total_ > this->entries_.size())
    {
        this->statusLabel_->setText(QStringLiteral("Showing the first %1 roles")
                                       .arg(QLocale().toString(MAX_ROLE_ENTRIES)));
        this->statusLabel_->setToolTip(
            QStringLiteral("Open on web to view the remaining roles."));
        return;
    }

    const auto singular =
        (this->nextCursor_.isEmpty() ? this->entries_.size() : this->total_) ==
        1;
    const auto noun = this->perspective_ == twitch_roles::Perspective::User
                          ? (singular ? "channel" : "channels")
                          : (singular ? "user" : "users");
    if (!this->nextCursor_.isEmpty())
    {
        this->statusLabel_->setText(
            QStringLiteral("%1 of %2 %3")
                .arg(QLocale().toString(this->entries_.size()),
                     QLocale().toString(this->total_), noun));
        return;
    }
    this->statusLabel_->setText(
        QStringLiteral("%1 %2")
            .arg(QLocale().toString(this->entries_.size()), noun));
}

void UserRolesView::openOnWeb() const
{
    openExternalUrl(
        twitch_roles::detail::websiteUrl(this->perspective_, this->login_));
}

void UserRolesView::openEntry(int row) const
{
    if (row < 0 || row >= this->entries_.size())
    {
        return;
    }
    openExternalUrl(twitchUserUrl(this->entries_.at(row).login));
}

void UserRolesView::showEntryMenu(const QPoint &position)
{
    const auto index = this->list_->indexAt(position);
    if (!index.isValid())
    {
        return;
    }
    this->list_->setCurrentIndex(index);
    const auto &entry = this->entries_.at(index.row());
    auto *menu = new QMenu(this);
    menu->addAction(this->perspective_ == twitch_roles::Perspective::User
                       ? "Open channel on Twitch"
                       : "Open user on Twitch",
                   this, [login = entry.login] {
                       openExternalUrl(twitchUserUrl(login));
                   });
    menu->addAction(this->perspective_ == twitch_roles::Perspective::User
                       ? "Copy channel name"
                       : "Copy username",
                   this, [login = entry.login] { crossPlatformCopy(login); });
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(this->list_->viewport()->mapToGlobal(position));
}

}
