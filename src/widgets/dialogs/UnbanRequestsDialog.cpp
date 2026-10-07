#include "widgets/dialogs/UnbanRequestsDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/IvrApi.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/DialogLinkText.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/Window.hpp"

#include <QAbstractListModel>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMessageBox>
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {

namespace {

constexpr int MAX_LOADED_UNBAN_REQUESTS = 20000;
constexpr int MAX_MODERATOR_NOTE_LENGTH = 500;
constexpr int MAX_CACHED_UNBAN_REQUESTS = 32;
constexpr qsizetype MAX_CHAT_HISTORY = 2000;
constexpr qsizetype MAX_COMMENTS = 500;
constexpr int SELECTED_AVATAR_SIZE = 64;
constexpr qint64 MAX_AVATAR_BYTES = 2 * 1024 * 1024;

QString unbanUserName(const GqlModeratorQueueUser &user)
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

QDateTime twitchDate(const QString &value)
{
    auto date = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!date.isValid())
    {
        date = QDateTime::fromString(value, Qt::ISODate);
    }
    return date;
}

QString shortRequestAge(const QString &timestamp)
{
    const auto time = twitchDate(timestamp);
    if (!time.isValid())
    {
        return QStringLiteral("Unknown date");
    }
    const auto seconds = time.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 60 * 60)
    {
        return QStringLiteral("%1 min ago")
            .arg(std::max<qint64>(1, seconds / 60));
    }
    if (seconds < 24 * 60 * 60)
    {
        return QStringLiteral("%1 hr ago").arg(seconds / (60 * 60));
    }
    if (seconds < 14 * 24 * 60 * 60)
    {
        const auto days = seconds / (24 * 60 * 60);
        return days == 1 ? QStringLiteral("1 day ago")
                         : QStringLiteral("%1 days ago").arg(days);
    }
    return QLocale().toString(time.toLocalTime().date(), QLocale::ShortFormat);
}

QString localDateTime(const QString &timestamp)
{
    const auto time = twitchDate(timestamp);
    return time.isValid()
               ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat)
               : QStringLiteral("Unknown");
}

QString localDate(const QString &timestamp)
{
    const auto time = twitchDate(timestamp);
    return time.isValid() ? QLocale().toString(time.toLocalTime().date(),
                                               QLocale::LongFormat)
                          : QStringLiteral("Unknown");
}

QString compactAge(const QString &timestamp)
{
    const auto followedAt = twitchDate(timestamp);
    const auto followedDate = followedAt.date();
    const auto today = QDateTime::currentDateTimeUtc().date();
    if (!followedDate.isValid() || followedDate > today)
    {
        return {};
    }

    auto months = (today.year() - followedDate.year()) * 12 + today.month() -
                  followedDate.month();
    if (today.day() < followedDate.day())
    {
        --months;
    }
    months = std::max(0, months);
    if (months >= 12)
    {
        const auto years = months / 12;
        const auto remainingMonths = months % 12;
        return remainingMonths == 0
                   ? QStringLiteral("%1y").arg(years)
                   : QStringLiteral("%1y %2m").arg(years).arg(remainingMonths);
    }
    if (months > 0)
    {
        return months == 1 ? QStringLiteral("1 month")
                           : QStringLiteral("%1 months").arg(months);
    }

    const auto days = followedDate.daysTo(today);
    if (days >= 14)
    {
        return QStringLiteral("%1w").arg(days / 7);
    }
    if (days > 0)
    {
        return days == 1 ? QStringLiteral("1 day")
                         : QStringLiteral("%1 days").arg(days);
    }
    return QStringLiteral("today");
}

QString subscriptionTier(QString tier)
{
    tier = tier.trimmed();
    bool ok = false;
    const auto numericTier = tier.toInt(&ok);
    if (ok && numericTier >= 1000 && numericTier % 1000 == 0)
    {
        tier = QString::number(numericTier / 1000);
    }
    return tier;
}

QString monthsText(int months)
{
    return months == 1 ? QStringLiteral("1 month")
                       : QStringLiteral("%1 months").arg(months);
}

QPixmap initialAvatar(const QString &name, int size)
{
    QPixmap avatar(size, size);
    avatar.fill(QColor::fromHsv(static_cast<int>(qHash(name.toLower()) % 360),
                                120, 185));
    QPainter painter(&avatar);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = makeResolvedFont(painter.font(), QFont::Bold);
    font.setPointSizeF(std::max(9.0, size * 0.34));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(avatar.rect(), Qt::AlignCenter,
                     name.trimmed().left(1).toUpper());
    return avatar;
}

QPixmap squareAvatar(const QByteArray &data, int size)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto sourceSize = reader.size();
    if (!sourceSize.isValid() || sourceSize.width() > 2048 ||
        sourceSize.height() > 2048)
    {
        return {};
    }
    reader.setScaledSize(
        sourceSize.scaled(size, size, Qt::KeepAspectRatioByExpanding));
    auto source = QPixmap::fromImage(reader.read());
    if (source.isNull())
    {
        return {};
    }
    source = source.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                           Qt::SmoothTransformation);
    const int x = std::max(0, (source.width() - size) / 2);
    const int y = std::max(0, (source.height() - size) / 2);
    return source.copy(x, y, size, size);
}

}

class UnbanRequestListModel final : public QAbstractListModel
{
public:
    explicit UnbanRequestListModel(QObject *parent)
        : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : this->requests_.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->requests_.size())
        {
            return {};
        }
        const auto &request = this->requests_.at(index.row());
        if (role == Qt::DisplayRole)
        {
            return unbanUserName(request.requester);
        }
        if (role == Qt::ToolTipRole)
        {
            return request.requesterMessage;
        }
        return {};
    }

    void clear()
    {
        this->beginResetModel();
        this->requests_.clear();
        this->endResetModel();
    }

    void append(QVector<GqlUnbanRequest> requests)
    {
        if (requests.isEmpty())
        {
            return;
        }
        const int first = this->requests_.size();
        this->beginInsertRows({}, first, first + requests.size() - 1);
        this->requests_.append(std::move(requests));
        this->endInsertRows();
    }

    int removeRequest(const QString &id)
    {
        const auto row = this->rowForId(id);
        if (row < 0)
        {
            return -1;
        }
        this->beginRemoveRows({}, row, row);
        this->requests_.removeAt(row);
        this->endRemoveRows();
        return row;
    }

    const GqlUnbanRequest *requestAt(int row) const
    {
        if (row < 0 || row >= this->requests_.size())
        {
            return nullptr;
        }
        return &this->requests_.at(row);
    }

    int rowForId(const QString &id) const
    {
        for (int row = 0; row < this->requests_.size(); ++row)
        {
            if (this->requests_.at(row).id == id)
            {
                return row;
            }
        }
        return -1;
    }

private:
    QVector<GqlUnbanRequest> requests_;
};

class UnbanRequestDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(), 48};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        auto *model = static_cast<const UnbanRequestListModel *>(index.model());
        const auto *request = model->requestAt(index.row());
        if (request == nullptr)
        {
            return;
        }

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
        const auto muted =
            selected ? foreground
                     : option.palette.color(QPalette::PlaceholderText);

        const int textLeft = option.rect.left() + 9;
        const int textWidth = option.rect.right() - textLeft - 10;
        QFont nameFont = makeResolvedFont(option.font, QFont::Bold);
        painter->setFont(nameFont);
        painter->setPen(foreground);
        painter->drawText(
            QRect(textLeft, option.rect.top() + 3, textWidth, 21),
            Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(nameFont).elidedText(unbanUserName(request->requester),
                                          Qt::ElideRight, textWidth));

        QFont detailFont = option.font;
        detailFont.setPointSizeF(std::max(7.0, detailFont.pointSizeF() - 1));
        painter->setFont(detailFont);
        painter->setPen(muted);
        const auto preview = request->requesterMessage.simplified();
        const auto line =
            QStringLiteral("%1  •  %2")
                .arg(
                    shortRequestAge(request->createdAt),
                    preview.isEmpty() ? QStringLiteral("No message") : preview);
        painter->drawText(
            QRect(textLeft, option.rect.top() + 23, textWidth, 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(detailFont)
                .elidedText(line, Qt::ElideRight, textWidth));

        painter->setPen(option.palette.mid().color());
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }
};

std::vector<QPointer<UnbanRequestsDialog>> UnbanRequestsDialog::activeDialogs_;

