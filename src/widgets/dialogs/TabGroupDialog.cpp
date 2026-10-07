#include "widgets/dialogs/TabGroupDialog.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {

namespace {

constexpr auto CHANNEL_ROLE = Qt::UserRole + 1;
constexpr auto OTHER_GROUP_ROLE = Qt::UserRole + 2;
constexpr auto MOVING_GROUP_ROLE = Qt::UserRole + 3;

class TabGroupRowDelegate final : public QStyledItemDelegate
{
public:
    explicit TabGroupRowDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    void setColors(const QColor &background, const QColor &hover,
                   const QColor &selected, const QColor &text,
                   const QColor &muted, const QColor &separator,
                   const QColor &accent)
    {
        this->background_ = background;
        this->hover_ = hover;
        this->selected_ = selected;
        this->text_ = text;
        this->muted_ = muted;
        this->separator_ = separator;
        this->accent_ = accent;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(),
                std::max(30, QFontMetrics(option.font).height() + 12)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);

        const auto background = option.state & QStyle::State_Selected
                                    ? this->selected_
                                : option.state & QStyle::State_MouseOver
                                    ? this->hover_
                                    : this->background_;
        painter->fillRect(option.rect, background);

        constexpr auto SIDE_PADDING = 8;
        const auto checkSize = std::min(14, option.rect.height() - 10);
        const QRect checkRect(option.rect.left() + SIDE_PADDING,
                              option.rect.center().y() - checkSize / 2,
                              checkSize, checkSize);
        painter->setRenderHint(QPainter::Antialiasing);
        const auto checked =
            index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const auto otherGroup = index.data(OTHER_GROUP_ROLE).toString();
        const auto moving = index.data(MOVING_GROUP_ROLE).toBool();
        const auto locked = !otherGroup.isEmpty() && !moving;
        if (locked)
        {
            painter->setPen(QPen(this->muted_, 1.2, Qt::SolidLine,
                                 Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            const QRectF shackle(checkRect.left() + checkSize * 0.27,
                                 checkRect.top() + checkSize * 0.13,
                                 checkSize * 0.46, checkSize * 0.5);
            painter->drawArc(shackle, 0, 180 * 16);
            const QRectF body(checkRect.left() + checkSize * 0.18,
                              checkRect.top() + checkSize * 0.45,
                              checkSize * 0.64, checkSize * 0.43);
            painter->drawRect(body);
        }
        else
        {
            painter->setPen(QPen(checked ? this->accent_ : this->muted_, 1));
            auto checkedBackground = this->accent_;
            checkedBackground.setAlpha(38);
            painter->setBrush(checked ? checkedBackground : Qt::NoBrush);
            painter->drawRect(
                QRectF(checkRect).adjusted(0.5, 0.5, -0.5, -0.5));
            if (checked)
            {
                QPainterPath tick;
                tick.moveTo(checkRect.left() + checkSize * 0.22,
                            checkRect.top() + checkSize * 0.52);
                tick.lineTo(checkRect.left() + checkSize * 0.43,
                            checkRect.top() + checkSize * 0.72);
                tick.lineTo(checkRect.left() + checkSize * 0.8,
                            checkRect.top() + checkSize * 0.27);
                painter->setPen(QPen(this->accent_, 1.4, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(tick);
            }
        }

        const auto title = index.data(Qt::DisplayRole).toString();
        const auto channel = index.data(CHANNEL_ROLE).toString();
        const auto left = checkRect.right() + 9;
        const auto right = option.rect.right() - SIDE_PADDING;
        const QFontMetrics metrics(option.font);
        const auto groupStatusWidth = otherGroup.isEmpty()
                                          ? 0
                                          : std::clamp(option.rect.width() / 3,
                                                       135, 205);
        const auto contentRight =
            right - (groupStatusWidth == 0 ? 0 : groupStatusWidth + 10);
        const auto available = std::max(0, contentRight - left);

        painter->setFont(option.font);
        painter->setPen(locked ? this->muted_ : this->text_);
        const auto titleWidth = std::min(
            metrics.horizontalAdvance(title),
            channel.isEmpty() ? available : std::max(80, available * 3 / 5));
        const QRect titleRect(left, option.rect.top(), titleWidth,
                              option.rect.height());
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(title, Qt::ElideRight, titleWidth));

        if (!channel.isEmpty() && titleWidth < available)
        {
            const auto channelText = QStringLiteral("· %1").arg(channel);
            const auto channelLeft = left + titleWidth + 8;
            const auto channelWidth = std::max(0, contentRight - channelLeft);
            painter->setPen(this->muted_);
            painter->drawText(
                QRect(channelLeft, option.rect.top(), channelWidth,
                      option.rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(channelText, Qt::ElideRight, channelWidth));
        }

        if (!otherGroup.isEmpty())
        {
            const QRect statusRect(contentRight + 10, option.rect.top(),
                                   groupStatusWidth, option.rect.height());
            if (moving)
            {
                painter->setPen(this->accent_);
                painter->drawText(
                    statusRect, Qt::AlignRight | Qt::AlignVCenter,
                    metrics.elidedText(
                        QStringLiteral("Moving from %1").arg(otherGroup),
                        Qt::ElideRight, statusRect.width()));
            }
            else
            {
                const auto action = QStringLiteral("Move here");
                const auto actionWidth = metrics.horizontalAdvance(action);
                const auto groupWidth =
                    std::max(0, statusRect.width() - actionWidth - 10);
                painter->setPen(this->muted_);
                painter->drawText(
                    QRect(statusRect.left(), statusRect.top(), groupWidth,
                          statusRect.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    metrics.elidedText(QStringLiteral("In %1").arg(otherGroup),
                                       Qt::ElideRight, groupWidth));
                painter->setPen(this->accent_);
                painter->drawText(
                    QRect(statusRect.right() - actionWidth + 1,
                          statusRect.top(), actionWidth, statusRect.height()),
                    Qt::AlignRight | Qt::AlignVCenter, action);
            }
        }

        painter->setPen(this->separator_);
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

private:
    QColor background_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor muted_;
    QColor separator_;
    QColor accent_;
};

QPalette tabGroupPalette()
{
    const auto *theme = getTheme();
    auto palette = theme->palette;
    palette.setColor(QPalette::Window, theme->window.background);
    palette.setColor(QPalette::WindowText, theme->window.text);
    palette.setColor(QPalette::Base,
                     theme->tabs.regular.backgrounds.regular);
    palette.setColor(QPalette::AlternateBase,
                     theme->tabs.regular.backgrounds.hover);
    palette.setColor(QPalette::Text, theme->window.text);
    palette.setColor(QPalette::Button,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ButtonText, theme->window.text);
    palette.setColor(QPalette::Highlight,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::HighlightedText, theme->window.text);
    palette.setColor(QPalette::PlaceholderText,
                     theme->messages.textColors.chatPlaceholder);
    palette.setColor(QPalette::Link, theme->accent);
    palette.setColor(QPalette::Mid, theme->tabs.dividerLine);
    palette.setColor(QPalette::ToolTipBase,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ToolTipText, theme->window.text);
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     theme->tabs.regular.backgrounds.unfocused);
    return palette;
}

}

TabGroupDialog::TabGroupDialog(QString groupName,
                               const std::vector<TabGroupDialogEntry> &entries,
                               bool creating, QString icon,
                               QString customIconPath, QWidget *parent)
    : QDialog(parent)
    , customIconPath_(std::move(customIconPath))
    , creating_(creating)
{
    this->setWindowTitle(creating ? "Create tab group" : "Edit tab group");
    const auto palette = tabGroupPalette();
    this->setPalette(palette);
    this->setAutoFillBackground(true);
    this->setMinimumSize(460, 340);
    this->resize(520, 410);
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 11, 12, 11);
    root->setSpacing(8);

    auto *hint = new QLabel(
        creating ? "Choose at least two tabs for this group."
                 : "Choose the tabs you want in this group.",
        this);
    hint->setWordWrap(true);
    hint->setPalette(palette);
    root->addWidget(hint);

    auto *identity = new QHBoxLayout;
    identity->setSpacing(7);
    identity->addWidget(new QLabel("Name", this));
    this->name_ = new QLineEdit(std::move(groupName), this);
    this->name_->setPlaceholderText("Tab count if blank");
    this->name_->setMaxLength(64);
    this->name_->setClearButtonEnabled(true);
    auto inputPalette = palette;
    inputPalette.setColor(QPalette::Base, getTheme()->splits.input.background);
    inputPalette.setColor(QPalette::Text, getTheme()->splits.input.text);
    inputPalette.setColor(QPalette::Disabled, QPalette::Base,
                          getTheme()->splits.input.background);
    this->name_->setPalette(inputPalette);
    identity->addWidget(this->name_, 1);
    identity->addWidget(new QLabel("Icon", this));
    this->icon_ = new QComboBox(this);
    this->icon_->addItem("Folder", "folder");
    this->icon_->addItem("Star", "star");
    this->icon_->addItem("Heart", "heart");
    this->icon_->addItem("Bell", "bell");
    this->icon_->addItem("Shield", "shield");
    this->icon_->addItem("No icon", "none");
    this->icon_->addItem("Custom image", "custom");
    auto iconIndex = this->icon_->findData(icon.toLower());
    this->icon_->setCurrentIndex(iconIndex < 0 ? 0 : iconIndex);
    this->icon_->setMinimumWidth(105);
    identity->addWidget(this->icon_);
    this->chooseIcon_ = new QPushButton(this);
    this->chooseIcon_->setAutoDefault(false);
    this->chooseIcon_->setMaximumWidth(140);
    identity->addWidget(this->chooseIcon_);
    root->addLayout(identity);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(6);
    this->search_ = new QLineEdit(this);
    this->search_->setPlaceholderText("Search tabs");
    this->search_->setClearButtonEnabled(true);
    this->search_->setPalette(inputPalette);
    auto *selectAll = new QPushButton("Select shown", this);
    selectAll->setToolTip(
        "Select shown tabs that are not in another group");
    auto *clear = new QPushButton("Deselect shown", this);
    selectAll->setAutoDefault(false);
    clear->setAutoDefault(false);
    tools->addWidget(this->search_, 1);
    tools->addWidget(selectAll);
    tools->addWidget(clear);
    root->addLayout(tools);

    this->tabs_ = new QListWidget(this);
    this->tabs_->setAlternatingRowColors(false);
    this->tabs_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->tabs_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->tabs_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->tabs_->setFrameShape(QFrame::NoFrame);
    this->tabs_->setMouseTracking(true);
    this->tabs_->installEventFilter(this);
    auto *delegate = new TabGroupRowDelegate(this->tabs_);
    delegate->setColors(
        getTheme()->tabs.regular.backgrounds.regular,
        getTheme()->tabs.regular.backgrounds.hover,
        getTheme()->tabs.selected.backgrounds.regular, getTheme()->window.text,
        getTheme()->tabs.regular.text, getTheme()->tabs.dividerLine,
        getTheme()->accent);
    this->tabs_->setItemDelegate(delegate);
    root->addWidget(this->tabs_, 1);

    this->rows_.reserve(entries.size());
    for (const auto &entry : entries)
    {
        const auto title = entry.title.trimmed().isEmpty()
                               ? QStringLiteral("Untitled tab")
                               : entry.title.trimmed();
        const auto channel = entry.channelTitle.trimmed();
        const auto showChannel =
            !channel.isEmpty() && channel.compare(title, Qt::CaseInsensitive) != 0;

        auto *item = new QListWidgetItem(title, this->tabs_);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setData(CHANNEL_ROLE, showChannel ? channel : QString());
        item->setData(OTHER_GROUP_ROLE, entry.otherGroupName);
        item->setData(MOVING_GROUP_ROLE,
                      !entry.otherGroupName.isEmpty() && entry.selected);
        item->setData(Qt::CheckStateRole,
                      entry.selected ? Qt::Checked : Qt::Unchecked);
        if (!entry.otherGroupName.isEmpty())
        {
            item->setToolTip(
                entry.selected
                    ? QStringLiteral("Moves from %1 when you save.")
                          .arg(entry.otherGroupName)
                    : QStringLiteral("This tab is in %1. Select it to move here.")
                          .arg(entry.otherGroupName));
        }
        item->setSizeHint({0, 34});
        this->rows_.push_back({
            .item = item,
            .page = entry.page,
            .searchText = title + QChar(' ') + channel + QChar(' ') +
                          entry.otherGroupName,
            .otherGroupName = entry.otherGroupName,
        });
    }

    auto *footer = new QHBoxLayout;
    footer->setSpacing(8);
    this->selectionSummary_ = new QLabel(this);
    auto summaryPalette = palette;
    summaryPalette.setColor(QPalette::WindowText,
                            getTheme()->tabs.regular.text);
    this->selectionSummary_->setPalette(summaryPalette);
    footer->addWidget(this->selectionSummary_);
    footer->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    this->acceptButton_ =
        buttons->addButton(creating ? "Create group" : "Save changes",
                           QDialogButtonBox::AcceptRole);
    footer->addWidget(buttons);
    root->addLayout(footer);

    const auto updateIconButton = [this] {
        const auto custom = this->groupIcon() == "custom";
        this->chooseIcon_->setVisible(custom);
        this->chooseIcon_->setText(
            this->customIconPath_.isEmpty()
                ? QStringLiteral("Choose image…")
                : QFileInfo(this->customIconPath_).fileName());
        this->chooseIcon_->setToolTip(
            this->customIconPath_.isEmpty()
                ? QStringLiteral("Choose a custom image")
                : this->customIconPath_);
        this->updateSelectionState();
    };

    QObject::connect(this->icon_, &QComboBox::currentIndexChanged, this,
                     [updateIconButton](int) {
                         updateIconButton();
                     });
    QObject::connect(this->chooseIcon_, &QPushButton::clicked, this, [this] {
        const QPointer<TabGroupDialog> self(this);
        const auto path = QFileDialog::getOpenFileName(
            this, "Choose a group icon", {},
            "Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)");
        if (self && !path.isEmpty())
        {
            this->customIconPath_ = path;
            this->chooseIcon_->setText(QFileInfo(path).fileName());
            this->chooseIcon_->setToolTip(path);
            this->updateSelectionState();
        }
    });
    QObject::connect(this->search_, &QLineEdit::textChanged, this,
                     [this](const QString &query) {
                         this->filterTabs(query);
                     });
    QObject::connect(selectAll, &QPushButton::clicked, this, [this] {
        this->setAllVisibleChecked(true);
    });
    QObject::connect(clear, &QPushButton::clicked, this, [this] {
        this->setAllVisibleChecked(false);
    });
    QObject::connect(this->tabs_, &QListWidget::itemClicked, this,
                     [this](QListWidgetItem *item) {
                         this->toggleItem(item);
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);

    updateIconButton();
    this->search_->setFocus();
}

QString TabGroupDialog::groupName() const
{
    return this->name_->text().trimmed().left(64);
}

QString TabGroupDialog::groupIcon() const
{
    return this->icon_->currentData().toString();
}

QString TabGroupDialog::customIconPath() const
{
    return this->customIconPath_;
}

QList<QWidget *> TabGroupDialog::selectedPages() const
{
    QList<QWidget *> pages;
    for (const auto &row : this->rows_)
    {
        if (row.page && row.item->checkState() == Qt::Checked)
        {
            pages.push_back(row.page);
        }
    }
    return pages;
}

bool TabGroupDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->tabs_ && event->type() == QEvent::KeyPress)
    {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Space ||
            keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter)
        {
            if (auto *item = this->tabs_->currentItem();
                item && !item->isHidden())
            {
                this->toggleItem(item);
            }
            return true;
        }
    }

