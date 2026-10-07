#include "widgets/dialogs/UserLogsView.hpp"

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
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStylePainter>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

QString timestampText(const QDateTime &timestamp)
{
    return timestamp.toLocalTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss t"));
}

QString formattedDate(const QDateTime &timestamp, const QString &style,
                      bool allHistory)
{
    const auto date = timestamp.toLocalTime().date();
    if (style == QLatin1String("iso"))
    {
        return date.toString(Qt::ISODate);
    }
    if (style == QLatin1String("month-first"))
    {
        return QLocale().toString(date, QStringLiteral("MMM d, yyyy"));
    }
    if (style == QLatin1String("day-first"))
    {
        return QLocale().toString(date, QStringLiteral("d MMM yyyy"));
    }
    if (style == QLatin1String("numeric-month-first"))
    {
        return date.toString(QStringLiteral("MM/dd/yyyy"));
    }
    if (style == QLatin1String("numeric-day-first"))
    {
        return date.toString(QStringLiteral("dd/MM/yyyy"));
    }
    return QLocale().toString(date, allHistory ? QStringLiteral("MMM d, yyyy")
                                               : QStringLiteral("MMM d"));
}

QString formattedTime(const QDateTime &timestamp, const QString &style)
{
    const auto time = timestamp.toLocalTime().time();
    if (style == QLatin1String("24h-second"))
    {
        return time.toString(QStringLiteral("HH:mm:ss"));
    }
    if (style == QLatin1String("12h-minute"))
    {
        return QLocale().toString(time, QStringLiteral("h:mm AP"));
    }
    if (style == QLatin1String("12h-second"))
    {
        return QLocale().toString(time, QStringLiteral("h:mm:ss AP"));
    }
    return time.toString(QStringLiteral("HH:mm"));
}

QString formattedTimestamp(const QDateTime &timestamp, bool showDate,
                           const QString &dateStyle, const QString &timeStyle,
                           bool allHistory)
{
    const auto time = formattedTime(timestamp, timeStyle);
    if (!showDate)
    {
        return time;
    }
    return formattedDate(timestamp, dateStyle, allHistory) + u' ' + time;
}

QString copyText(const twitch_user_logs::Message &message,
                 bool includeTimestamp, bool showDate, const QString &dateStyle,
                 const QString &timeStyle, bool allHistory)
{
    if (!includeTimestamp)
    {
        return message.text;
    }
    return QStringLiteral("[%1] %2").arg(
        formattedTimestamp(message.timestamp, showDate, dateStyle, timeStyle,
                           allHistory),
        message.text);
}

void openExternalUrl(const QUrl &url)
{
    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(url.toString());
        return;
    }
    QDesktopServices::openUrl(url);
}

}

class CompactPeriodComboBox final : public QComboBox
{
public:
    enum { CompactLabel = Qt::UserRole + 1 };

    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        this->initStyleOption(&option);
        if (const auto compact = this->currentData(CompactLabel).toString();
            !compact.isEmpty())
        {
            option.currentText = compact;
        }
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

class UserLogsModel final : public QAbstractListModel
{
public:
    enum Role {
        MessageText = Qt::UserRole + 1,
        Timestamp,
        StartsDay,
    };

    UserLogsModel(const QVector<const twitch_user_logs::Message *> *messages,
                  QObject *parent)
        : QAbstractListModel(parent)
        , messages_(messages)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() || this->messages_ == nullptr
                   ? 0
                   : this->messages_->size();
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || this->messages_ == nullptr || index.row() < 0 ||
            index.row() >= this->messages_->size())
        {
            return {};
        }
        const auto &message = *this->messages_->at(index.row());
        switch (role)
        {
            case Qt::DisplayRole:
            case MessageText:
                return message.text;
            case Timestamp:
                return message.timestamp;
            case StartsDay:
                return index.row() > 0 &&
                       this->messages_->at(index.row() - 1)
                               ->timestamp.toLocalTime()
                               .date() !=
                           message.timestamp.toLocalTime().date();
            case Qt::ToolTipRole:
                return QStringLiteral("%1\n%2").arg(
                    timestampText(message.timestamp), message.text);
            default:
                return {};
        }
    }

    void refresh()
    {
        this->beginResetModel();
        this->endResetModel();
    }

private:
    const QVector<const twitch_user_logs::Message *> *messages_{};
};

namespace {

class UserLogDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setColors(const QColor &background, const QColor &alternate,
                   const QColor &hover, const QColor &selected,
                   const QColor &text, const QColor &timestamp,
                   const QColor &separator)
    {
        this->background_ = background;
        this->alternate_ = alternate;
        this->hover_ = hover;
        this->selected_ = selected;
        this->text_ = text;
        this->timestamp_ = timestamp;
        this->separator_ = separator;
    }