UnbanRequestsDialog::UnbanRequestsDialog(QString channelId,
                                         QString channelLogin,
                                         std::weak_ptr<Channel> outputChannel,
                                         QWidget *parent)
    : QDialog(parent)
    , channelId_(std::move(channelId))
    , channelLogin_(std::move(channelLogin))
    , outputChannel_(std::move(outputChannel))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(
        QStringLiteral("Unban requests in #%1").arg(this->channelLogin_));
    this->setPalette(moltorinoDialogPalette(*getTheme()));
    this->setAutoFillBackground(true);
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));
    this->resize(920, 620);
    this->setMinimumSize(780, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(7);

    auto *header = new QHBoxLayout;
    header->setSpacing(7);
    this->countLabel_ = new QLabel(QStringLiteral("0 open requests"), this);
    QFont countFont = makeResolvedFont(this->countLabel_->font(), QFont::Bold);
    this->countLabel_->setFont(countFont);
    header->addWidget(this->countLabel_);
    header->addStretch(1);
    header->addWidget(new QLabel(QStringLiteral("Sort:"), this));
    this->sort_ = new QComboBox(this);
    this->sort_->addItem(QStringLiteral("Newest first"), true);
    this->sort_->addItem(QStringLiteral("Oldest first"), false);
    header->addWidget(this->sort_);
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

    auto *queuePanel = new QFrame(splitter);
    queuePanel->setFrameShape(QFrame::NoFrame);
    queuePanel->setMinimumWidth(190);
    queuePanel->setMaximumWidth(300);
    auto *queueLayout = new QVBoxLayout(queuePanel);
    queueLayout->setContentsMargins(0, 0, 0, 0);
    queueLayout->setSpacing(0);
    this->queueStatusLabel_ = new QLabel(queuePanel);
    this->queueStatusLabel_->setWordWrap(true);
    this->queueStatusLabel_->setContentsMargins(10, 8, 10, 8);
    this->queueStatusLabel_->hide();
    queueLayout->addWidget(this->queueStatusLabel_);
    this->requestModel_ = new UnbanRequestListModel(this);
    this->requestList_ = new QListView(queuePanel);
    this->requestList_->setModel(this->requestModel_);
    this->requestList_->setItemDelegate(
        new UnbanRequestDelegate(this->requestList_));
    this->requestList_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->requestList_->setUniformItemSizes(true);
    this->requestList_->setMouseTracking(true);
    this->requestList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    queueLayout->addWidget(this->requestList_, 1);
    this->loadMoreRequests_ =
        new QPushButton(QStringLiteral("Load more requests"), queuePanel);
    this->loadMoreRequests_->setAutoDefault(false);
    this->loadMoreRequests_->hide();
    queueLayout->addWidget(this->loadMoreRequests_);
    splitter->addWidget(queuePanel);

    this->detailStack_ = new QStackedWidget(splitter);
    auto *empty = new QWidget(this->detailStack_);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->addStretch(1);
    auto *emptyTitle = new QLabel(QStringLiteral("No request selected"), empty);
    QFont emptyTitleFont = makeResolvedFont(emptyTitle->font(), QFont::Bold);
    emptyTitle->setFont(emptyTitleFont);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    auto *emptyHint =
        new QLabel(QStringLiteral("Choose a request from the queue to review it."),
                   empty);
    emptyHint->setObjectName(QStringLiteral("SecondaryText"));
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addStretch(1);
    this->detailStack_->addWidget(empty);

    auto *detail = new QWidget(this->detailStack_);
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(10, 0, 0, 0);
    detailLayout->setSpacing(7);

    auto *identity = new QHBoxLayout;
    identity->setSpacing(9);
    this->avatar_ = new QLabel(detail);
    this->avatar_->setFixedSize(SELECTED_AVATAR_SIZE, SELECTED_AVATAR_SIZE);
    auto *avatarColumn = new QVBoxLayout;
    avatarColumn->setContentsMargins(0, 3, 0, 0);
    avatarColumn->setSpacing(0);
    avatarColumn->addWidget(this->avatar_, 0, Qt::AlignTop);
    avatarColumn->addStretch(1);
    identity->addLayout(avatarColumn);
    auto *identityText = new QVBoxLayout;
    identityText->setSpacing(2);
    auto *nameRow = new QHBoxLayout;
    nameRow->setSpacing(7);
    this->displayName_ = new QLabel(detail);
    QFont nameFont = makeResolvedFont(this->displayName_->font(), QFont::Bold);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 2);
    this->displayName_->setFont(nameFont);
    nameRow->addWidget(this->displayName_);
    this->login_ = new QLabel(detail);
    this->login_->setObjectName(QStringLiteral("SecondaryText"));
    nameRow->addWidget(this->login_);
    nameRow->addStretch(1);
    identityText->addLayout(nameRow);
    this->accountCreated_ = new QLabel(detail);
    this->banStatus_ = new QLabel(detail);
    this->historyCounts_ = new QLabel(detail);
    this->accountCreated_->setObjectName(QStringLiteral("SecondaryText"));
    this->historyCounts_->setObjectName(QStringLiteral("SecondaryText"));
    this->historyCounts_->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
    identityText->addWidget(this->accountCreated_);
    identityText->addWidget(this->banStatus_);
    identityText->addWidget(this->historyCounts_);
    identity->addLayout(identityText, 1);
    detailLayout->addLayout(identity);

    auto *requestFrame = new QFrame(detail);
    requestFrame->setObjectName(QStringLiteral("RequestCard"));
    requestFrame->setFrameShape(QFrame::StyledPanel);
    requestFrame->setBackgroundRole(QPalette::Base);
    requestFrame->setAutoFillBackground(true);
    auto *requestLayout = new QVBoxLayout(requestFrame);
    requestLayout->setContentsMargins(10, 8, 10, 8);
    requestLayout->setSpacing(4);
    auto *requestHeader = new QHBoxLayout;
    auto *requestTitle =
        new QLabel(QStringLiteral("Unban request"), requestFrame);
    QFont sectionFont = makeResolvedFont(requestTitle->font(), QFont::Bold);
    requestTitle->setFont(sectionFont);
    requestHeader->addWidget(requestTitle);
    requestHeader->addStretch(1);
    this->requestDate_ = new QLabel(requestFrame);
    this->requestDate_->setObjectName(QStringLiteral("SecondaryText"));
    requestHeader->addWidget(this->requestDate_);
    requestLayout->addLayout(requestHeader);
    this->requestText_ = new QLabel(requestFrame);
    this->requestText_->setWordWrap(true);
    configureDialogLinkLabel(this->requestText_);
    requestLayout->addWidget(this->requestText_);
    detailLayout->addWidget(requestFrame);

    this->detailTabs_ = new QTabWidget(detail);
    auto *chatPage = new QWidget(this->detailTabs_);
    auto *chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(6, 6, 6, 6);
    this->chatHistory_ = new QTableWidget(chatPage);
    this->chatHistory_->setColumnCount(2);
    this->chatHistory_->setItemDelegateForColumn(
        1, new DialogLinkTextDelegate(this->chatHistory_));
    this->chatHistory_->setHorizontalHeaderLabels(
        {QStringLiteral("Time"), QStringLiteral("Message")});
    this->chatHistory_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    this->chatHistory_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    this->chatHistory_->horizontalHeader()->setHighlightSections(false);
    this->chatHistory_->verticalHeader()->hide();
    this->chatHistory_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->chatHistory_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->chatHistory_->setAlternatingRowColors(true);
    this->chatHistory_->setMouseTracking(true);
    this->chatHistory_->setVerticalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    chatLayout->addWidget(this->chatHistory_, 1);
    this->detailTabs_->addTab(chatPage, QStringLiteral("Chat history"));

    auto *commentsPage = new QWidget(this->detailTabs_);
    auto *commentsLayout = new QVBoxLayout(commentsPage);
    commentsLayout->setContentsMargins(6, 6, 6, 6);
    this->commentsTable_ = new QTableWidget(commentsPage);
    this->commentsTable_->setColumnCount(3);
    this->commentsTable_->setItemDelegateForColumn(
        2, new DialogLinkTextDelegate(this->commentsTable_));
    this->commentsTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Time"), QStringLiteral("Moderator"),
         QStringLiteral("Comment")});
    this->commentsTable_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    this->commentsTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    this->commentsTable_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    this->commentsTable_->horizontalHeader()->setHighlightSections(false);
    this->commentsTable_->verticalHeader()->hide();
    this->commentsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->commentsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->commentsTable_->setAlternatingRowColors(true);
    this->commentsTable_->setMouseTracking(true);
    commentsLayout->addWidget(this->commentsTable_, 1);
    this->loadOlderComments_ =
        new QPushButton(QStringLiteral("Load older comments"), commentsPage);
    this->loadOlderComments_->setAutoDefault(false);
    this->loadOlderComments_->hide();
    commentsLayout->addWidget(this->loadOlderComments_, 0, Qt::AlignLeft);
    this->detailTabs_->addTab(commentsPage, QStringLiteral("Mod comments"));
    detailLayout->addWidget(this->detailTabs_, 1);

    auto *notePanel = new QFrame(detail);
    notePanel->setObjectName(QStringLiteral("NotePanel"));
    notePanel->setFrameShape(QFrame::StyledPanel);
    notePanel->setBackgroundRole(QPalette::Base);
    notePanel->setAutoFillBackground(true);
    auto *noteLayout = new QVBoxLayout(notePanel);
    noteLayout->setContentsMargins(8, 7, 8, 7);
    noteLayout->setSpacing(5);
    auto *noteHeader = new QHBoxLayout;
    auto *noteLabel = new QLabel(QStringLiteral("Moderator note"), notePanel);
    noteHeader->addWidget(noteLabel);
    noteHeader->addStretch(1);
    this->noteCount_ = new QLabel(QStringLiteral("0 / 500"), notePanel);
    this->noteCount_->setObjectName(QStringLiteral("SecondaryText"));
    noteHeader->addWidget(this->noteCount_);
    noteLayout->addLayout(noteHeader);
    this->moderatorNote_ = new QPlainTextEdit(notePanel);
    this->moderatorNote_->setPlaceholderText(
        QStringLiteral("Optional note for the moderation log"));
    this->moderatorNote_->setFixedHeight(56);
    noteLayout->addWidget(this->moderatorNote_);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(7);
    this->detailStatus_ = new QLabel(notePanel);
    this->detailStatus_->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    actions->addWidget(this->detailStatus_, 1);
    this->denyButton_ =
        new QPushButton(QStringLiteral("Deny request"), notePanel);
    this->denyButton_->setObjectName(QStringLiteral("DenyButton"));
    this->denyButton_->setAutoDefault(false);
    this->approveButton_ =
        new QPushButton(QStringLiteral("Unban user"), notePanel);
    this->approveButton_->setObjectName(QStringLiteral("ApproveButton"));
    this->approveButton_->setAutoDefault(false);
    actions->addWidget(this->denyButton_);
    actions->addWidget(this->approveButton_);
    noteLayout->addLayout(actions);
    detailLayout->addWidget(notePanel);

    this->detailStack_->addWidget(detail);
    splitter->addWidget(this->detailStack_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({225, 695});
    root->addWidget(splitter, 1);

    this->setStyleSheet(moltorinoDialogStyleSheet() + QStringLiteral(R"(
        QPushButton#ApproveButton:hover:enabled {
            background: #2f7d4a;
            color: white;
        }
        QPushButton#DenyButton:hover:enabled {
            background: #a53b43;
            color: white;
        }
    )"));

    QObject::connect(this->refreshButton_, &QPushButton::clicked, this, [this] {
        this->loadFirstPage();
    });
    QObject::connect(this->sort_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this] {
                         this->loadFirstPage();
                     });
    QObject::connect(this->loadMoreRequests_, &QPushButton::clicked, this,
                     [this] {
                         this->loadNextPage();
                     });
    QObject::connect(this->requestList_->selectionModel(),
                     &QItemSelectionModel::currentChanged, this, [this] {
                         this->selectCurrentRequest();
                     });
    QObject::connect(this->requestList_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         const auto *bar =
                             this->requestList_->verticalScrollBar();
                         if (value >= bar->maximum() - 64)
                         {
                             this->loadNextPage();
                         }
                     });
    QObject::connect(
        this->detailTabs_, &QTabWidget::currentChanged, this,
        [this](int index) {
            const auto *cache = this->currentCache();
            if (index == 1 && cache != nullptr && !cache->commentsLoaded)
            {
                this->loadComments();
            }
        });
    QObject::connect(
        this->chatHistory_->verticalScrollBar(), &QScrollBar::valueChanged,
        this, [this](int value) {
            const auto *bar = this->chatHistory_->verticalScrollBar();
            const auto *cache = this->currentCache();
            if (this->rebuildingChatHistory_ ||
                cache == nullptr || cache->chatLoading ||
                !cache->chatHasNextPage || bar->maximum() <= bar->minimum() ||
                value > bar->minimum() + 24)
            {
                return;
            }
            this->loadChatHistory(true);
        });
    QObject::connect(this->loadOlderComments_, &QPushButton::clicked, this,
                     [this] {
                         this->loadComments(true);
                     });
    QObject::connect(this->approveButton_, &QPushButton::clicked, this, [this] {
        this->resolveCurrentRequest(true);
    });
    QObject::connect(this->denyButton_, &QPushButton::clicked, this, [this] {
        this->resolveCurrentRequest(false);
    });
    QObject::connect(this->moderatorNote_, &QPlainTextEdit::textChanged, this,
                     [this] {
                         const auto text = this->moderatorNote_->toPlainText();
                         if (text.size() <= MAX_MODERATOR_NOTE_LENGTH)
                         {
                             return;
                         }
                         const auto cursor = this->moderatorNote_->textCursor();
                         auto trimmed = text.left(MAX_MODERATOR_NOTE_LENGTH);
                         if (trimmed.back().isHighSurrogate())
                         {
                             trimmed.chop(1);
                         }
                         this->moderatorNote_->setPlainText(trimmed);
                         auto restored = cursor;
                         restored.setPosition(trimmed.size());
                         this->moderatorNote_->setTextCursor(restored);
                     });
    QObject::connect(
        this->moderatorNote_, &QPlainTextEdit::textChanged, this, [this] {
            const auto text = this->moderatorNote_->toPlainText();
            this->noteCount_->setText(
                QStringLiteral("%1 / %2")
                    .arg(QLocale().toString(text.size()),
                         QLocale().toString(MAX_MODERATOR_NOTE_LENGTH)));
            if (this->selectedRequestId_.isEmpty())
            {
                return;
            }
            if (text.isEmpty())
            {
                this->moderatorNoteDrafts_.remove(this->selectedRequestId_);
            }
            else
            {
                this->moderatorNoteDrafts_.insert(this->selectedRequestId_,
                                                  text);
            }
        });

    QWidget::setTabOrder(this->refreshButton_, this->sort_);
    QWidget::setTabOrder(this->sort_, this->requestList_);
    QWidget::setTabOrder(this->requestList_, this->detailTabs_);
    QWidget::setTabOrder(this->detailTabs_, this->moderatorNote_);
    QWidget::setTabOrder(this->moderatorNote_, this->denyButton_);
    QWidget::setTabOrder(this->denyButton_, this->approveButton_);
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
        ++this->queueGeneration_;
        this->queueLoading_ = false;
        this->requestModel_->clear();
        this->detailCache_.clear();
        this->detailCacheOrder_.clear();
        this->seenRequestIds_.clear();
        this->totalRequests_ = 0;
        this->clearRequest();
        this->updateQueueSummary();
        if (!this->actionInFlight_)
        {
            this->loadFirstPage();
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

    for (auto *label : {this->countLabel_, this->queueStatusLabel_,
                        this->displayName_, this->login_, this->accountCreated_,
                        this->banStatus_, this->historyCounts_, this->requestDate_,
                        this->detailStatus_, this->noteCount_})
    {
        label->setTextFormat(Qt::PlainText);
    }
    this->clearRequest();
    this->loadFirstPage();
}

