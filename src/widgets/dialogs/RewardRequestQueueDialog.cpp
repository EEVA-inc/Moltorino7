#include "widgets/dialogs/RewardRequestQueueDialog.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "Application.hpp"
#    include "common/Channel.hpp"
#    include "controllers/accounts/AccountController.hpp"
#    include "providers/moltorino/MoltorinoAuth.hpp"
#    include "singletons/Fonts.hpp"
#    include "singletons/Settings.hpp"
#    include "singletons/WindowManager.hpp"
#    include "widgets/dialogs/DialogLinkText.hpp"
#    include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#    include "widgets/Window.hpp"

#    include <QAbstractTableModel>
#    include <QDateTime>
#    include <QFrame>
#    include <QHBoxLayout>
#    include <QHeaderView>
#    include <QItemSelectionModel>
#    include <QKeySequence>
#    include <QLabel>
#    include <QListWidget>
#    include <QLocale>
#    include <QMessageBox>
#    include <QPainter>
#    include <QPushButton>
#    include <QScrollBar>
#    include <QSignalBlocker>
#    include <QFontMetrics>
#    include <QSizePolicy>
#    include <QSplitter>
#    include <QStyledItemDelegate>
#    include <QStyleOptionViewItem>
#    include <QTableView>
#    include <QVBoxLayout>

#    include <algorithm>
#    include <utility>

namespace chatterino {

namespace {

constexpr int MAX_LOADED_REWARD_REQUESTS = 1000;
constexpr int REWARD_UPDATE_BATCH_SIZE = 50;

QString queueUserName(const GqlModeratorQueueUser &user)
{
    if (!user.displayName.trimmed().isEmpty())
    {
        return user.displayName.trimmed();
    }
    if (!user.login.trimmed().isEmpty())
    {
        return user.login.trimmed();
    }
    return QStringLiteral("Unknown user");
}

QString relativeQueueTime(const QString &timestamp)
{
    const auto time = QDateTime::fromString(timestamp, Qt::ISODate);
    if (!time.isValid())
    {
        return QStringLiteral("Unknown");
    }

    const auto seconds = time.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 60)
    {
        return QStringLiteral("Just now");
    }
    if (seconds < 60 * 60)
    {
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    }
    if (seconds < 24 * 60 * 60)
    {
        return QStringLiteral("%1 hr ago").arg(seconds / (60 * 60));
    }
    if (seconds < 7 * 24 * 60 * 60)
    {
        const auto days = seconds / (24 * 60 * 60);
        return days == 1 ? QStringLiteral("1 day ago")
                         : QStringLiteral("%1 days ago").arg(days);
    }
    return QLocale().toString(time.toLocalTime().date(), QLocale::ShortFormat);
}

QString localQueueTime(const QString &timestamp)
{
    const auto time = QDateTime::fromString(timestamp, Qt::ISODate);
    return time.isValid()
               ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat)
               : QStringLiteral("Unknown time");
}

QColor rewardColor(const QString &colorText)
{
    QColor color(colorText);
    if (!color.isValid())
    {
        color = QColor(QStringLiteral("#9146ff"));
    }
    return color;
}

enum RewardItemRole {
    RewardIdRole = Qt::UserRole,
    RewardTitleRole,
    RewardCountRole,
    RewardColorRole,
};

}

class RewardListDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(), 28};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        painter->fillRect(
            option.rect,
            selected ? option.palette.highlight()
            : hovered ? option.palette.alternateBase()
                      : option.palette.base());

        const auto foreground = selected
                                    ? option.palette.highlightedText().color()
                                    : option.palette.text().color();
        const int swatchSize = 10;
        const int swatchLeft = option.rect.left() + 8;
        const int swatchTop =
            option.rect.top() + (option.rect.height() - swatchSize) / 2;
        painter->setPen(Qt::NoPen);
        painter->setBrush(index.data(RewardColorRole).value<QColor>());
        painter->drawRect(swatchLeft, swatchTop, swatchSize, swatchSize);

        const auto title = index.data(RewardTitleRole).toString();
        const auto count = index.data(RewardCountRole).toString();
        painter->setFont(option.font);
        painter->setPen(foreground);
        const QFontMetrics metrics(option.font);
        const int countWidth = metrics.horizontalAdvance(count);
        const int countRight = option.rect.right() - 9;
        painter->drawText(QRect(countRight - countWidth, option.rect.top(),
                                countWidth, option.rect.height()),
                          Qt::AlignRight | Qt::AlignVCenter, count);

        const int titleLeft = swatchLeft + swatchSize + 7;
        const int titleWidth =
            std::max(0, countRight - countWidth - 10 - titleLeft);
        painter->drawText(
            QRect(titleLeft, option.rect.top(), titleWidth,
                  option.rect.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(title, Qt::ElideRight, titleWidth));

        painter->setPen(option.palette.mid().color());
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }
};

class RewardRequestTableModel final : public QAbstractTableModel
{
public:
    enum Column {
        User,
        Reward,
        Request,
        Waiting,
        ColumnCount,
    };