    void setTimestampStyle(bool showDate, QString dateStyle, QString timeStyle,
                           bool allHistory)
    {
        this->showDate_ = showDate;
        this->dateStyle_ = std::move(dateStyle);
        this->timeStyle_ = std::move(timeStyle);
        this->allHistory_ = allHistory;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(),
                std::max(24, QFontMetrics(option.font).height() + 8)};
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

        const auto startsDay = index.data(UserLogsModel::StartsDay).toBool();
        if (startsDay)
        {
            painter->setPen(this->separator_);
            painter->drawLine(option.rect.topLeft(), option.rect.topRight());
        }

        const QFontMetrics metrics(option.font);
        const auto horizontalPadding = 8;
        const auto timestamp = formattedTimestamp(
            index.data(UserLogsModel::Timestamp).toDateTime(), this->showDate_,
            this->dateStyle_, this->timeStyle_, this->allHistory_);
        const auto timestampWidth = metrics.horizontalAdvance(timestamp) + 14;
        const auto baseline = option.rect.top() +
                              (option.rect.height() - metrics.height()) / 2 +
                              metrics.ascent();

        painter->setFont(option.font);
        painter->setPen(this->timestamp_);
        painter->drawText(option.rect.left() + horizontalPadding, baseline,
                          timestamp);

        const auto messageLeft =
            option.rect.left() + horizontalPadding + timestampWidth;
        const auto messageWidth =
            std::max(0, option.rect.right() - messageLeft - horizontalPadding);
        const auto message = metrics.elidedText(
            index.data(UserLogsModel::MessageText).toString(), Qt::ElideRight,
            messageWidth);
        painter->setPen(this->text_);
        painter->drawText(messageLeft, baseline, message);
        painter->restore();
    }

private:
    QColor background_;
    QColor alternate_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor timestamp_;
    QColor separator_;
    bool showDate_ = true;
    bool allHistory_ = false;
    QString dateStyle_;
    QString timeStyle_;
};

}