void UnbanRequestsDialog::showDialog(const QString &channelId,
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
    auto *dialog = new UnbanRequestsDialog(channelId, channelLogin,
                                           std::move(outputChannel), parent);
    activeDialogs_.push_back(dialog);
    dialog->show();
}

QString UnbanRequestsDialog::moderationToken(const QString &action)
{
    QString error;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channelId_, this->channelLogin_, &error);
    if (!auth.hasToken())
    {
        this->setQueueStatus(error.isEmpty()
                                 ? MoltorinoAuth::authRequiredMessage(action)
                                 : error,
                             true);
        return {};
    }
    return auth.token;
}

void UnbanRequestsDialog::loadFirstPage()
{
    if (this->actionInFlight_)
    {
        return;
    }
    this->preferredRequestId_ = this->selectedRequestId_;
    ++this->queueGeneration_;
    this->queueLoading_ = false;
    this->requestModel_->clear();
    this->seenRequestIds_.clear();
    this->detailCache_.clear();
    this->detailCacheOrder_.clear();
    this->nextCursor_.clear();
    this->totalRequests_ = 0;
    this->hasNextPage_ = true;
    this->clearRequest();
    this->updateQueueSummary();
    this->loadNextPage();
}

void UnbanRequestsDialog::loadNextPage()
{
    if (this->queueLoading_ || !this->hasNextPage_ || this->actionInFlight_ ||
        this->requestModel_->rowCount() >= MAX_LOADED_UNBAN_REQUESTS)
    {
        return;
    }
    const auto token = this->moderationToken("unban requests");
    if (token.isEmpty())
    {
        return;
    }

    const int generation = this->queueGeneration_;
    const auto requestedCursor = this->nextCursor_;
    const bool newestFirst = this->sort_->currentData().toBool();
    this->queueLoading_ = true;
    this->refreshButton_->setEnabled(false);
    this->sort_->setEnabled(false);
    this->loadMoreRequests_->setEnabled(false);
    if (this->requestModel_->rowCount() == 0)
    {
        this->setQueueStatus(QStringLiteral("Loading requests..."));
    }
    QPointer<UnbanRequestsDialog> self(this);
    TwitchGql::getUnbanRequests(
        this->channelLogin_, this->channelId_, requestedCursor, newestFirst,
        token,
        [self, generation, requestedCursor](GqlUnbanRequestPage page) mutable {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            self->queueLoading_ = false;
            self->refreshButton_->setEnabled(true);
            self->sort_->setEnabled(true);
            self->totalRequests_ = page.totalCount;
            QVector<GqlUnbanRequest> unique;
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
                MAX_LOADED_UNBAN_REQUESTS - self->requestModel_->rowCount();
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
                self->requestModel_->rowCount() < MAX_LOADED_UNBAN_REQUESTS;
            self->loadMoreRequests_->setVisible(self->hasNextPage_);
            self->loadMoreRequests_->setEnabled(self->hasNextPage_);
            self->updateQueueSummary();
            const int preferredRow =
                self->requestModel_->rowForId(self->preferredRequestId_);
            if (preferredRow >= 0)
            {
                self->requestList_->setCurrentIndex(
                    self->requestModel_->index(preferredRow, 0));
                self->preferredRequestId_.clear();
            }
            else if (requestedCursor.isEmpty())
            {
                self->preferredRequestId_.clear();
            }
            if (page.hasNextPage && cursorAdvanced &&
                self->requestModel_->rowCount() >= MAX_LOADED_UNBAN_REQUESTS)
            {
                self->setQueueStatus(
                    QStringLiteral("Showing the first 20,000 requests."));
            }
            else if (self->requestModel_->rowCount() == 0)
            {
                self->setQueueStatus(
                    page.isEnabled
                        ? QStringLiteral("No open unban requests.")
                        : QStringLiteral(
                              "Unban requests are disabled for this channel."));
            }
            else
            {
                self->setQueueStatus({});
                if (!self->requestList_->currentIndex().isValid())
                {
                    self->requestList_->setCurrentIndex(
                        self->requestModel_->index(0, 0));
                }
            }
            if (!self->hasNextPage_)
            {
                self->preferredRequestId_.clear();
            }
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            self->queueLoading_ = false;
            self->refreshButton_->setEnabled(true);
            self->sort_->setEnabled(true);
            self->loadMoreRequests_->setEnabled(true);
            self->setQueueStatus(MoltorinoAuth::normalizeAuthError(
                                     "loading unban requests", error),
                                 true);
        });
}

void UnbanRequestsDialog::selectCurrentRequest()
{
    const auto index = this->requestList_->currentIndex();
    const auto *request = this->requestModel_->requestAt(index.row());
    if (request == nullptr)
    {
        this->clearRequest();
        return;
    }
    this->showRequest(*request);
    this->prefetchNextRequest(index.row());
}

UnbanRequestsDialog::DetailCache &UnbanRequestsDialog::cacheFor(
    const QString &requestId)
{
    this->touchCache(requestId);
    return this->detailCache_[requestId];
}