    explicit RewardRequestTableModel(QObject *parent)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : this->rows_.size();
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
            case User:
                return QStringLiteral("Viewer");
            case Reward:
                return QStringLiteral("Reward");
            case Request:
                return QStringLiteral("Message");
            case Waiting:
                return QStringLiteral("Waiting");
            default:
                return {};
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->rows_.size())
        {
            return {};
        }
        const auto &request = this->rows_.at(index.row());
        if (role == Qt::ToolTipRole)
        {
            if (index.column() == User && !request.user.login.isEmpty())
            {
                return QStringLiteral("@%1").arg(request.user.login);
            }
            if (index.column() == Request)
            {
                return request.input.trimmed().isEmpty()
                           ? QStringLiteral("No message")
                           : request.input;
            }
            if (index.column() == Reward)
            {
                return request.rewardTitle;
            }
            if (index.column() == Waiting)
            {
                return localQueueTime(request.timestamp);
            }
            return {};
        }
        if (role != Qt::DisplayRole)
        {
            return {};
        }
        switch (index.column())
        {
            case User:
                return queueUserName(request.user);
            case Reward:
                return request.rewardTitle;
            case Request:
                return request.input.trimmed().isEmpty()
                           ? QStringLiteral("No message")
                           : request.input.simplified();
            case Waiting:
                return relativeQueueTime(request.timestamp);
            default:
                return {};
        }
    }

    void clear()
    {
        this->beginResetModel();
        this->rows_.clear();
        this->endResetModel();
    }

    void append(QVector<GqlRewardRequest> rows)
    {
        if (rows.isEmpty())
        {
            return;
        }
        const auto first = this->rows_.size();
        this->beginInsertRows({}, first, first + rows.size() - 1);
        this->rows_.append(std::move(rows));
        this->endInsertRows();
    }

    void removeIds(const QSet<QString> &ids)
    {
        if (ids.isEmpty())
        {
            return;
        }
        this->beginResetModel();
        this->rows_.removeIf([&ids](const auto &row) {
            return ids.contains(row.id);
        });
        this->endResetModel();
    }

    const GqlRewardRequest *requestAt(int row) const
    {
        if (row < 0 || row >= this->rows_.size())
        {
            return nullptr;
        }
        return &this->rows_.at(row);
    }

private:
    QVector<GqlRewardRequest> rows_;
};

std::vector<QPointer<RewardRequestQueueDialog>>
    RewardRequestQueueDialog::activeDialogs_;