UserLogsView::UserLogsView(QWidget *parent)
    : QWidget(parent)
{
    this->setAttribute(Qt::WA_NoMousePropagation);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    auto *periodRow = new QHBoxLayout;
    periodRow->setContentsMargins(0, 0, 0, 0);
    periodRow->setSpacing(5);
    this->olderButton_ = new QToolButton(this);
    this->olderButton_->setArrowType(Qt::LeftArrow);
    this->olderButton_->setToolTip("Older period");
    this->olderButton_->setAccessibleName("Older period");
    periodRow->addWidget(this->olderButton_);
    this->periodBox_ = new CompactPeriodComboBox(this);
    this->periodBox_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    this->periodBox_->setMinimumContentsLength(7);
    this->periodBox_->setAccessibleName("Log period");
    periodRow->addWidget(this->periodBox_);
    this->newerButton_ = new QToolButton(this);
    this->newerButton_->setArrowType(Qt::RightArrow);
    this->newerButton_->setToolTip("Newer period");
    this->newerButton_->setAccessibleName("Newer period");
    periodRow->addWidget(this->newerButton_);
    this->statusLabel_ = new Label;
    this->statusLabel_->setPadding({0, 0, 0, 0});
    this->statusLabel_->setShouldElide(true);
    this->statusLabel_->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Preferred);
    periodRow->addWidget(this->statusLabel_, 1);
    this->retryButton_ = new QPushButton("Retry", this);
    this->retryButton_->setAutoDefault(false);
    this->retryButton_->hide();
    periodRow->addWidget(this->retryButton_);
    this->webButton_ = new QPushButton("Open on web", this);
    this->webButton_->setAutoDefault(false);
    periodRow->addWidget(this->webButton_);
    layout->addLayout(periodRow);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(5);
    this->searchInput_ = new QLineEdit(this);
    this->searchInput_->setPlaceholderText("Search this period");
    this->searchInput_->setClearButtonEnabled(true);
    this->searchInput_->setMaxLength(500);
    this->searchInput_->setAccessibleName("Search logs");
    searchRow->addWidget(this->searchInput_, 1);
    this->allHistoryButton_ = new QPushButton("Search all history", this);
    this->allHistoryButton_->setAutoDefault(false);
    searchRow->addWidget(this->allHistoryButton_);
    layout->addLayout(searchRow);

    this->list_ = new QListView(this);
    this->model_ = new UserLogsModel(&this->visibleMessages_, this);
    this->list_->setModel(this->model_);
    this->list_->setItemDelegate(new UserLogDelegate(this->list_));
    this->list_->setFrameShape(QFrame::NoFrame);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->list_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->list_->setMouseTracking(true);
    this->list_->setUniformItemSizes(true);
    this->list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(this->list_, 1);

    this->filterTimer_ = new QTimer(this);
    this->filterTimer_->setSingleShot(true);
    this->filterTimer_->setInterval(100);

    QObject::connect(this->olderButton_, &QToolButton::clicked, this, [this] {
        if (this->mode_ == Mode::AllHistory)
        {
            this->loadSearchPage(
                this->allHistoryQuery_,
                this->searchOffset_ +
                    twitch_user_logs::detail::SEARCH_PAGE_SIZE);
            return;
        }
        this->selectPeriod(this->selectedPeriod_ + 1);
    });
    QObject::connect(this->newerButton_, &QToolButton::clicked, this, [this] {
        if (this->mode_ == Mode::AllHistory)
        {
            this->loadSearchPage(
                this->allHistoryQuery_,
                std::max<qsizetype>(
                    0, this->searchOffset_ -
                           twitch_user_logs::detail::SEARCH_PAGE_SIZE));
            return;
        }
        this->selectPeriod(this->selectedPeriod_ - 1);
    });
    QObject::connect(this->periodBox_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this](int index) {
                         if (index != this->selectedPeriod_)
                         {
                             this->selectPeriod(index);
                         }
                     });
    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this, [this] {
        if (this->mode_ == Mode::AllHistory &&
            this->searchInput_->text().trimmed().isEmpty())
        {
            this->filterTimer_->stop();
            this->returnToPeriod();
            return;
        }
        this->filterTimer_->start();
        this->updateControls();
    });
    QObject::connect(this->searchInput_, &QLineEdit::returnPressed, this,
                     [this] {
                         if (this->allHistoryButton_->isEnabled())
                         {
                             this->allHistoryButton_->click();
                         }
                     });
    QObject::connect(this->filterTimer_, &QTimer::timeout, this, [this] {
        this->rebuildVisibleMessages(true, false);
    });
    QObject::connect(
        this->allHistoryButton_, &QPushButton::clicked, this, [this] {
            const auto query = this->searchInput_->text().trimmed();
            if (this->mode_ == Mode::AllHistory &&
                query == this->allHistoryQuery_)
            {
                this->returnToPeriod();
                return;
            }
            this->searchAllHistory();
        });
    QObject::connect(this->retryButton_, &QPushButton::clicked, this, [this] {
        this->retry();
    });
    QObject::connect(this->webButton_, &QPushButton::clicked, this, [this] {
        this->openOnWeb();
    });
    QObject::connect(this->list_, &QListView::customContextMenuRequested, this,
                     [this](const QPoint &position) {
                         this->showMessageMenu(position);
                     });

    auto *copyShortcut =
        new QShortcut(QKeySequence::StandardKey::Copy, this->list_);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(copyShortcut, &QShortcut::activated, this, [this] {
        this->copySelectedMessage();
    });
    auto *findShortcut = new QShortcut(QKeySequence::StandardKey::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(findShortcut, &QShortcut::activated, this, [this] {
        this->searchInput_->setFocus(Qt::ShortcutFocusReason);
        this->searchInput_->selectAll();
    });

    getSettings()->showUserLogsDate.connect(
        [this](const auto &, auto) {
            this->refreshTimestampStyle();
        },
        this->settingConnections_, false);
    getSettings()->userLogsNewestAtBottom.connect(
        [this](const auto &value, auto) {
            this->newestAtBottom_ = value;
            this->rebuildVisibleMessages(false, false);
        },
        this->settingConnections_, false);
    getSettings()->userLogsDateStyle.connect(
        [this](const auto &, auto) {
            this->refreshTimestampStyle();
        },
        this->settingConnections_, false);
    getSettings()->userLogsTimeStyle.connect(
        [this](const auto &, auto) {
            this->refreshTimestampStyle();
        },
        this->settingConnections_, false);

    this->setMinimumSize(430, 275);
    this->newestAtBottom_ = getSettings()->userLogsNewestAtBottom.getValue();
    this->refreshTimestampStyle();
    this->updateControls();
    this->updateStatus();
}

void UserLogsView::setContext(const QString &channel, const QString &user)
{
    const auto normalizedChannel = channel.trimmed().toLower();
    const auto normalizedUser = user.trimmed().toLower();
    if (this->channel_ == normalizedChannel && this->user_ == normalizedUser)
    {
        return;
    }
    this->channel_ = normalizedChannel;
    this->user_ = normalizedUser;
    this->reset();
    if (!this->channel_.isEmpty() && !this->user_.isEmpty())
    {
        this->loadPeriods();
    }
}