UnbanRequestsDialog::DetailCache *UnbanRequestsDialog::currentCache()
{
    auto it = this->detailCache_.find(this->selectedRequestId_);
    return it == this->detailCache_.end() ? nullptr : &it.value();
}

void UnbanRequestsDialog::touchCache(const QString &requestId)
{
    if (requestId.isEmpty())
    {
        return;
    }
    if (!this->detailCache_.contains(requestId))
    {
        this->detailCache_.insert(requestId, DetailCache{});
    }
    this->detailCacheOrder_.removeAll(requestId);
    this->detailCacheOrder_.prepend(requestId);
    while (this->detailCache_.size() > MAX_CACHED_UNBAN_REQUESTS)
    {
        const auto oldest = this->detailCacheOrder_.takeLast();
        if (oldest == this->selectedRequestId_)
        {
            this->detailCacheOrder_.prepend(oldest);
            continue;
        }
        this->detailCache_.remove(oldest);
    }
}

void UnbanRequestsDialog::prefetchNextRequest(int currentRow)
{
    const auto *next = this->requestModel_->requestAt(currentRow + 1);
    if (next == nullptr)
    {
        return;
    }

    this->cacheFor(next->id);
    this->loadAvatar(next->id, next->requester.profileImageUrl,
                     unbanUserName(next->requester));
    this->loadUserContext(*next);
    this->loadChatHistoryFor(*next);
}

void UnbanRequestsDialog::showRequest(const GqlUnbanRequest &request)
{
    this->selectedRequestId_ = request.id;
    this->actionError_.clear();
    auto &cache = this->cacheFor(request.id);
    this->chatHistory_->setRowCount(0);
    this->commentsTable_->setRowCount(0);
    this->loadOlderComments_->hide();
    this->detailTabs_->setTabText(0, QStringLiteral("Chat history"));
    this->detailTabs_->setTabText(1, QStringLiteral("Mod comments"));
    this->detailTabs_->setCurrentIndex(0);
    this->moderatorNote_->setPlainText(
        this->moderatorNoteDrafts_.value(request.id));
    this->displayName_->setText(unbanUserName(request.requester));
    this->login_->setText(
        request.requester.login.isEmpty()
            ? QString()
            : QStringLiteral("@%1").arg(request.requester.login));
    this->accountCreated_->setText(
        QStringLiteral("Loading account details..."));
    this->banStatus_->clear();
    this->historyCounts_->clear();
    this->historyCounts_->setToolTip({});
    this->requestDate_->setText(shortRequestAge(request.createdAt));
    this->requestDate_->setToolTip(localDateTime(request.createdAt));
    setDialogLinkLabelText(
        this->requestText_,
        request.requesterMessage.trimmed().isEmpty()
            ? QStringLiteral("No message was included.")
            : request.requesterMessage.trimmed());
    this->avatar_->setPixmap(
        cache.avatar.isNull() ? initialAvatar(unbanUserName(request.requester),
                                              SELECTED_AVATAR_SIZE)
                              : cache.avatar);
    this->detailStack_->setCurrentIndex(1);
    this->refreshDetailStatus();
    this->updateActionState();

    if (cache.contextLoaded)
    {
        this->applyUserContext(cache.userContext);
        this->loadAvatar(request.id, cache.userContext.user.profileImageUrl,
                         unbanUserName(cache.userContext.user));
    }
    else
    {
        this->loadAvatar(request.id, request.requester.profileImageUrl,
                         unbanUserName(request.requester));
        this->loadUserContext(request);
    }
    this->loadRelationshipContext(request);

    if (cache.chatLoaded)
    {
        this->rebuildChatHistory();
    }
    else
    {
        this->loadChatHistory();
    }
    if (cache.commentsLoaded)
    {
        this->rebuildComments();
    }
}

void UnbanRequestsDialog::clearRequest()
{
    this->selectedRequestId_.clear();
    this->actionError_.clear();
    this->detailStack_->setCurrentIndex(0);
    this->updateActionState();
}

void UnbanRequestsDialog::applyUserContext(
    const GqlUnbanRequestUserContext &context)
{
    this->displayName_->setText(unbanUserName(context.user));
    this->login_->setText(context.user.login.isEmpty()
                              ? QString()
                              : QStringLiteral("@%1").arg(context.user.login));
    this->accountCreated_->setText(QStringLiteral("Account created %1")
                                       .arg(localDate(context.user.createdAt)));
    if (context.currentlyBanned)
    {
        auto banned =
            QStringLiteral("Banned %1").arg(localDateTime(context.bannedAt));
        if (!context.bannedByLogin.isEmpty())
        {
            banned += QStringLiteral(" by %1").arg(context.bannedByLogin);
        }
        this->banStatus_->setText(banned);
    }
    else
    {
        this->banStatus_->setText(QStringLiteral("No longer banned"));
    }
    this->updateViewerHistoryLine();
}

void UnbanRequestsDialog::loadUserContext(const GqlUnbanRequest &request)
{
    auto &cache = this->cacheFor(request.id);
    if (cache.contextLoaded)
    {
        if (request.id == this->selectedRequestId_)
        {
            this->applyUserContext(cache.userContext);
        }
        return;
    }
    if (cache.contextLoading)
    {
        return;
    }
    const auto token = this->moderationToken("reviewing an unban request");
    if (token.isEmpty())
    {
        return;
    }
    cache.contextLoading = true;
    cache.contextError.clear();
    if (request.id == this->selectedRequestId_)
    {
        this->refreshDetailStatus();
    }
    const int generation = this->queueGeneration_;
    QPointer<UnbanRequestsDialog> self(this);
    TwitchGql::getUnbanRequestUserContext(
        this->channelId_, request.requester.id, token,
        [self, generation,
         requestId = request.id](GqlUnbanRequestUserContext context) mutable {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            auto &cache = it.value();
            cache.contextLoading = false;
            cache.contextLoaded = true;
            cache.userContext = std::move(context);
            cache.contextError.clear();
            self->loadAvatar(requestId, cache.userContext.user.profileImageUrl,
                             unbanUserName(cache.userContext.user));
            if (requestId == self->selectedRequestId_)
            {
                self->applyUserContext(cache.userContext);
                self->refreshDetailStatus();
            }
        },
        [self, generation, requestId = request.id](const QString &error) {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            it->contextLoading = false;
            it->contextError = MoltorinoAuth::normalizeAuthError(
                "reviewing an unban request", error);
            if (requestId == self->selectedRequestId_)
            {
                self->accountCreated_->setText(
                    QStringLiteral("Account details unavailable"));
                self->refreshDetailStatus();
            }
        });
}