RewardRequestQueueDialog::RewardRequestQueueDialog(
    QString channelId, QString channelLogin,
    std::weak_ptr<Channel> outputChannel, QWidget *parent)
    : QDialog(parent)
    , channelId_(std::move(channelId))
    , channelLogin_(std::move(channelLogin))
    , outputChannel_(std::move(outputChannel))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(
        QStringLiteral("Reward requests in #%1").arg(this->channelLogin_));
    this->setPalette(moltorinoDialogPalette(*getTheme()));
    this->setAutoFillBackground(true);
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));
    this->resize(720, 410);
    this->setMinimumSize(690, 410);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(7);

    auto *header = new QHBoxLayout;
    header->setSpacing(7);
    this->countLabel_ = new QLabel(QStringLiteral("0 open requests"), this);
    QFont countFont = makeResolvedFont(this->countLabel_->font(), QFont::Bold);
    this->countLabel_->setFont(countFont);
    header->addWidget(this->countLabel_);
    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setSizePolicy(QSizePolicy::Ignored,
                                      QSizePolicy::Preferred);
    header->addWidget(this->statusLabel_, 1);
    this->refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    this->refreshButton_->setShortcut(QKeySequence(Qt::Key_F5));
    this->refreshButton_->setToolTip(QStringLiteral("Refresh requests (F5)"));
    this->refreshButton_->setAutoDefault(false);
    this->refreshButton_->setSizePolicy(QSizePolicy::Maximum,
                                        QSizePolicy::Fixed);
    header->addWidget(this->refreshButton_);
    root->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto *rewardPanel = new QFrame(splitter);
    rewardPanel->setFrameShape(QFrame::NoFrame);
    rewardPanel->setMinimumWidth(160);
    rewardPanel->setMaximumWidth(225);
    auto *rewardLayout = new QVBoxLayout(rewardPanel);
    rewardLayout->setContentsMargins(0, 0, 0, 0);
    rewardLayout->setSpacing(5);
    auto *rewardsTitle =
        new QLabel(QStringLiteral("Rewards with requests"), rewardPanel);
    QFont sectionFont = makeResolvedFont(rewardsTitle->font(), QFont::Bold);
    rewardsTitle->setFont(sectionFont);
    rewardLayout->addWidget(rewardsTitle);
    this->rewards_ = new QListWidget(rewardPanel);
    this->rewards_->setItemDelegate(new RewardListDelegate(this->rewards_));
    this->rewards_->setUniformItemSizes(true);
    this->rewards_->setMouseTracking(true);
    this->rewards_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rewardLayout->addWidget(this->rewards_, 1);
    splitter->addWidget(rewardPanel);

    auto *requestPanel = new QWidget(splitter);
    auto *requestLayout = new QVBoxLayout(requestPanel);
    requestLayout->setContentsMargins(8, 0, 0, 0);
    requestLayout->setSpacing(7);

    auto *requestHeader = new QHBoxLayout;
    this->requestTitle_ =
        new QLabel(QStringLiteral("All pending requests"), requestPanel);
    this->requestTitle_->setFont(sectionFont);
    requestHeader->addWidget(this->requestTitle_);
    requestHeader->addStretch(1);
    this->loadedLabel_ = new QLabel(QStringLiteral("0 requests"), requestPanel);
    this->loadedLabel_->setObjectName(QStringLiteral("SecondaryText"));
    requestHeader->addWidget(this->loadedLabel_);
    this->loadMoreButton_ =
        new QPushButton(QStringLiteral("Load 50 more"), requestPanel);
    this->loadMoreButton_->setAutoDefault(false);
    this->loadMoreButton_->hide();
    requestHeader->addWidget(this->loadMoreButton_);
    requestLayout->addLayout(requestHeader);

    this->requestModel_ = new RewardRequestTableModel(this);
    this->requests_ = new QTableView(requestPanel);
    this->requests_->setModel(this->requestModel_);
    this->requests_->setItemDelegateForColumn(
        RewardRequestTableModel::Request,
        new DialogLinkTextDelegate(this->requests_));
    this->requests_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->requests_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->requests_->setAlternatingRowColors(true);
    this->requests_->setMouseTracking(true);
    this->requests_->setWordWrap(false);
    this->requests_->verticalHeader()->hide();
    this->requests_->verticalHeader()->setDefaultSectionSize(28);
    this->requests_->horizontalHeader()->setHighlightSections(false);
    this->requests_->horizontalHeader()->setStretchLastSection(false);
    this->requests_->horizontalHeader()->setSectionResizeMode(
        RewardRequestTableModel::User, QHeaderView::Interactive);
    this->requests_->horizontalHeader()->setSectionResizeMode(
        RewardRequestTableModel::Reward, QHeaderView::Interactive);
    this->requests_->horizontalHeader()->setSectionResizeMode(
        RewardRequestTableModel::Request, QHeaderView::Stretch);
    this->requests_->horizontalHeader()->setSectionResizeMode(
        RewardRequestTableModel::Waiting, QHeaderView::Interactive);
    this->requests_->setColumnWidth(RewardRequestTableModel::User, 96);
    this->requests_->setColumnWidth(RewardRequestTableModel::Reward, 174);
    this->requests_->setColumnWidth(RewardRequestTableModel::Waiting, 78);
    requestLayout->addWidget(this->requests_, 1);

    auto *preview = new QFrame(requestPanel);
    preview->setObjectName(QStringLiteral("RequestPreview"));
    preview->setFrameShape(QFrame::StyledPanel);
    preview->setBackgroundRole(QPalette::Base);
    preview->setAutoFillBackground(true);
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(9, 6, 9, 6);
    previewLayout->setSpacing(3);
    auto *previewHeader = new QHBoxLayout;
    previewHeader->setSpacing(7);
    this->selectionTitle_ = new QLabel(preview);
    this->selectionTitle_->setSizePolicy(QSizePolicy::Ignored,
                                         QSizePolicy::Preferred);
    QFont selectionFont =
        makeResolvedFont(this->selectionTitle_->font(), QFont::Bold);
    this->selectionTitle_->setFont(selectionFont);
    previewHeader->addWidget(this->selectionTitle_, 1);
    this->selectionMeta_ = new QLabel(preview);
    this->selectionMeta_->setObjectName(QStringLiteral("SecondaryText"));
    previewHeader->addWidget(this->selectionMeta_);
    previewLayout->addLayout(previewHeader);

    auto *previewActions = new QHBoxLayout;
    previewActions->setSpacing(7);
    this->selectionMessage_ = new QLabel(preview);
    this->selectionMessage_->setWordWrap(true);
    configureDialogLinkLabel(this->selectionMessage_);
    this->selectionMessage_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    this->selectionMessage_->setMaximumHeight(36);
    previewActions->addWidget(this->selectionMessage_, 1);
    this->completeButton_ =
        new QPushButton(QStringLiteral("Complete all"), preview);
    this->completeButton_->setObjectName(QStringLiteral("CompleteButton"));
    this->completeButton_->setAutoDefault(false);
    this->rejectButton_ =
        new QPushButton(QStringLiteral("Reject all and refund"), preview);
    this->rejectButton_->setObjectName(QStringLiteral("RejectButton"));
    this->rejectButton_->setAutoDefault(false);
    previewActions->addWidget(this->completeButton_);
    previewActions->addWidget(this->rejectButton_);
    previewLayout->addLayout(previewActions);
    requestLayout->addWidget(preview);

    splitter->addWidget(requestPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({175, 545});
    root->addWidget(splitter, 1);

    this->setStyleSheet(moltorinoDialogStyleSheet() + QStringLiteral(R"(
        QPushButton#CompleteButton:hover:enabled {
            background: #2f7d4a;
            color: white;
        }
        QPushButton#RejectButton:hover:enabled {
            background: #a53b43;
            color: white;
        }
    )"));

    QObject::connect(this->refreshButton_, &QPushButton::clicked, this, [this] {
        this->reloadOverview();
    });
    QObject::connect(this->loadMoreButton_, &QPushButton::clicked, this,
                     [this] {
                         this->loadNextPage();
                     });
    QObject::connect(this->rewards_, &QListWidget::currentItemChanged, this,
                     [this](QListWidgetItem *current) {
                         this->selectReward(current);
                     });
    QObject::connect(this->requests_->selectionModel(),
                     &QItemSelectionModel::selectionChanged, this, [this] {
                         this->updateSelectionPreview();
                         this->updateActionButtons();
                     });
    QObject::connect(this->requests_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         const auto *bar = this->requests_->verticalScrollBar();
                         if (value >= bar->maximum() - 2)
                         {
                             this->loadNextPage();
                         }
                     });
    QObject::connect(
        this->completeButton_, &QPushButton::clicked, this, [this] {
            this->resolveRequests(GqlRewardRequestResolution::Complete);
        });
    QObject::connect(this->rejectButton_, &QPushButton::clicked, this, [this] {
        this->resolveRequests(GqlRewardRequestResolution::RejectAndRefund);
    });
    QWidget::setTabOrder(this->refreshButton_, this->rewards_);
    QWidget::setTabOrder(this->rewards_, this->requests_);
    QWidget::setTabOrder(this->requests_, this->loadMoreButton_);
    QWidget::setTabOrder(this->loadMoreButton_, this->completeButton_);
    QWidget::setTabOrder(this->completeButton_, this->rejectButton_);
    this->themeConnections_.managedConnect(getTheme()->updated, [this] {
        this->applyTheme();
    });
    this->applyTheme();
    this->authToken_ = MoltorinoAuth::resolveModerationToken(
                           this->channelId_, this->channelLogin_)
                           .token;
    const auto authChanged = [this] {
        this->authToken_ = MoltorinoAuth::resolveModerationToken(
                               this->channelId_, this->channelLogin_)
                               .token;
        ++this->authGeneration_;
        ++this->requestGeneration_;
        this->overviewLoading_ = false;
        this->pageLoading_ = false;
        this->hasNextPage_ = false;
        this->overview_ = {};
        this->requestModel_->clear();
        this->rewards_->clear();
        this->refreshButton_->setEnabled(!this->actionInFlight_);
        this->updateSummary();
        this->updateSelectionPreview();
        this->updateActionButtons();
        if (!this->actionInFlight_)
        {
            this->reloadOverview();
        }
    };
    const auto checkAuth = [this, authChanged] {
        if (MoltorinoAuth::resolveModerationToken(this->channelId_,
                                                  this->channelLogin_)
                .token != this->authToken_)
        {
            authChanged();
        }
    };
    this->authConnections_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, authChanged);
    getSettings()->customPinAuthToken.connect(checkAuth, this->authConnections_,
                                              false);
    getSettings()->moltorinoAuthAccounts.connect(checkAuth,
                                                 this->authConnections_, false);
    for (auto *label : {this->countLabel_, this->statusLabel_,
                        this->requestTitle_, this->loadedLabel_,
                        this->selectionTitle_, this->selectionMeta_})
    {
        label->setTextFormat(Qt::PlainText);
    }
    this->updateSelectionPreview();
    this->reloadOverview();
}