void UserLogsView::activate()
{
    this->activated_ = true;
    if (!this->periodsLoaded_ && !this->loadingPeriods_)
    {
        this->loadPeriods();
    }
    else if (this->periodsLoaded_ && !this->periodPageLoaded_ &&
             !this->loadingPage_ && this->selectedPeriod_ >= 0)
    {
        this->loadSelectedPeriod();
    }
}

void UserLogsView::deactivate()
{
    this->activated_ = false;
}

void UserLogsView::refreshStyle(float scale)
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
    palette.setColor(QPalette::PlaceholderText,
                     theme->messages.textColors.chatPlaceholder);
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     theme->tabs.regular.text);

    this->setPalette(palette);
    for (auto *widget : {static_cast<QWidget *>(this->olderButton_),
                         static_cast<QWidget *>(this->newerButton_),
                         static_cast<QWidget *>(this->periodBox_),
                         static_cast<QWidget *>(this->statusLabel_),
                         static_cast<QWidget *>(this->retryButton_),
                         static_cast<QWidget *>(this->webButton_),
                         static_cast<QWidget *>(this->allHistoryButton_),
                         static_cast<QWidget *>(this->list_)})
    {
        widget->setPalette(palette);
    }
    auto inputPalette = palette;
    inputPalette.setColor(QPalette::Base, theme->splits.input.background);
    inputPalette.setColor(QPalette::Text, theme->splits.input.text);
    this->searchInput_->setPalette(inputPalette);

    auto *delegate =
        static_cast<UserLogDelegate *>(this->list_->itemDelegate());
    delegate->setColors(
        theme->messages.backgrounds.regular,
        theme->messages.backgrounds.alternate,
        theme->tabs.regular.backgrounds.hover, theme->messages.selection,
        theme->messages.textColors.regular,
        theme->messages.textColors.timestamp, theme->splits.messageSeperator);

    this->setFont(font);
    this->list_->setFont(font);
    this->refreshPeriodWidths();
    this->list_->doItemsLayout();
    this->list_->viewport()->update();
}

void UserLogsView::reset()
{
    ++this->generation_;
    this->filterTimer_->stop();
    this->periods_.clear();
    this->periodPage_ = {};
    this->searchPage_ = {};
    this->visibleMessages_ = {};
    this->allHistoryQuery_.clear();
    this->retrySearchQuery_.clear();
    this->error_.clear();
    this->searchOffset_ = 0;
    this->retrySearchOffset_ = 0;
    this->selectedPeriod_ = -1;
    this->mode_ = Mode::Period;
    this->retryAction_ = RetryAction::None;
    this->periodsLoaded_ = false;
    this->periodPageLoaded_ = false;
    this->loadingPeriods_ = false;
    this->loadingPage_ = false;
    this->searchingAllHistory_ = false;
    this->periodBox_->blockSignals(true);
    this->periodBox_->clear();
    this->periodBox_->blockSignals(false);
    const QSignalBlocker searchBlocker(this->searchInput_);
    this->searchInput_->clear();
    this->model_->refresh();
    this->updateControls();
    this->updateStatus();
}

void UserLogsView::loadPeriods()
{
    if (this->loadingPeriods_ || this->channel_.isEmpty() ||
        this->user_.isEmpty())
    {
        this->updateStatus();
        return;
    }
    this->loadingPeriods_ = true;
    this->error_.clear();
    this->retryAction_ = RetryAction::None;
    const auto generation = this->generation_;
    const QPointer<UserLogsView> self(this);
    this->updateControls();
    this->updateStatus();
    twitch_user_logs::loadAvailablePeriods(
        this->channel_, this->user_,
        [self, generation](QVector<twitch_user_logs::Period> periods) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingPeriods_ = false;
            self->periodsLoaded_ = true;
            self->periods_ = std::move(periods);
            self->periodBox_->blockSignals(true);
            self->periodBox_->clear();
            for (const auto &period : self->periods_)
            {
                self->periodBox_->addItem(period.label());
                self->periodBox_->setItemData(
                    self->periodBox_->count() - 1, period.compactLabel(),
                    CompactPeriodComboBox::CompactLabel);
            }
            self->selectedPeriod_ = self->periods_.isEmpty() ? -1 : 0;
            self->periodBox_->setCurrentIndex(self->selectedPeriod_);
            self->periodBox_->blockSignals(false);
            self->refreshPeriodWidths();
            self->updateControls();
            if (self->activated_ && self->selectedPeriod_ >= 0)
            {
                self->loadSelectedPeriod();
            }
            else
            {
                self->updateStatus();
            }
        },
        [self, generation](QString error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingPeriods_ = false;
            self->error_ = std::move(error);
            self->retryAction_ = RetryAction::Periods;
            self->updateControls();
            self->updateStatus();
        });
}