void UnbanRequestsDialog::loadRelationshipContext(
    const GqlUnbanRequest &request)
{
    auto &cache = this->cacheFor(request.id);
    if (cache.relationshipLoaded)
    {
        if (request.id == this->selectedRequestId_)
        {
            this->updateViewerHistoryLine();
        }
        return;
    }
    if (cache.relationshipLoading)
    {
        return;
    }

    const auto login = request.requester.login.trimmed();
    if (login.isEmpty())
    {
        cache.relationshipLoaded = true;
        cache.relationshipFailed = true;
        this->updateViewerHistoryLine();
        return;
    }

    cache.relationshipLoading = true;
    const int generation = this->queueGeneration_;
    QPointer<UnbanRequestsDialog> self(this);
    getIvr()->getSubage(
        login, this->channelLogin_,
        [self, generation, requestId = request.id](const IvrSubage &subage) {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }

            auto &cache = it.value();
            cache.relationship.followedAt = subage.followingSince;
            cache.relationship.subTier = subage.subTier;
            cache.relationship.giftSource = subage.giftSource;
            cache.relationship.totalSubMonths = subage.totalSubMonths;
            cache.relationship.isSubHidden = subage.isSubHidden;
            cache.relationship.isSubbed = subage.isSubbed;
            cache.relationship.isGifted = subage.isGifted;
            cache.relationship.giftIsAnonymous = subage.giftIsAnonymous;
            cache.relationshipLoading = false;
            cache.relationshipLoaded = true;
            cache.relationshipFailed = false;
            if (requestId == self->selectedRequestId_)
            {
                self->updateViewerHistoryLine();
            }
        },
        [self, generation, requestId = request.id] {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            it->relationshipLoading = false;
            it->relationshipLoaded = true;
            it->relationshipFailed = true;
            if (requestId == self->selectedRequestId_)
            {
                self->updateViewerHistoryLine();
            }
        });
}

void UnbanRequestsDialog::updateViewerHistoryLine()
{
    const auto *cache = this->currentCache();
    if (cache == nullptr)
    {
        this->historyCounts_->clear();
        this->historyCounts_->setToolTip({});
        return;
    }

    QStringList facts;
    QStringList details;
    if (cache->contextLoaded)
    {
        facts.push_back(
            QStringLiteral("%1 ban%2")
                .arg(QLocale().toString(cache->userContext.banCount))
                .arg(cache->userContext.banCount == 1 ? QString()
                                                      : QStringLiteral("s")));
        facts.push_back(
            QStringLiteral("%1 timeout%2")
                .arg(QLocale().toString(cache->userContext.timeoutCount))
                .arg(cache->userContext.timeoutCount == 1
                         ? QString()
                         : QStringLiteral("s")));
    }

    if (cache->relationshipLoaded)
    {
        if (cache->relationshipFailed)
        {
            facts.push_back(QStringLiteral("Follow and sub info unavailable"));
        }
        else
        {
            const auto followedAt = twitchDate(cache->relationship.followedAt);
            const auto age = compactAge(cache->relationship.followedAt);
            if (followedAt.isValid() && !age.isEmpty())
            {
                facts.push_back(QStringLiteral("Following %1").arg(age));
                details.push_back(
                    QStringLiteral("Followed on %1")
                        .arg(QLocale().toString(followedAt.toLocalTime().date(),
                                                QLocale::LongFormat)));
            }
            else
            {
                facts.push_back(QStringLiteral("Not following"));
            }

            const auto &relationship = cache->relationship;
            if (relationship.isSubHidden)
            {
                facts.push_back(QStringLiteral("Subscription hidden"));
            }
            else if (relationship.isSubbed)
            {
                const auto tier = subscriptionTier(relationship.subTier);
                auto subscription =
                    tier.isEmpty() ? QStringLiteral("Subscribed")
                                   : QStringLiteral("Tier %1 sub").arg(tier);
                if (relationship.totalSubMonths > 0)
                {
                    subscription += QStringLiteral(", %1").arg(
                        monthsText(relationship.totalSubMonths));
                }
                facts.push_back(subscription);

                if (relationship.isGifted)
                {
                    details.push_back(
                        relationship.giftIsAnonymous ||
                                relationship.giftSource.isEmpty()
                            ? QStringLiteral(
                                  "Current subscription was gifted anonymously")
                            : QStringLiteral(
                                  "Current subscription was gifted by %1")
                                  .arg(relationship.giftSource));
                }
            }
            else if (relationship.totalSubMonths > 0)
            {
                facts.push_back(
                    QStringLiteral("Past sub, %1")
                        .arg(monthsText(relationship.totalSubMonths)));
            }
            else
            {
                facts.push_back(QStringLiteral("Not subscribed"));
            }
        }
    }

    this->historyCounts_->setText(facts.join(QStringLiteral("  •  ")));
    this->historyCounts_->setToolTip(details.join(u'\n'));
}

void UnbanRequestsDialog::loadAvatar(const QString &requestId,
                                      const QString &url,
                                      const QString &fallbackName)
{
    const QUrl avatarUrl(url);
    if (!avatarUrl.isValid() || avatarUrl.scheme() != QStringLiteral("https") ||
        avatarUrl.host().isEmpty() || !avatarUrl.userInfo().isEmpty() ||
        avatarUrl.port(443) != 443)
    {
        return;
    }
    auto &cache = this->cacheFor(requestId);
    if (cache.avatarUrl == url &&
        (cache.avatarLoading || !cache.avatar.isNull()))
    {
        return;
    }
    cache.avatarUrl = url;
    cache.avatarLoading = true;
    const auto generation = this->queueGeneration_;
    NetworkRequest(avatarUrl)
        .caller(this)
        .timeout(10000)
        .maximumResponseSize(MAX_AVATAR_BYTES)
        .onSuccess([this, generation, requestId, url,
                    fallbackName](const NetworkResult &result) {
            if (generation != this->queueGeneration_)
            {
                return;
            }
            auto it = this->detailCache_.find(requestId);
            if (it == this->detailCache_.end() || it->avatarUrl != url)
            {
                return;
            }
            it->avatarLoading = false;
            auto avatar = squareAvatar(result.getData(), SELECTED_AVATAR_SIZE);
            if (avatar.isNull())
            {
                avatar = initialAvatar(fallbackName, SELECTED_AVATAR_SIZE);
            }
            it->avatar = avatar;
            if (requestId == this->selectedRequestId_)
            {
                this->avatar_->setPixmap(avatar);
            }
        })
        .onError([this, generation, requestId, url,
                  fallbackName](const NetworkResult &) {
            if (generation != this->queueGeneration_)
            {
                return;
            }
            auto it = this->detailCache_.find(requestId);
            if (it == this->detailCache_.end() || it->avatarUrl != url)
            {
                return;
            }
            it->avatarLoading = false;
            if (requestId == this->selectedRequestId_)
            {
                this->avatar_->setPixmap(
                    initialAvatar(fallbackName, SELECTED_AVATAR_SIZE));
            }
        })
        .execute();
}

void UnbanRequestsDialog::loadChatHistory(bool older)
{
    const auto index = this->requestList_->currentIndex();
    const auto *request = this->requestModel_->requestAt(index.row());
    if (request == nullptr || request->id != this->selectedRequestId_)
    {
        return;
    }
    this->loadChatHistoryFor(*request, older);
}