void RewardRequestQueueDialog::showDialog(const QString &channelId,
                                          const QString &channelLogin,
                                          std::weak_ptr<Channel> outputChannel,
                                          QWidget *parent)
{
    std::erase_if(activeDialogs_, [](const auto &dialog) {
        return dialog.isNull();
    });
    for (const auto &dialog : activeDialogs_)
    {
        if (dialog->channelId_.compare(channelId, Qt::CaseInsensitive) == 0)
        {
            dialog->outputChannel_ = outputChannel;
            dialog->showNormal();
            dialog->raise();
            dialog->activateWindow();
            return;
        }
    }

    if (parent == nullptr)
    {
        parent = &getApp()->getWindows()->getMainWindow();
    }
    auto *dialog = new RewardRequestQueueDialog(
        channelId, channelLogin, std::move(outputChannel), parent);
    activeDialogs_.push_back(dialog);
    dialog->show();
}

QString RewardRequestQueueDialog::moderationToken(const QString &action)
{
    QString error;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channelId_, this->channelLogin_, &error);
    if (!auth.hasToken())
    {
        this->setStatus(error.isEmpty()
                            ? MoltorinoAuth::authRequiredMessage(action)
                            : error,
                        true);
        return {};
    }
    return auth.token;
}

void RewardRequestQueueDialog::reloadOverview(const QString &notice,
                                              bool noticeIsError)
{
    if (this->overviewLoading_ || this->actionInFlight_)
    {
        return;
    }
    const auto token = this->moderationToken("reward requests");
    if (token.isEmpty())
    {
        return;
    }

    this->refreshNotice_ = notice;
    this->refreshNoticeIsError_ = noticeIsError;
    const auto preferredReward = this->selectedRewardId_;
    const auto generation = ++this->requestGeneration_;
    this->pageLoading_ = false;
    this->pageLoadFailed_ = false;
    this->hasNextPage_ = false;
    this->overviewLoading_ = true;
    this->refreshButton_->setEnabled(false);
    this->loadMoreButton_->hide();
    this->updateActionButtons();
    this->setStatus(QStringLiteral("Loading reward requests..."));
    QPointer<RewardRequestQueueDialog> self(this);
    TwitchGql::getRewardRequestOverview(
        this->channelLogin_, this->channelId_, token,
        [self, generation,
         preferredReward](GqlRewardRequestOverview overview) mutable {
            if (!self || generation != self->requestGeneration_)
            {
                return;
            }
            self->overviewLoading_ = false;
            self->refreshButton_->setEnabled(true);
            self->overview_ = std::move(overview);
            if (!self->overview_.isAvailable || !self->overview_.isEnabled)
            {
                self->selectFirstRequestAfterReload_ = false;
                self->requestModel_->clear();
                auto message = QStringLiteral("Channel Points requests are not "
                                              "available for this channel.");
                if (!self->refreshNotice_.isEmpty())
                {
                    message = self->refreshNotice_ + QLatin1Char(' ') + message;
                }
                self->refreshNotice_.clear();
                self->refreshNoticeIsError_ = false;
                self->setStatus(message, true);
                self->rebuildRewardList();
                self->updateSummary();
                self->updateActionButtons();
                return;
            }
            self->rebuildRewardList(preferredReward);
            self->updateSummary();
            self->setStatus({});
            self->loadFirstPage();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->requestGeneration_)
            {
                return;
            }
            self->overviewLoading_ = false;
            self->refreshButton_->setEnabled(true);
            self->selectFirstRequestAfterReload_ = false;
            self->pageLoadFailed_ = true;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "loading reward requests", error);
            const auto message =
                self->refreshNotice_.isEmpty()
                    ? normalized
                    : self->refreshNotice_ + QLatin1Char(' ') + normalized;
            self->refreshNotice_.clear();
            self->refreshNoticeIsError_ = false;
            self->setStatus(message, true);
            self->updateActionButtons();
        });
}