void UserLogsView::loadSelectedPeriod()
{
    if (this->loadingPage_ || this->selectedPeriod_ < 0 ||
        this->selectedPeriod_ >= this->periods_.size())
    {
        return;
    }
    this->filterTimer_->stop();
    this->loadingPage_ = true;
    this->periodPageLoaded_ = false;
    this->searchingAllHistory_ = false;
    this->mode_ = Mode::Period;
    this->periodPage_ = {};
    this->searchPage_ = {};
    this->allHistoryQuery_.clear();
    this->retrySearchQuery_.clear();
    this->searchOffset_ = 0;
    this->retrySearchOffset_ = 0;
    this->visibleMessages_ = {};
    this->model_->refresh();
    this->error_.clear();
    this->retryAction_ = RetryAction::None;
    const auto generation = this->generation_;
    const auto periodIndex = this->selectedPeriod_;
    const auto period = this->periods_.at(periodIndex);
    const QPointer<UserLogsView> self(this);
    this->updateControls();
    this->updateStatus();
    twitch_user_logs::loadPeriod(
        this->channel_, this->user_, period,
        [self, generation, periodIndex](twitch_user_logs::Page page) {
            if (!self || generation != self->generation_ ||
                periodIndex != self->selectedPeriod_)
            {
                return;
            }
            self->loadingPage_ = false;
            self->searchingAllHistory_ = false;
            self->periodPageLoaded_ = true;
            self->periodPage_ = std::move(page);
            self->rebuildVisibleMessages();
        },
        [self, generation, periodIndex](QString error) {
            if (!self || generation != self->generation_ ||
                periodIndex != self->selectedPeriod_)
            {
                return;
            }
            self->loadingPage_ = false;
            self->searchingAllHistory_ = false;
            self->periodPageLoaded_ = false;
            self->error_ = std::move(error);
            self->retryAction_ = RetryAction::Period;
            self->updateControls();
            self->updateStatus();
        },
        [self, generation, periodIndex] {
            return self && generation == self->generation_ &&
                   periodIndex == self->selectedPeriod_;
        });
}

void UserLogsView::searchAllHistory()
{
    const auto query = this->searchInput_->text().trimmed();
    if (query.isEmpty() || this->loadingPage_ || this->channel_.isEmpty() ||
        this->user_.isEmpty())
    {
        return;
    }
    this->loadSearchPage(query, 0);
}

void UserLogsView::loadSearchPage(QString query, qsizetype offset)
{
    if (query.isEmpty() || offset < 0 || this->loadingPage_ ||
        this->channel_.isEmpty() || this->user_.isEmpty())
    {
        return;
    }
    this->filterTimer_->stop();
    this->loadingPage_ = true;
    this->searchingAllHistory_ = true;
    this->error_.clear();
    this->retryAction_ = RetryAction::None;
    this->retrySearchQuery_.clear();
    const auto generation = this->generation_;
    const QPointer<UserLogsView> self(this);
    this->updateControls();
    this->updateStatus();
    twitch_user_logs::search(
        this->channel_, this->user_, query, offset,
        [self, generation, query, offset](twitch_user_logs::Page page) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingPage_ = false;
            self->searchingAllHistory_ = false;
            self->mode_ = Mode::AllHistory;
            self->allHistoryQuery_ = query;
            self->searchOffset_ = offset;
            self->searchPage_ = std::move(page);
            self->rebuildVisibleMessages();
        },
        [self, generation, query, offset](QString error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loadingPage_ = false;
            self->searchingAllHistory_ = false;
            self->error_ = std::move(error);
            self->retryAction_ = RetryAction::Search;
            self->retrySearchQuery_ = query;
            self->retrySearchOffset_ = offset;
            self->updateControls();
            self->updateStatus();
        },
        [self, generation] {
            return self && generation == self->generation_;
        });
}

void UserLogsView::returnToPeriod()
{
    this->mode_ = Mode::Period;
    this->searchPage_ = {};
    this->allHistoryQuery_.clear();
    this->retrySearchQuery_.clear();
    this->searchOffset_ = 0;
    this->retrySearchOffset_ = 0;
    if (!this->periodPageLoaded_ && this->selectedPeriod_ >= 0)
    {
        this->loadSelectedPeriod();
        return;
    }
    this->rebuildVisibleMessages(false, false);
}