void UnbanRequestsDialog::loadChatHistoryFor(const GqlUnbanRequest &request,
                                             bool older)
{
    auto &cache = this->cacheFor(request.id);
    if (cache.chatLoading || (!older && cache.chatLoaded) ||
        (older && !cache.chatHasNextPage))
    {
        return;
    }
    const auto token = this->moderationToken("loading chat history");
    if (token.isEmpty())
    {
        return;
    }

    const int generation = this->queueGeneration_;
    const auto requestId = request.id;
    const auto cursor = older ? cache.chatCursor : QString();
    cache.chatLoading = true;
    cache.chatError.clear();
    if (request.id == this->selectedRequestId_)
    {
        this->refreshDetailStatus();
    }
    QPointer<UnbanRequestsDialog> self(this);
    TwitchGql::getUsercardMessagesBySender(
        this->channelId_, request.requester.id, cursor, token,
        [self, generation, requestId,
         cursor](GqlUsercardMessagePage page) mutable {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            auto &cache = it.value();
            cache.chatLoading = false;
            cache.chatLoaded = true;
            cache.chatError.clear();
            for (auto &message : page.messages)
            {
                if (cache.chatMessages.size() < MAX_CHAT_HISTORY &&
                    !cache.seenChatMessageIds.contains(message.id))
                {
                    cache.seenChatMessageIds.insert(message.id);
                    cache.chatMessages.push_back(std::move(message));
                }
            }
            const bool cursorAdvanced =
                !page.nextCursor.isEmpty() && page.nextCursor != cursor;
            cache.chatCursor = cursorAdvanced ? page.nextCursor : QString();
            cache.chatHasNextPage = page.hasNextPage && cursorAdvanced &&
                                    cache.chatMessages.size() < MAX_CHAT_HISTORY;
            if (requestId == self->selectedRequestId_)
            {
                self->refreshDetailStatus();
                self->rebuildChatHistory(!cursor.isEmpty());
            }
        },
        [self, generation, requestId](const QString &error) {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            it->chatLoading = false;
            it->chatError = MoltorinoAuth::normalizeAuthError(
                "loading chat history", error);
            if (requestId == self->selectedRequestId_)
            {
                self->refreshDetailStatus();
            }
        });
}

void UnbanRequestsDialog::rebuildChatHistory(bool preserveScrollPosition)
{
    auto *cache = this->currentCache();
    if (cache == nullptr)
    {
        return;
    }
    auto *bar = this->chatHistory_->verticalScrollBar();
    const int oldMaximum = bar->maximum();
    const int oldValue = bar->value();
    const auto requestId = this->selectedRequestId_;
    this->rebuildingChatHistory_ = true;
    this->chatHistory_->setSortingEnabled(false);
    this->chatHistory_->setRowCount(cache->chatMessages.size());
    int row = 0;
    for (auto it = cache->chatMessages.crbegin();
         it != cache->chatMessages.crend(); ++it, ++row)
    {
        auto *time = new QTableWidgetItem(localDateTime(it->sentAt));
        auto text = it->text;
        if (it->isDeleted)
        {
            text +=
                it->deletedBy.isEmpty()
                    ? QStringLiteral("  (deleted)")
                    : QStringLiteral("  (deleted by %1)").arg(it->deletedBy);
        }
        auto *message = new QTableWidgetItem(text);
        message->setToolTip(text);
        this->chatHistory_->setItem(row, 0, time);
        this->chatHistory_->setItem(row, 1, message);
    }
    this->detailTabs_->setTabText(
        0, QStringLiteral("Chat history (%1)")
               .arg(QLocale().toString(cache->chatMessages.size())));
    this->detailTabs_->setTabToolTip(
        0, cache->chatMessages.size() >= MAX_CHAT_HISTORY
               ? QStringLiteral("Showing the latest 2,000 messages.")
               : QString{});
    this->rebuildingChatHistory_ = false;

    QTimer::singleShot(
        0, this,
        [this, requestId, preserveScrollPosition, oldMaximum, oldValue] {
            if (requestId != this->selectedRequestId_)
            {
                return;
            }
            auto *scrollBar = this->chatHistory_->verticalScrollBar();
            if (preserveScrollPosition)
            {
                scrollBar->setValue(oldValue + scrollBar->maximum() -
                                    oldMaximum);
            }
            else
            {
                scrollBar->setValue(scrollBar->maximum());
            }
            const auto *cache = this->currentCache();
            if (cache != nullptr && cache->chatHasNextPage &&
                !cache->chatLoading &&
                scrollBar->maximum() <= scrollBar->minimum())
            {
                this->loadChatHistory(true);
            }
        });
}

void UnbanRequestsDialog::loadComments(bool older)
{
    const auto index = this->requestList_->currentIndex();
    const auto *request = this->requestModel_->requestAt(index.row());
    if (request == nullptr || request->id != this->selectedRequestId_)
    {
        return;
    }
    auto &cache = this->cacheFor(request->id);
    if (cache.commentsLoading || (!older && cache.commentsLoaded) ||
        (older && !cache.commentsHasNextPage))
    {
        return;
    }
    const auto token = this->moderationToken("loading moderator comments");
    if (token.isEmpty())
    {
        return;
    }

    const int generation = this->queueGeneration_;
    const auto requestId = request->id;
    const auto cursor = older ? cache.commentsCursor : QString();
    cache.commentsLoading = true;
    cache.commentsError.clear();
    if (request->id == this->selectedRequestId_)
    {
        this->refreshDetailStatus();
    }
    this->loadOlderComments_->setEnabled(false);
    QPointer<UnbanRequestsDialog> self(this);
    TwitchGql::getModeratorComments(
        this->channelId_, request->requester.id, cursor, token,
        [self, generation, requestId,
         cursor](GqlModeratorCommentPage page) mutable {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            auto &cache = it.value();
            cache.commentsLoading = false;
            cache.commentsLoaded = true;
            cache.commentsError.clear();
            for (auto &comment : page.comments)
            {
                if (cache.comments.size() < MAX_COMMENTS &&
                    !cache.seenCommentIds.contains(comment.id))
                {
                    cache.seenCommentIds.insert(comment.id);
                    cache.comments.push_back(std::move(comment));
                }
            }
            const bool cursorAdvanced =
                !page.nextCursor.isEmpty() && page.nextCursor != cursor;
            cache.commentsCursor = cursorAdvanced ? page.nextCursor : QString();
            cache.commentsHasNextPage = page.hasNextPage && cursorAdvanced &&
                                        cache.comments.size() < MAX_COMMENTS;
            if (requestId == self->selectedRequestId_)
            {
                self->refreshDetailStatus();
                self->rebuildComments();
            }
        },
        [self, generation, requestId](const QString &error) {
            if (!self || generation != self->queueGeneration_)
            {
                return;
            }
            auto it = self->detailCache_.find(requestId);
            if (it == self->detailCache_.end())
            {
                return;
            }
            it->commentsLoading = false;
            it->commentsError = MoltorinoAuth::normalizeAuthError(
                "loading moderator comments", error);
            if (requestId == self->selectedRequestId_)
            {
                self->loadOlderComments_->setEnabled(true);
                self->refreshDetailStatus();
            }
        });
}

void UnbanRequestsDialog::rebuildComments()
{
    auto *cache = this->currentCache();
    if (cache == nullptr)
    {
        return;
    }
    std::stable_sort(cache->comments.begin(), cache->comments.end(),
                     [](const auto &left, const auto &right) {
                         return twitchDate(left.timestamp) <
                                twitchDate(right.timestamp);
                     });
    this->commentsTable_->setRowCount(cache->comments.size());
    for (int row = 0; row < cache->comments.size(); ++row)
    {
        const auto &comment = cache->comments.at(row);
        auto *time = new QTableWidgetItem(localDateTime(comment.timestamp));
        auto author = comment.authorDisplayName;
        if (author.isEmpty())
        {
            author = comment.authorLogin;
        }
        auto *moderator = new QTableWidgetItem(author);
        auto *text = new QTableWidgetItem(comment.text);
        text->setToolTip(comment.text);
        this->commentsTable_->setItem(row, 0, time);
        this->commentsTable_->setItem(row, 1, moderator);
        this->commentsTable_->setItem(row, 2, text);
    }
    this->detailTabs_->setTabText(
        1, QStringLiteral("Mod comments (%1)")
               .arg(QLocale().toString(cache->comments.size())));
    this->detailTabs_->setTabToolTip(
        1, cache->comments.size() >= MAX_COMMENTS
               ? QStringLiteral("Showing the latest 500 comments.")
               : QString{});
    this->loadOlderComments_->setVisible(cache->commentsHasNextPage);
    this->loadOlderComments_->setEnabled(cache->commentsHasNextPage &&
                                         !cache->commentsLoading);
}