void RewardRequestQueueDialog::rebuildRewardList(
    const QString &preferredRewardId)
{
    const QSignalBlocker blocker(this->rewards_);
    this->rewards_->clear();
    auto count = QLocale().toString(this->overview_.totalPendingCount);
    if (this->overview_.countAtMaximum)
    {
        count += QLatin1Char('+');
    }
    auto *all =
        new QListWidgetItem(QStringLiteral("All requests"), this->rewards_);
    all->setData(RewardIdRole, QString());
    all->setData(RewardTitleRole, QStringLiteral("All requests"));
    all->setData(RewardCountRole, count);
    all->setData(RewardColorRole, rewardColor(QStringLiteral("#9146ff")));
    all->setToolTip(QStringLiteral("Show every pending reward request"));

    QListWidgetItem *selected = all;
    for (const auto &reward : this->overview_.rewards)
    {
        if (reward.pendingCount <= 0)
        {
            continue;
        }
        auto rewardCount = QLocale().toString(reward.pendingCount);
        if (reward.countAtMaximum)
        {
            rewardCount += QLatin1Char('+');
        }
        auto *item = new QListWidgetItem(reward.title, this->rewards_);
        item->setData(RewardIdRole, reward.id);
        item->setData(RewardTitleRole, reward.title);
        item->setData(RewardCountRole, rewardCount);
        item->setData(RewardColorRole, rewardColor(reward.backgroundColor));
        item->setToolTip(
            reward.prompt.trimmed().isEmpty()
                ? QStringLiteral("Reward: %1\nCost: %2 points")
                      .arg(reward.title, QLocale().toString(reward.cost))
                : QStringLiteral("Reward: %1\nCost: %2 points\nDescription: %3")
                      .arg(reward.title, QLocale().toString(reward.cost),
                           reward.prompt));
        if (reward.id == preferredRewardId)
        {
            selected = item;
        }
    }
    this->rewards_->setCurrentItem(selected);
    this->selectedRewardId_ = selected->data(RewardIdRole).toString();
    this->requestTitle_->setText(
        this->selectedRewardId_.isEmpty()
            ? QStringLiteral("All pending requests")
            : selected->data(RewardTitleRole).toString());
    this->requests_->setColumnHidden(RewardRequestTableModel::Reward,
                                     !this->selectedRewardId_.isEmpty());
}

void RewardRequestQueueDialog::selectReward(QListWidgetItem *item)
{
    if (item == nullptr || this->overviewLoading_ || this->actionInFlight_)
    {
        return;
    }
    const auto rewardId = item->data(RewardIdRole).toString();
    if (rewardId == this->selectedRewardId_ &&
        this->requestModel_->rowCount() > 0)
    {
        return;
    }
    this->selectedRewardId_ = rewardId;
    this->requestTitle_->setText(rewardId.isEmpty()
                                     ? QStringLiteral("All pending requests")
                                     : item->data(RewardTitleRole).toString());
    this->requests_->setColumnHidden(RewardRequestTableModel::Reward,
                                     !rewardId.isEmpty());
    this->loadFirstPage();
}

void RewardRequestQueueDialog::loadFirstPage()
{
    ++this->requestGeneration_;
    this->requestModel_->clear();
    this->loadedLabel_->setText(QStringLiteral("0 requests"));
    this->seenRequestIds_.clear();
    this->nextCursor_.clear();
    this->hasNextPage_ = true;
    this->pageLoading_ = false;
    this->pageLoadFailed_ = false;
    this->updateSelectionPreview();
    this->updateActionButtons();
    this->loadNextPage();
}