void UserLogsView::retry()
{
    const auto action = std::exchange(this->retryAction_, RetryAction::None);
    this->error_.clear();
    switch (action)
    {
        case RetryAction::Periods:
            this->loadPeriods();
            break;
        case RetryAction::Period:
            this->loadSelectedPeriod();
            break;
        case RetryAction::Search:
            this->loadSearchPage(this->retrySearchQuery_,
                                 this->retrySearchOffset_);
            break;
        case RetryAction::None:
            break;
    }
}

void UserLogsView::selectPeriod(int index)
{
    if (this->loadingPage_ || index < 0 || index >= this->periods_.size() ||
        index == this->selectedPeriod_)
    {
        return;
    }
    this->selectedPeriod_ = index;
    this->periodBox_->blockSignals(true);
    this->periodBox_->setCurrentIndex(index);
    this->periodBox_->blockSignals(false);
    this->loadSelectedPeriod();
}

void UserLogsView::rebuildVisibleMessages(bool preserveScroll, bool clearError)
{
    const auto oldScroll = this->list_->verticalScrollBar()->value();
    const auto &source = this->mode_ == Mode::AllHistory
                             ? this->searchPage_.messages
                             : this->periodPage_.messages;
    const auto query = this->searchInput_->text().trimmed();
    const auto maximumUsefulCapacity =
        std::max<qsizetype>(4096, source.size() * 2);
    if (this->visibleMessages_.capacity() > maximumUsefulCapacity)
    {
        this->visibleMessages_ = {};
    }
    else
    {
        this->visibleMessages_.clear();
    }
    this->visibleMessages_.reserve(source.size());
    const bool serverAlreadyFiltered =
        this->mode_ == Mode::AllHistory && query == this->allHistoryQuery_;
    const auto appendIfVisible = [this, &query,
                                  serverAlreadyFiltered](const auto &message) {
        if (query.isEmpty() || serverAlreadyFiltered ||
            message.text.contains(query, Qt::CaseInsensitive))
        {
            this->visibleMessages_.push_back(&message);
        }
    };
    if (this->newestAtBottom_)
    {
        for (auto it = source.crbegin(); it != source.crend(); ++it)
        {
            appendIfVisible(*it);
        }
    }
    else
    {
        for (const auto &message : source)
        {
            appendIfVisible(message);
        }
    }
    this->model_->refresh();
    this->refreshTimestampStyle();
    if (preserveScroll)
    {
        this->list_->verticalScrollBar()->setValue(oldScroll);
    }
    else
    {
        if (this->newestAtBottom_)
        {
            this->list_->scrollToBottom();
        }
        else
        {
            this->list_->scrollToTop();
        }
    }
    if (clearError)
    {
        this->error_.clear();
        this->retryAction_ = RetryAction::None;
    }
    this->updateControls();
    this->updateStatus();
}

void UserLogsView::refreshTimestampStyle()
{
    this->showDate_ = getSettings()->showUserLogsDate.getValue();
    this->dateStyle_ = getSettings()->userLogsDateStyle.getValue();
    this->timeStyle_ = getSettings()->userLogsTimeStyle.getValue();
    auto *delegate =
        static_cast<UserLogDelegate *>(this->list_->itemDelegate());
    delegate->setTimestampStyle(this->showDate_, this->dateStyle_,
                                this->timeStyle_,
                                this->mode_ == Mode::AllHistory);
    this->list_->doItemsLayout();
    this->list_->viewport()->update();
}

void UserLogsView::refreshPeriodWidths()
{
    const QFontMetrics metrics(this->periodBox_->font());
    auto compactWidth = metrics.horizontalAdvance(QStringLiteral("Sep '00"));
    auto popupWidth = compactWidth;
    for (int index = 0; index < this->periodBox_->count(); ++index)
    {
        compactWidth = std::max(
            compactWidth,
            metrics.horizontalAdvance(
                this->periodBox_
                    ->itemData(index, CompactPeriodComboBox::CompactLabel)
                    .toString()));
        popupWidth = std::max(
            popupWidth,
            metrics.horizontalAdvance(this->periodBox_->itemText(index)));
    }
    const auto arrowAndPadding =
        this->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 22;
    this->periodBox_->setFixedWidth(compactWidth + arrowAndPadding);
    this->periodBox_->view()->setMinimumWidth(
        std::max(this->periodBox_->width(), popupWidth + arrowAndPadding));
}