    return QDialog::eventFilter(watched, event);
}

void TabGroupDialog::filterTabs(const QString &query)
{
    const auto needle = query.trimmed();
    for (auto &row : this->rows_)
    {
        row.item->setHidden(!needle.isEmpty() &&
                            !row.searchText.contains(needle,
                                                     Qt::CaseInsensitive));
    }
}

void TabGroupDialog::setAllVisibleChecked(bool checked)
{
    this->tabs_->setUpdatesEnabled(false);
    for (auto &row : this->rows_)
    {
        if (row.item->isHidden())
        {
            continue;
        }

        if (!row.otherGroupName.isEmpty())
        {
            if (!checked)
            {
                this->updateItemTransferState(row.item, false);
            }
            continue;
        }

        row.item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    this->tabs_->setUpdatesEnabled(true);
    this->updateSelectionState();
}

void TabGroupDialog::toggleItem(QListWidgetItem *item)
{
    if (!item)
    {
        return;
    }

    if (!item->data(OTHER_GROUP_ROLE).toString().isEmpty())
    {
        this->updateItemTransferState(item,
                                      item->checkState() != Qt::Checked);
    }
    else
    {
        item->setCheckState(item->checkState() == Qt::Checked
                                ? Qt::Unchecked
                                : Qt::Checked);
    }
    this->updateSelectionState();
}

void TabGroupDialog::updateItemTransferState(QListWidgetItem *item,
                                             bool moving)
{
    const auto otherGroup = item->data(OTHER_GROUP_ROLE).toString();
    if (otherGroup.isEmpty())
    {
        return;
    }

    item->setData(MOVING_GROUP_ROLE, moving);
    item->setCheckState(moving ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(
        moving
            ? QStringLiteral("Moves from %1 when you save.")
                  .arg(otherGroup)
            : QStringLiteral("This tab is in %1. Select it to move here.")
                  .arg(otherGroup));
}

void TabGroupDialog::updateSelectionState()
{
    int selected = 0;
    for (const auto &row : this->rows_)
    {
        const auto checked = row.item->checkState() == Qt::Checked;
        selected += checked && row.page ? 1 : 0;
    }

    QString summary;
    if (this->creating_ && selected == 0)
    {
        summary = QStringLiteral("Choose at least two tabs");
    }
    else if (this->creating_ && selected == 1)
    {
        summary = QStringLiteral("1 tab selected. Choose one more.");
    }
    else
    {
        summary = selected == 1
                      ? QStringLiteral("1 tab selected")
                      : QStringLiteral("%1 tabs selected").arg(selected);
    }
    this->selectionSummary_->setText(summary);
    const auto iconReady =
        this->groupIcon() != "custom" || !this->customIconPath_.isEmpty();
    this->acceptButton_->setEnabled(
        selected >= (this->creating_ ? 2 : 1) && iconReady);
}

}