void RewardRequestQueueDialog::loadNextPage()
{
    if (this->pageLoading_ || !this->hasNextPage_ || this->actionInFlight_ ||
        this->requestModel_->rowCount() >= MAX_LOADED_REWARD_REQUESTS)
    {
        return;
    }
    const auto token = this->moderationToken("reward requests");
    if (token.isEmpty())
    {
        return;
    }

    const auto generation = this->requestGeneration_;
    const auto requestedCursor = this->nextCursor_;
    this->pageLoading_ = true;
    this->loadMoreButton_->setEnabled(false);
    this->updateActionButtons();
    if (this->requestModel_->rowCount() == 0)
    {
        this->setStatus(QStringLiteral("Loading requests..."));
    }
    QPointer<RewardRequestQueueDialog> self(this);
    TwitchGql::getRewardRequests(
        this->channelLogin_, this->channelId_, this->selectedRewardId_,
        requestedCursor, token,
        [self, generation, requestedCursor](GqlRewardRequestPage page) mutable {
            if (!self || generation != self->requestGeneration_)
            {
                return;
            }
            self->pageLoading_ = false;
            self->pageLoadFailed_ = false;
            QVector<GqlRewardRequest> unique;
            unique.reserve(page.requests.size());
            for (auto &request : page.requests)
            {
                if (!self->seenRequestIds_.contains(request.id))
                {
                    self->seenRequestIds_.insert(request.id);
                    unique.push_back(std::move(request));
                }
            }
            const int availableRows =
                MAX_LOADED_REWARD_REQUESTS - self->requestModel_->rowCount();
            if (unique.size() > availableRows)
            {
                unique.resize(availableRows);
            }
            self->requestModel_->append(std::move(unique));
            const bool cursorAdvanced = !page.nextCursor.isEmpty() &&
                                        page.nextCursor != requestedCursor;
            self->nextCursor_ = cursorAdvanced ? page.nextCursor : QString();
            self->hasNextPage_ =
                page.hasNextPage && cursorAdvanced &&
                self->requestModel_->rowCount() < MAX_LOADED_REWARD_REQUESTS;
            self->loadMoreButton_->setVisible(self->hasNextPage_);
            self->loadMoreButton_->setEnabled(self->hasNextPage_);
            const auto loaded = self->requestModel_->rowCount();
            self->loadedLabel_->setText(
                self->hasNextPage_ ? QStringLiteral("%1 loaded")
                                         .arg(QLocale().toString(loaded))
                : loaded == 1 ? QStringLiteral("1 request")
                              : QStringLiteral("%1 requests")
                                    .arg(QLocale().toString(loaded)));
            if (!self->refreshNotice_.isEmpty())
            {
                self->setStatus(self->refreshNotice_,
                                self->refreshNoticeIsError_);
                self->refreshNotice_.clear();
                self->refreshNoticeIsError_ = false;
            }
            else if (page.hasNextPage && cursorAdvanced &&
                     self->requestModel_->rowCount() >=
                         MAX_LOADED_REWARD_REQUESTS)
            {
                self->setStatus(
                    self->selectedRewardId_.isEmpty()
                        ? QStringLiteral("Showing the first 1,000 requests. "
                                         "Choose a reward to narrow the queue.")
                        : QStringLiteral(
                              "Showing the first 1,000 requests for this "
                              "reward. Resolve some, then refresh to "
                              "continue."));
            }
            else
            {
                self->setStatus({});
            }
            if (self->selectFirstRequestAfterReload_ &&
                self->requestModel_->rowCount() > 0)
            {
                self->selectFirstRequestAfterReload_ = false;
                self->requests_->selectRow(0);
            }
            else if (!self->hasNextPage_ &&
                     self->requestModel_->rowCount() == 0)
            {
                self->selectFirstRequestAfterReload_ = false;
            }
            self->updateSelectionPreview();
            self->updateActionButtons();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->requestGeneration_)
            {
                return;
            }
            self->pageLoading_ = false;
            self->pageLoadFailed_ = true;
            self->selectFirstRequestAfterReload_ = false;
            self->loadMoreButton_->setEnabled(true);
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "loading reward requests", error);
            const auto message =
                self->refreshNotice_.isEmpty()
                    ? normalized
                    : self->refreshNotice_ + QLatin1Char(' ') + normalized;
            self->refreshNotice_.clear();
            self->refreshNoticeIsError_ = false;
            self->setStatus(message, true);
            self->updateSelectionPreview();
            self->updateActionButtons();
        });
}

void RewardRequestQueueDialog::updateSummary()
{
    auto count = QLocale().toString(this->overview_.totalPendingCount);
    if (this->overview_.countAtMaximum)
    {
        count += QLatin1Char('+');
    }
    this->countLabel_->setText(
        this->overview_.totalPendingCount == 1
            ? QStringLiteral("1 open request")
            : QStringLiteral("%1 open requests").arg(count));
}