void UserLogsView::updateControls()
{
    const auto busy = this->loadingPeriods_ || this->loadingPage_;
    const auto hasPeriods = !this->periods_.isEmpty();
    const auto currentQuery = this->searchInput_->text().trimmed();
    const auto inSearchMode = this->mode_ == Mode::AllHistory;
    const auto currentSearch =
        inSearchMode && currentQuery == this->allHistoryQuery_;
    if (inSearchMode)
    {
        this->olderButton_->setToolTip("Older matches");
        this->olderButton_->setAccessibleName("Older matches");
        this->newerButton_->setToolTip("Newer matches");
        this->newerButton_->setAccessibleName("Newer matches");
        this->olderButton_->setEnabled(
            !busy && this->retryAction_ == RetryAction::None && currentSearch &&
            this->searchPage_.hasMoreResults);
        this->newerButton_->setEnabled(
            !busy && this->retryAction_ == RetryAction::None && currentSearch &&
            this->searchOffset_ > 0);
    }
    else
    {
        this->olderButton_->setToolTip("Older period");
        this->olderButton_->setAccessibleName("Older period");
        this->newerButton_->setToolTip("Newer period");
        this->newerButton_->setAccessibleName("Newer period");
        this->olderButton_->setEnabled(
            !busy && hasPeriods && this->selectedPeriod_ >= 0 &&
            this->selectedPeriod_ + 1 < this->periods_.size());
        this->newerButton_->setEnabled(!busy && hasPeriods &&
                                       this->selectedPeriod_ > 0);
    }
    this->periodBox_->setEnabled(!busy && hasPeriods);
    this->searchInput_->setEnabled(!busy && hasPeriods);

    this->allHistoryButton_->setText(currentSearch ? "Search this period"
                                                  : "Search all history");
    this->allHistoryButton_->setEnabled(
        !busy && hasPeriods && (!currentQuery.isEmpty() || currentSearch));
    this->retryButton_->setVisible(this->retryAction_ != RetryAction::None);
    this->webButton_->setEnabled(!this->channel_.isEmpty() &&
                                 !this->user_.isEmpty());
}

void UserLogsView::updateStatus()
{
    this->statusLabel_->setToolTip({});
    if (this->channel_.isEmpty() || this->user_.isEmpty())
    {
        this->statusLabel_->setText("Logs are not ready yet.");
        return;
    }
    if (this->loadingPeriods_)
    {
        this->statusLabel_->setText("Finding available logs...");
        return;
    }
    if (this->loadingPage_)
    {
        this->statusLabel_->setText(this->searchingAllHistory_
                                        ? "Searching all history..."
                                        : "Loading logs...");
        return;
    }
    if (!this->error_.isEmpty())
    {
        this->statusLabel_->setText(this->error_);
        this->statusLabel_->setToolTip(this->error_);
        return;
    }
    if (this->periodsLoaded_ && this->periods_.isEmpty())
    {
        this->statusLabel_->setText(
            "No logs for this user in this channel.");
        return;
    }
    if (this->periodsLoaded_ && !this->activated_ && !this->periodPageLoaded_)
    {
        this->statusLabel_->setText("Logs are ready.");
        return;
    }

    const auto &page =
        this->mode_ == Mode::AllHistory ? this->searchPage_ : this->periodPage_;
    const auto query = this->searchInput_->text().trimmed();
    if (this->mode_ == Mode::AllHistory && page.messages.isEmpty())
    {
        this->statusLabel_->setText(
            this->searchOffset_ > 0
                ? QStringLiteral("No older matches remain.")
                : QStringLiteral("No messages matched “%1”.")
                      .arg(this->allHistoryQuery_));
        return;
    }
    if (this->mode_ == Mode::Period &&
        this->selectedPeriod_ >= 0 && page.messages.isEmpty())
    {
        this->statusLabel_->setText("No messages in this period.");
        return;
    }
    if (!query.isEmpty() && this->visibleMessages_.isEmpty() &&
        !page.messages.isEmpty())
    {
        this->statusLabel_->setText(
            QStringLiteral("No matches for “%1”.").arg(query));
        return;
    }

    auto count =
        QStringLiteral("%1 message%2")
            .arg(QLocale().toString(this->visibleMessages_.size()))
            .arg(this->visibleMessages_.size() == 1 ? QString()
                                                    : QStringLiteral("s"));
    if (!query.isEmpty() &&
        this->visibleMessages_.size() != page.messages.size())
    {
        count = QStringLiteral("%1 of %2 messages")
                    .arg(QLocale().toString(this->visibleMessages_.size()))
                    .arg(QLocale().toString(page.messages.size()));
    }
    if (this->mode_ == Mode::AllHistory)
    {
        const auto pageNumber =
            this->searchOffset_ / twitch_user_logs::detail::SEARCH_PAGE_SIZE +
            1;
        this->statusLabel_->setText(
            QStringLiteral("All history · page %1 · %2")
                .arg(QLocale().toString(pageNumber), count));
        return;
    }
    this->statusLabel_->setText(count);
}