void UnbanRequestsDialog::resolveCurrentRequest(bool approve)
{
    if (this->selectedRequestId_.isEmpty() || this->actionInFlight_)
    {
        return;
    }
    const auto index = this->requestList_->currentIndex();
    const auto *request = this->requestModel_->requestAt(index.row());
    if (request == nullptr || request->id != this->selectedRequestId_)
    {
        return;
    }

    const auto requestId = request->id;
    const auto generation = this->queueGeneration_;
    const auto authGeneration = this->authGeneration_;
    const auto note = this->moderatorNote_->toPlainText().trimmed();
    const auto name = unbanUserName(request->requester);
    const auto title = approve ? QStringLiteral("Unban %1").arg(name)
                               : QStringLiteral("Deny unban request");
    const auto explanation =
        approve ? QStringLiteral(
                      "%1 will be unbanned and this request will be closed.")
                      .arg(name)
                : QStringLiteral("%1 will remain banned and cannot submit "
                                 "another request for this ban.")
                      .arg(name);
    const QPointer<UnbanRequestsDialog> self(this);
    QPointer<QMessageBox> confirmation = new QMessageBox(
        QMessageBox::Question, title, explanation, QMessageBox::NoButton, this);
    auto *resolveButton = confirmation->addButton(
        approve ? QStringLiteral("Unban") : QStringLiteral("Deny"),
        approve ? QMessageBox::AcceptRole : QMessageBox::DestructiveRole);
    auto *cancelButton = confirmation->addButton(QMessageBox::Cancel);
    confirmation->setDefaultButton(cancelButton);
    confirmation->setEscapeButton(cancelButton);
    installMoltorinoDialogTheme(confirmation);
    confirmation->exec();
    if (!self || !confirmation)
    {
        return;
    }
    const bool confirmed = confirmation->clickedButton() == resolveButton;
    delete confirmation;
    if (!confirmed ||
        authGeneration != this->authGeneration_ ||
        generation != this->queueGeneration_ ||
        requestId != this->selectedRequestId_ ||
        this->requestModel_->rowForId(requestId) < 0)
    {
        return;
    }

    const auto token = this->moderationToken("resolving an unban request");
    if (token.isEmpty())
    {
        return;
    }

    this->actionError_.clear();
    this->actionInFlight_ = true;
    this->updateActionState();
    this->setDetailStatus(
        approve ? QStringLiteral("Unbanning %1...").arg(name)
                : QStringLiteral("Denying %1's request...").arg(name));
    TwitchGql::resolveUnbanRequest(
        requestId, approve, note, token,
        [self, requestId, name, approve, authGeneration] {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            if (authGeneration != self->authGeneration_)
            {
                self->loadFirstPage();
                return;
            }
            self->actionError_.clear();
            const int removedRow =
                self->requestModel_->removeRequest(requestId);
            self->seenRequestIds_.remove(requestId);
            self->detailCache_.remove(requestId);
            self->detailCacheOrder_.removeAll(requestId);
            self->moderatorNoteDrafts_.remove(requestId);
            self->totalRequests_ = std::max(0, self->totalRequests_ - 1);
            self->updateQueueSummary();
            const auto message =
                approve ? QStringLiteral("%1 was unbanned.").arg(name)
                        : QStringLiteral("%1's unban request was denied.")
                              .arg(name);
            self->publishResult(message);
            self->setQueueStatus(message);
            if (self->requestModel_->rowCount() == 0)
            {
                self->clearRequest();
            }
            else
            {
                const int nextRow = std::clamp(
                    removedRow, 0, self->requestModel_->rowCount() - 1);
                self->requestList_->setCurrentIndex(
                    self->requestModel_->index(nextRow, 0));
            }
            self->updateActionState();
        },
        [self, authGeneration](const QString &error) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            if (authGeneration != self->authGeneration_)
            {
                self->loadFirstPage();
                return;
            }
            self->actionError_ = MoltorinoAuth::normalizeAuthError(
                "resolving an unban request", error);
            self->refreshDetailStatus();
            self->updateActionState();
        });
}

void UnbanRequestsDialog::updateQueueSummary()
{
    this->countLabel_->setText(
        this->totalRequests_ == 1
            ? QStringLiteral("1 open request")
            : QStringLiteral("%1 open requests")
                  .arg(QLocale().toString(this->totalRequests_)));
}

void UnbanRequestsDialog::updateActionState()
{
    const bool enabled =
        !this->selectedRequestId_.isEmpty() && !this->actionInFlight_;
    this->approveButton_->setEnabled(enabled);
    this->denyButton_->setEnabled(enabled);
    this->moderatorNote_->setEnabled(enabled);
    this->requestList_->setEnabled(!this->actionInFlight_);
    this->sort_->setEnabled(!this->queueLoading_ && !this->actionInFlight_);
    this->refreshButton_->setEnabled(!this->queueLoading_ &&
                                     !this->actionInFlight_);
}

void UnbanRequestsDialog::refreshDetailStatus()
{
    if (this->actionInFlight_)
    {
        return;
    }
    const auto *cache = this->currentCache();
    if (cache == nullptr)
    {
        this->setDetailStatus({});
        return;
    }

    auto error = this->actionError_;
    if (error.isEmpty())
    {
        error = cache->contextError;
    }
    if (error.isEmpty())
    {
        error = cache->chatError;
    }
    if (error.isEmpty())
    {
        error = cache->commentsError;
    }
    this->setDetailStatus(error, !error.isEmpty());
}

void UnbanRequestsDialog::setQueueStatus(const QString &text, bool error)
{
    this->queueStatusIsError_ = error;
    this->queueStatusLabel_->setText(text);
    this->queueStatusLabel_->setVisible(!text.isEmpty());
    auto palette = this->palette();
    palette.setColor(QPalette::WindowText,
                     error ? palette.color(QPalette::BrightText)
                           : this->palette().color(QPalette::WindowText));
    this->queueStatusLabel_->setPalette(palette);
}

void UnbanRequestsDialog::setDetailStatus(const QString &text, bool error)
{
    this->detailStatusIsError_ = error;
    this->detailStatus_->setText(text);
    this->detailStatus_->setToolTip(text);
    auto palette = this->palette();
    palette.setColor(QPalette::WindowText,
                     error ? palette.color(QPalette::BrightText)
                           : this->palette().color(QPalette::WindowText));
    this->detailStatus_->setPalette(palette);
}

void UnbanRequestsDialog::applyTheme()
{
    applyMoltorinoDialogPalette(this, *getTheme());
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));
    this->setQueueStatus(this->queueStatusLabel_->text(),
                         this->queueStatusIsError_);
    this->setDetailStatus(this->detailStatus_->text(),
                          this->detailStatusIsError_);
}

void UnbanRequestsDialog::publishResult(const QString &text) const
{
    if (const auto channel = this->outputChannel_.lock())
    {
        channel->addSystemMessage(text);
    }
}

}