void RewardRequestQueueDialog::updateSelectionPreview()
{
    const auto selection = this->requests_->selectionModel()->selectedRows();
    if (selection.size() == 1)
    {
        const auto *request =
            this->requestModel_->requestAt(selection.front().row());
        if (request != nullptr)
        {
            this->selectionTitle_->setText(
                QStringLiteral("%1  •  %2")
                    .arg(queueUserName(request->user), request->rewardTitle));
            this->selectionTitle_->setToolTip(
                request->user.login.isEmpty()
                    ? request->rewardTitle
                    : QStringLiteral("@%1\n%2").arg(request->user.login,
                                                    request->rewardTitle));
            this->selectionMeta_->setText(
                relativeQueueTime(request->timestamp));
            this->selectionMeta_->setToolTip(
                localQueueTime(request->timestamp));
            const auto message = request->input.trimmed();
            setDialogLinkLabelText(
                this->selectionMessage_,
                message.isEmpty() ? QStringLiteral("No message") : message);
            this->selectionMessage_->setToolTip(message);
            return;
        }
    }

    this->selectionMeta_->clear();
    this->selectionMeta_->setToolTip({});
    this->selectionTitle_->setToolTip({});
    this->selectionMessage_->setToolTip({});
    if (selection.size() > 1)
    {
        this->selectionTitle_->setText(
            QStringLiteral("%1 requests selected")
                .arg(QLocale().toString(selection.size())));
        setDialogLinkLabelText(
            this->selectionMessage_,
            QStringLiteral("Actions apply only to the selected requests."));
    }
    else if (this->requestModel_->rowCount() > 0)
    {
        this->selectionTitle_->setText(
            this->selectedRewardId_.isEmpty()
                ? QStringLiteral("All pending requests")
                : QStringLiteral("All requests for this reward"));
        setDialogLinkLabelText(
            this->selectionMessage_,
            QStringLiteral(
                "Select rows to act on specific requests instead."));
    }
    else
    {
        this->selectionTitle_->setText(QStringLiteral("No pending requests"));
        setDialogLinkLabelText(
            this->selectionMessage_,
            QStringLiteral("Refresh to check for new requests."));
    }
}

void RewardRequestQueueDialog::updateActionButtons()
{
    const auto selection = this->requests_->selectionModel()->selectedRows();
    const int selected = selection.size();
    int visibleCount = this->overview_.totalPendingCount;
    if (!this->selectedRewardId_.isEmpty())
    {
        visibleCount = 0;
        for (const auto &reward : this->overview_.rewards)
        {
            if (reward.id == this->selectedRewardId_)
            {
                visibleCount = reward.pendingCount;
                break;
            }
        }
    }
    const bool hasAny = visibleCount > 0 && this->requestModel_->rowCount() > 0;
    const bool hasSafeScope = selected > 0 || !this->pageLoadFailed_;
    const bool enabled = !this->overviewLoading_ && !this->pageLoading_ &&
                         !this->actionInFlight_ && hasAny && hasSafeScope;
    this->completeButton_->setEnabled(enabled);
    this->rejectButton_->setEnabled(enabled);
    this->completeButton_->setText(
        selected == 1  ? QStringLiteral("Complete")
        : selected > 1 ? QStringLiteral("Complete %1").arg(selected)
                       : QStringLiteral("Complete all"));
    this->rejectButton_->setText(
        selected == 1  ? QStringLiteral("Reject and refund")
        : selected > 1 ? QStringLiteral("Reject %1 and refund").arg(selected)
                       : QStringLiteral("Reject all and refund"));
    const auto scope =
        selected > 0 ? QStringLiteral("the selected requests")
        : this->selectedRewardId_.isEmpty()
            ? QStringLiteral("every pending request")
            : QStringLiteral("every pending request for this reward");
    this->completeButton_->setToolTip(QStringLiteral("Complete %1").arg(scope));
    this->rejectButton_->setToolTip(
        QStringLiteral("Reject %1 and refund the Channel Points").arg(scope));
}