void UserLogsView::openOnWeb() const
{
    if (this->channel_.isEmpty() || this->user_.isEmpty())
    {
        return;
    }
    QUrl url(QStringLiteral("https://tv.supa.sh/logs"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("c"), this->channel_);
    query.addQueryItem(QStringLiteral("u"), this->user_);
    const auto search = this->searchInput_->text().trimmed();
    if (this->mode_ == Mode::AllHistory && !this->allHistoryQuery_.isEmpty())
    {
        query.addQueryItem(QStringLiteral("q"), this->allHistoryQuery_);
    }
    else
    {
        if (this->selectedPeriod_ >= 0 &&
            this->selectedPeriod_ < this->periods_.size())
        {
            query.addQueryItem(QStringLiteral("d"),
                               this->periods_.at(this->selectedPeriod_).key());
        }
        if (!search.isEmpty())
        {
            query.addQueryItem(QStringLiteral("s"), search);
        }
    }
    url.setQuery(query);
    openExternalUrl(url);
}

void UserLogsView::copySelectedMessage(bool includeTimestamp) const
{
    const auto row = this->list_->currentIndex().row();
    if (row < 0 || row >= this->visibleMessages_.size())
    {
        return;
    }
    crossPlatformCopy(copyText(
        *this->visibleMessages_.at(row), includeTimestamp, this->showDate_,
        this->dateStyle_, this->timeStyle_, this->mode_ == Mode::AllHistory));
}

void UserLogsView::resolveMessageLink(const twitch_user_logs::Message &message,
                                      std::function<void(QUrl)> callback)
{
    const auto fallback =
        twitch_user_logs::detail::messageUrl(this->channel_, message);
    if (!fallback.isValid() || fallback.isEmpty())
    {
        return;
    }
    if (!message.siteAnchor.isEmpty())
    {
        callback(fallback);
        return;
    }

    const auto generation = this->generation_;
    const QPointer<UserLogsView> self(this);
    const auto onSuccess = [self, generation, message,
                            callback](QString siteAnchor) mutable {
        if (!self || generation != self->generation_)
        {
            return;
        }
        auto resolved = message;
        resolved.siteAnchor = std::move(siteAnchor);
        callback(
            twitch_user_logs::detail::messageUrl(self->channel_, resolved));
    };
    const auto onError = [self, generation, fallback,
                          callback](const QString &) mutable {
        if (!self || generation != self->generation_)
        {
            return;
        }
        callback(fallback);
    };

    if (this->mode_ == Mode::AllHistory)
    {
        twitch_user_logs::resolveSearchMessageAnchor(
            this->channel_, this->user_, this->allHistoryQuery_, message,
            onSuccess, onError);
        return;
    }
    if (this->selectedPeriod_ < 0 ||
        this->selectedPeriod_ >= this->periods_.size())
    {
        callback(fallback);
        return;
    }
    twitch_user_logs::resolvePeriodMessageAnchor(
        this->channel_, this->user_, this->periods_.at(this->selectedPeriod_),
        message, onSuccess, onError);
}

void UserLogsView::showMessageMenu(const QPoint &position)
{
    const auto index = this->list_->indexAt(position);
    if (!index.isValid())
    {
        return;
    }
    this->list_->setCurrentIndex(index);
    const auto message = *this->visibleMessages_.at(index.row());
    const auto withTime = copyText(message, true, this->showDate_,
                                    this->dateStyle_, this->timeStyle_,
                                    this->mode_ == Mode::AllHistory);
    auto *menu = new QMenu(this);
    menu->addAction("Copy message", this, [text = message.text] {
        crossPlatformCopy(text);
    });
    menu->addAction("Copy with time", this, [withTime] {
        crossPlatformCopy(withTime);
    });
    const auto messageUrl =
        twitch_user_logs::detail::messageUrl(this->channel_, message);
    if (messageUrl.isValid() && !messageUrl.isEmpty())
    {
        const auto resolveLink =
            [this, message, generation = this->generation_,
             period = this->selectedPeriod_, mode = this->mode_,
             query = this->allHistoryQuery_](std::function<void(QUrl)> callback) {
                if (generation == this->generation_ &&
                    period == this->selectedPeriod_ && mode == this->mode_ &&
                    query == this->allHistoryQuery_)
                {
                    this->resolveMessageLink(message, std::move(callback));
                }
            };
        menu->addSeparator();
        menu->addAction("Open message on web", this, [resolveLink] {
            resolveLink([](const QUrl &url) { openExternalUrl(url); });
        });
        menu->addAction("Copy message link", this, [resolveLink] {
            resolveLink([](const QUrl &url) {
                crossPlatformCopy(url.toString(QUrl::FullyEncoded));
            });
        });
    }
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(this->list_->viewport()->mapToGlobal(position));
}

}