void RewardRequestQueueDialog::resolveRequests(
    GqlRewardRequestResolution resolution)
{
    if (this->actionInFlight_ || this->overviewLoading_ || this->pageLoading_ ||
        !this->completeButton_->isEnabled())
    {
        return;
    }

    const auto authGeneration = this->authGeneration_;
    const auto requestGeneration = this->requestGeneration_;
    const auto rewardId = this->selectedRewardId_;
    QStringList ids;
    const auto selectedRows = this->requests_->selectionModel()->selectedRows();
    for (const auto &index : selectedRows)
    {
        if (const auto *request = this->requestModel_->requestAt(index.row()))
        {
            ids.push_back(request->id);
        }
    }

    const bool all = ids.isEmpty();
    const bool rejecting =
        resolution == GqlRewardRequestResolution::RejectAndRefund;
    QString countText;
    if (all)
    {
        if (this->selectedRewardId_.isEmpty())
        {
            countText = QStringLiteral("every pending reward request in #%1")
                            .arg(this->channelLogin_);
        }
        else
        {
            const auto title = this->rewards_->currentItem()
                                   ? this->rewards_->currentItem()
                                         ->data(RewardTitleRole)
                                         .toString()
                                   : QStringLiteral("this reward");
            countText =
                QStringLiteral("every pending request for %1").arg(title);
        }
    }
    else
    {
        countText =
            ids.size() == 1
                ? QStringLiteral("the selected request")
                : QStringLiteral("the %1 selected requests").arg(ids.size());
    }
    if (all || rejecting)
    {
        const auto title = rejecting ? QStringLiteral("Reject requests")
                                     : QStringLiteral("Complete requests");
        const auto text =
            rejecting
                ? QStringLiteral("Reject %1 and refund their Channel Points?")
                      .arg(countText)
                : QStringLiteral("Complete %1?").arg(countText);
        const QPointer<RewardRequestQueueDialog> self(this);
        QPointer<QMessageBox> confirmation = new QMessageBox(
            QMessageBox::Question, title, text, QMessageBox::NoButton, this);
        auto *actionButton = confirmation->addButton(
            rejecting ? QStringLiteral("Reject and refund")
                      : QStringLiteral("Complete all"),
            rejecting ? QMessageBox::DestructiveRole : QMessageBox::AcceptRole);
        auto *cancelButton = confirmation->addButton(QMessageBox::Cancel);
        confirmation->setDefaultButton(cancelButton);
        confirmation->setEscapeButton(cancelButton);
        installMoltorinoDialogTheme(confirmation);
        confirmation->exec();
        if (!self || !confirmation)
        {
            return;
        }
        const bool confirmed = confirmation->clickedButton() == actionButton;
        delete confirmation;
        if (!confirmed)
        {
            return;
        }
    }

    if (authGeneration != this->authGeneration_ ||
        requestGeneration != this->requestGeneration_ ||
        rewardId != this->selectedRewardId_)
    {
        this->setStatus(QStringLiteral("The queue or account changed. Review "
                                       "the requests and try again."),
                        true);
        return;
    }
    const auto token = this->moderationToken("updating reward requests");
    if (token.isEmpty())
    {
        return;
    }

    this->actionInFlight_ = true;
    this->updateActionButtons();
    this->refreshButton_->setEnabled(false);
    const auto verb =
        rejecting ? QStringLiteral("Rejecting") : QStringLiteral("Completing");
    this->setStatus(QStringLiteral("%1 %2...").arg(verb, countText));
    const QSet<QString> resolvedIds(ids.cbegin(), ids.cend());
    QPointer<RewardRequestQueueDialog> self(this);
    const auto success = [self, resolvedIds, rejecting, all] {
        if (!self)
        {
            return;
        }
        self->actionInFlight_ = false;
        self->refreshButton_->setEnabled(true);
        if (all)
        {
            self->requestModel_->clear();
        }
        else
        {
            self->requestModel_->removeIds(resolvedIds);
            self->selectFirstRequestAfterReload_ = true;
        }
        const auto message =
            rejecting ? QStringLiteral(
                            "Reward requests rejected and points refunded.")
                      : QStringLiteral("Reward requests completed.");
        self->publishResult(message);
        self->reloadOverview(message);
    };
    const auto failure = [self](const QString &error, int completedCount) {
        if (!self)
        {
            return;
        }
        self->actionInFlight_ = false;
        self->refreshButton_->setEnabled(true);
        const auto normalized = MoltorinoAuth::normalizeAuthError(
            "updating reward requests", error);
        if (completedCount > 0)
        {
            self->requestModel_->clear();
            self->updateSelectionPreview();
            self->reloadOverview(
                QStringLiteral("%1 requests were updated before Twitch stopped "
                               "the action. %2")
                    .arg(QLocale().toString(completedCount), normalized),
                true);
        }
        else
        {
            self->setStatus(normalized, true);
            self->updateActionButtons();
        }
    };

    if (all)
    {
        TwitchGql::updateAllRewardRequests(this->channelId_, rewardId,
                                           resolution, token, success,
                                           [failure](const QString &error) {
                                               failure(error, 0);
                                           });
    }
    else
    {
        this->resolveSelectedBatches(std::move(ids), resolution, token, success,
                                     failure, authGeneration);
    }
}

void RewardRequestQueueDialog::resolveSelectedBatches(
    QStringList remaining, GqlRewardRequestResolution resolution,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &, int)> failureCallback,
    quint64 authGeneration, int completedCount)
{
    if (remaining.isEmpty())
    {
        successCallback();
        return;
    }

    if (authGeneration != this->authGeneration_)
    {
        failureCallback(QStringLiteral("Authorization changed. Remaining "
                                       "requests were not updated."),
                        completedCount);
        return;
    }

    const auto batch = remaining.mid(0, REWARD_UPDATE_BATCH_SIZE);
    remaining.remove(0, batch.size());
    const int completedAfterBatch = completedCount + batch.size();
    const auto currentFailure = [failureCallback,
                                 completedCount](const QString &error) {
        failureCallback(error, completedCount);
    };
    QPointer<RewardRequestQueueDialog> self(this);
    TwitchGql::updateRewardRequests(
        this->channelId_, batch, resolution, oauthToken,
        [self, remaining = std::move(remaining), resolution, oauthToken,
         authGeneration, completedAfterBatch,
         successCallback = std::move(successCallback),
         failureCallback = std::move(failureCallback)]() mutable {
            if (!self)
            {
                return;
            }
            self->resolveSelectedBatches(std::move(remaining), resolution,
                                         oauthToken, std::move(successCallback),
                                         std::move(failureCallback),
                                         authGeneration, completedAfterBatch);
        },
        currentFailure);
}

void RewardRequestQueueDialog::setStatus(const QString &text, bool error)
{
    this->statusIsError_ = error;
    this->statusLabel_->setText(text);
    this->statusLabel_->setToolTip(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    auto palette = this->palette();
    palette.setColor(QPalette::WindowText,
                     error ? palette.color(QPalette::BrightText)
                           : this->palette().color(QPalette::WindowText));
    this->statusLabel_->setPalette(palette);
}

void RewardRequestQueueDialog::applyTheme()
{
    applyMoltorinoDialogPalette(this, *getTheme());
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));
    this->setStatus(this->statusLabel_->text(), this->statusIsError_);
}

void RewardRequestQueueDialog::publishResult(const QString &text) const
{
    if (const auto channel = this->outputChannel_.lock())
    {
        channel->addSystemMessage(text);
    }
}

}

#endif
