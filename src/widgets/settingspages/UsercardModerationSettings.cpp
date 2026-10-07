#include "widgets/settingspages/UsercardModerationSettings.hpp"

#include "Application.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/hotkeys/HotkeyHelpers.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "widgets/settingspages/CustomWidgets.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDropEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace {

using namespace chatterino;

constexpr int HOTKEY_TIMEOUT_COUNT = 8;
constexpr int ROLE_STABLE_ID = Qt::UserRole;
constexpr int ROLE_SHORTCUT = Qt::UserRole + 1;
constexpr int ROLE_HAS_REASON = Qt::UserRole + 2;

const std::array<TimeoutButton, HOTKEY_TIMEOUT_COUNT> DEFAULT_TIMEOUTS{{
    {"s", 1},
    {"s", 30},
    {"m", 1},
    {"m", 5},
    {"m", 30},
    {"h", 1},
    {"d", 1},
    {"w", 1},
}};

const std::map<QString, int> MAX_DURATION_BY_UNIT{
    {"s", 99}, {"m", 99}, {"h", 99}, {"d", 14}, {"w", 2},
};

QString unitName(const QString &unit, int amount)
{
    const bool singular = amount == 1;
    if (unit == "s")
    {
        return singular ? "second" : "seconds";
    }
    if (unit == "m")
    {
        return singular ? "minute" : "minutes";
    }
    if (unit == "h")
    {
        return singular ? "hour" : "hours";
    }
    if (unit == "d")
    {
        return singular ? "day" : "days";
    }
    return singular ? "week" : "weeks";
}

QString longDuration(const TimeoutButton &button)
{
    return QString("%1 %2")
        .arg(button.second)
        .arg(unitName(button.first, button.second));
}

QString shortDuration(const TimeoutButton &button)
{
    return QString::number(button.second) + button.first;
}

int maximumForUnit(const QString &unit)
{
    const auto it = MAX_DURATION_BY_UNIT.find(unit);
    return it == MAX_DURATION_BY_UNIT.end() ? 99 : it->second;
}

QString actionForReasonPrompt(bool promptForReason)
{
    return promptForReason ? "execModeratorActionWithReason"
                           : "execModeratorAction";
}

QString generatedHotkeyName(const QString &target, bool promptForReason)
{
    QString actionName;
    if (target == "ban" || target == "unban")
    {
        actionName = target;
    }
    else
    {
        actionName = "timeout button " + target;
    }

    QString name = "usercard " + actionName;
    if (promptForReason)
    {
        name += " reason prompt";
    }
    return name;
}

bool isGeneratedHotkeyName(const QString &name, const QString &target,
                           bool promptForReason)
{
    const auto base = generatedHotkeyName(target, promptForReason);
    if (name == base)
    {
        return true;
    }

    const auto prefix = base + ' ';
    if (!name.startsWith(prefix))
    {
        return false;
    }

    bool ok = false;
    const int suffix = name.sliced(prefix.size()).toInt(&ok);
    return ok && suffix >= 2;
}

bool isGeneratedTimeoutHotkeyName(const QString &name, bool promptForReason)
{
    for (int position = 1; position <= HOTKEY_TIMEOUT_COUNT; ++position)
    {
        if (isGeneratedHotkeyName(name, QString::number(position),
                                  promptForReason))
        {
            return true;
        }
    }
    return false;
}

QString shortcutText(const QKeySequence &sequence)
{
    return sequence.toString(QKeySequence::NativeText);
}

class TimeoutListDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        const QFontMetrics metrics(option.font);
        return {240, std::max(36, metrics.height() * 2 + 6)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem panel(option);
        this->initStyleOption(&panel, index);
        panel.text.clear();
        const auto *style = panel.widget != nullptr ? panel.widget->style()
                                                    : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &panel, painter,
                           panel.widget);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const auto primary =
            selected ? option.palette.color(QPalette::HighlightedText)
                     : option.palette.color(QPalette::Text);
        auto secondary = primary;
        secondary.setAlpha(165);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect bounds = option.rect.adjusted(9, 4, -9, -4);
        const int handleX = bounds.left() + 4;
        const int centerY = bounds.center().y();
        QPen handlePen(secondary, 1.25);
        painter->setPen(handlePen);
        for (int offset : {-4, 0, 4})
        {
            painter->drawLine(handleX, centerY + offset, handleX + 8,
                              centerY + offset);
        }

        const int textLeft = handleX + 18;
        const auto mainFont = makeResolvedFont(option.font, QFont::DemiBold);
        painter->setFont(mainFont);
        painter->setPen(primary);
        const QFontMetrics mainMetrics(mainFont);
        painter->drawText(
            QRect(textLeft, bounds.top(), bounds.right() - textLeft,
                  mainMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            index.data(Qt::DisplayRole).toString());

        QFont detailFont = option.font;
        if (detailFont.pointSizeF() > 7)
        {
            detailFont.setPointSizeF(detailFont.pointSizeF() - 1);
        }
        painter->setFont(detailFont);
        painter->setPen(secondary);
        QString detail = index.data(ROLE_SHORTCUT).toString();
        if (index.data(ROLE_HAS_REASON).toBool())
        {
            detail += detail.isEmpty() ? "Saved reason" : "  ·  Saved reason";
        }
        if (detail.isEmpty())
        {
            detail = "No shortcut";
        }
        const QFontMetrics detailMetrics(detailFont);
        const QRect detailRect(
            textLeft, bounds.bottom() - detailMetrics.height() + 1,
            bounds.right() - textLeft, detailMetrics.height());
        painter->drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                          detailMetrics.elidedText(detail, Qt::ElideRight,
                                                   detailRect.width()));
        painter->restore();
    }
};

class TimeoutListWidget final : public QListWidget
{
public:
    using Reordered =
        std::function<void(const std::vector<int> &, const std::vector<int> &)>;

    explicit TimeoutListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        this->setItemDelegate(new TimeoutListDelegate(this));
        this->setSelectionMode(QAbstractItemView::SingleSelection);
        this->setDragEnabled(true);
        this->setAcceptDrops(true);
        this->setDropIndicatorShown(true);
        this->setDragDropMode(QAbstractItemView::InternalMove);
        this->setDefaultDropAction(Qt::MoveAction);
        this->setDragDropOverwriteMode(false);
        this->setUniformItemSizes(true);
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void fitAllRows()
    {
        const auto margins = this->contentsMargins();
        int height = margins.top() + margins.bottom() + this->frameWidth() * 2;
        for (int row = 0; row < this->count(); ++row)
        {
            height += this->sizeHintForRow(row);
        }
        if (this->count() > 1)
        {
            height += this->spacing() * (this->count() - 1);
        }

        this->setFixedHeight(height);
        this->updateGeometry();
    }

    std::vector<int> stableOrder() const
    {
        std::vector<int> order;
        order.reserve(this->count());
        for (int row = 0; row < this->count(); ++row)
        {
            order.push_back(this->item(row)->data(ROLE_STABLE_ID).toInt());
        }
        return order;
    }

    Reordered reordered;

    bool moveCurrent(int offset)
    {
        const int from = this->currentRow();
        const int to = from + offset;
        if (from < 0 || to < 0 || to >= this->count())
        {
            return false;
        }

        const auto before = this->stableOrder();
        auto *item = this->takeItem(from);
        this->insertItem(to, item);
        this->setCurrentItem(item);
        this->notifyIfReordered(before);
        return true;
    }

protected:
    void dropEvent(QDropEvent *event) override
    {
        const auto before = this->stableOrder();
        QListWidget::dropEvent(event);
        this->notifyIfReordered(before);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->modifiers() == Qt::AltModifier &&
            (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down))
        {
            this->moveCurrent(event->key() == Qt::Key_Up ? -1 : 1);
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }

private:
    void notifyIfReordered(const std::vector<int> &before)
    {
        const auto after = this->stableOrder();
        if (before != after && this->reordered)
        {
            this->reordered(before, after);
        }
    }
};

class PhysicalKeySequenceEdit final : public QKeySequenceEdit
{
public:
    explicit PhysicalKeySequenceEdit(QWidget *parent = nullptr)
        : QKeySequenceEdit(parent)
        , lineEdit_(this->findChild<QLineEdit *>(
              QStringLiteral("qt_keysequenceedit_lineedit")))
    {
        QObject::connect(this, &QKeySequenceEdit::keySequenceChanged, this,
                         [this](const QKeySequence &) {
                             if (!this->handlingKeyPress_)
                             {
                                 this->resetRecordingState();
                             }
                         });
        QObject::connect(
            this, &QKeySequenceEdit::editingFinished, this, [this] {
                const auto sequence = this->keySequence();
                const auto canonical = this->canonicalized(sequence);
                this->resetRecordingState();
                if (canonical != sequence)
                {
                    QKeySequenceEdit::setKeySequence(canonical);
                }
            });
    }

    void setExternalKeySequence(const QKeySequence &sequence)
    {
        this->resetRecordingState();
        QKeySequenceEdit::setKeySequence(sequence);
    }

    bool isRecording() const
    {
        return this->recording_;
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event->isAutoRepeat() && !isModifierKey(event->key()) &&
            this->physicalCombinations_.size() <
                static_cast<size_t>(this->maximumSequenceLength()))
        {
            if (!this->recording_)
            {
                this->physicalCombinations_.clear();
                this->recording_ = true;
            }
            this->physicalCombinations_.push_back(
                physicalDigitCombination(event));
        }

        {
            QScopedValueRollback handlingKeyPress(this->handlingKeyPress_,
                                                  true);
            QKeySequenceEdit::keyPressEvent(event);
        }
        this->refreshRecordingText();
    }

private:
    static bool isModifierKey(int key)
    {
        return key == Qt::Key_Control || key == Qt::Key_Shift ||
               key == Qt::Key_Meta || key == Qt::Key_Alt ||
               key == Qt::Key_unknown;
    }

    static std::optional<QKeyCombination> physicalDigitCombination(
        const QKeyEvent *event)
    {
        if (!event->modifiers().testFlag(Qt::ShiftModifier))
        {
            return std::nullopt;
        }

        const auto digit = physicalNumberRowKey(
            QGuiApplication::platformName(), event->key(),
            event->nativeScanCode(), event->nativeVirtualKey());
        if (!digit)
        {
            return std::nullopt;
        }

        return QKeyCombination(event->modifiers(), *digit);
    }

    QKeySequence canonicalized(const QKeySequence &sequence) const
    {
        std::array<QKeyCombination, 4> combinations{};
        const int count = std::min(sequence.count(), 4);
        for (int index = 0; index < count; ++index)
        {
            combinations[index] =
                index < static_cast<int>(this->physicalCombinations_.size()) &&
                        this->physicalCombinations_[index]
                    ? *this->physicalCombinations_[index]
                    : sequence[index];
        }

        switch (count)
        {
            case 0:
                return {};
            case 1:
                return {combinations[0]};
            case 2:
                return {combinations[0], combinations[1]};
            case 3:
                return {combinations[0], combinations[1], combinations[2]};
            case 4:
            default:
                return {combinations[0], combinations[1], combinations[2],
                        combinations[3]};
        }
    }

    void refreshRecordingText()
    {
        if (!this->recording_ || this->lineEdit_ == nullptr)
        {
            return;
        }

        const auto sequence = this->canonicalized(this->keySequence());
        auto text = sequence.toString(QKeySequence::NativeText);
        if (sequence.count() < this->maximumSequenceLength())
        {
            text += ", ...";
        }
        this->lineEdit_->setText(text);
    }

    void resetRecordingState()
    {
        this->recording_ = false;
        this->physicalCombinations_.clear();
    }

    QLineEdit *lineEdit_ = nullptr;
    std::vector<std::optional<QKeyCombination>> physicalCombinations_;
    bool recording_ = false;
    bool handlingKeyPress_ = false;
};

QLabel *makeMutedLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new SLabel(text, parent);
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText,
                     palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    label->setWordWrap(true);
    return label;
}

QFrame *makePanel(QWidget *parent = nullptr)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("usercardModerationSection"));
    panel->setFrameShape(QFrame::NoFrame);
    panel->setStyleSheet(QStringLiteral(R"(
        QFrame#usercardModerationSection {
            background-color: transparent;
            border: none;
        }
    )"));
    return panel;
}

}

namespace chatterino {

struct UsercardModerationSettings::Impl {
    struct TimeoutEntry {
        TimeoutButton button;
        QString reason;
        int stableID = -1;
    };

    struct ShortcutEditorState {
        QString target;
        bool promptForReason = false;
        QString hotkeyName;
    };

    explicit Impl(UsercardModerationSettings *owner)
        : owner(owner)
    {
        this->loadEntries();
        this->buildUi();

        this->signalHolder.managedConnect(
            getApp()->getHotkeys()->onItemsUpdated, [this] {
                this->refreshHotkeyUi();
            });
        const auto reloadTimeouts = [this] {
            if (this->updating || this->entriesRefreshQueued)
            {
                return;
            }
            this->entriesRefreshQueued = true;

            QTimer::singleShot(0, this->owner, [this] {
                this->entriesRefreshQueued = false;
                const QScopedValueRollback updating(this->updating, true);
                const auto row = this->timeoutList->currentRow();
                this->loadEntries();
                this->populateTimeoutList();
                this->timeoutList->setCurrentRow(
                    std::clamp(row, 0, this->timeoutList->count() - 1));
                this->refreshPreview();
            });
        };
        getSettings()->timeoutButtons.connect(reloadTimeouts,
                                              this->signalHolder, false);
        getSettings()->timeoutButtonReasons.connect(reloadTimeouts,
                                                    this->signalHolder, false);
    }

    void loadEntries()
    {
        this->entries.clear();
        const auto buttons = getSettings()->timeoutButtons.getValue();
        const auto reasons = getSettings()->timeoutButtonReasons.getValue();

        this->entries.reserve(DEFAULT_TIMEOUTS.size());
        const auto count = DEFAULT_TIMEOUTS.size();
        for (size_t index = 0; index < count; ++index)
        {
            TimeoutEntry entry;
            entry.button = index < buttons.size() ? buttons[index]
                                                  : DEFAULT_TIMEOUTS[index];
            if (!MAX_DURATION_BY_UNIT.contains(entry.button.first))
            {
                entry.button.first = "s";
            }
            entry.button.second = std::clamp(
                entry.button.second, 1, maximumForUnit(entry.button.first));
            if (index < reasons.size())
            {
                entry.reason = reasons[index];
            }
            entry.stableID = static_cast<int>(index);
            this->entries.push_back(std::move(entry));
        }

        std::vector<TimeoutButton> normalizedButtons;
        std::vector<QString> normalizedReasons;
        normalizedButtons.reserve(this->entries.size());
        normalizedReasons.reserve(this->entries.size());
        for (const auto &entry : this->entries)
        {
            normalizedButtons.push_back(entry.button);
            normalizedReasons.push_back(entry.reason);
        }
        while (!normalizedReasons.empty() &&
               normalizedReasons.back().trimmed().isEmpty())
        {
            normalizedReasons.pop_back();
        }

        if (buttons != normalizedButtons)
        {
            getSettings()->timeoutButtons.setValue(normalizedButtons);
        }
        if (reasons != normalizedReasons)
        {
            getSettings()->timeoutButtonReasons.setValue(normalizedReasons);
        }
    }

    void buildUi()
    {
        auto *outer = new QVBoxLayout(this->owner);
        outer->setContentsMargins(0, 0, 0, 0);

        auto *scroll = new QScrollArea(this->owner);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll);
        content->setAutoFillBackground(true);
        auto *root = new QVBoxLayout(content);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(0);
        scroll->setWidget(content);
        outer->addWidget(scroll);

        auto *intro = new SLabel(
            "Edit timeout buttons, reasons, and shortcuts. Changes also apply "
            "to usercards and Hotkeys.",
            this->owner);
        intro->setWordWrap(true);
        root->addWidget(intro);
        root->addSpacing(10);

        this->buildPreview(root);

        auto *workspace = new QHBoxLayout;
        workspace->setSpacing(10);

        auto *listPanel = makePanel(this->owner);
        auto *listColumn = new QVBoxLayout(listPanel);
        listColumn->setContentsMargins(10, 8, 10, 10);
        listColumn->setSpacing(5);

        auto *listHeader = new QHBoxLayout;
        auto *listTitle = new SLabel("Timeout buttons", listPanel);
        listTitle->setFont(
            makeResolvedFont(listTitle->font(), QFont::DemiBold));
        listHeader->addWidget(listTitle);
        listHeader->addStretch();
        listHeader->addWidget(makeMutedLabel("Drag to reorder", listPanel));
        this->moveUp = new QToolButton(listPanel);
        this->moveUp->setAutoRaise(true);
        this->moveUp->setAccessibleName(QStringLiteral("Move timeout up"));
        this->moveUp->setIcon(
            listPanel->style()->standardIcon(QStyle::SP_ArrowUp));
        this->moveUp->setToolTip("Move the selected timeout up");
        listHeader->addWidget(this->moveUp);
        this->moveDown = new QToolButton(listPanel);
        this->moveDown->setAutoRaise(true);
        this->moveDown->setAccessibleName(QStringLiteral("Move timeout down"));
        this->moveDown->setIcon(
            listPanel->style()->standardIcon(QStyle::SP_ArrowDown));
        this->moveDown->setToolTip("Move the selected timeout down");
        listHeader->addWidget(this->moveDown);
        listColumn->addLayout(listHeader);

        this->timeoutList = new TimeoutListWidget(listPanel);
        this->timeoutList->setMinimumWidth(190);
        this->timeoutList->setToolTip(
            "Drag a timeout to move it, or use Alt+Up and Alt+Down.");
        listColumn->addWidget(this->timeoutList, 1);
        workspace->addWidget(listPanel, 2);

        auto *actionColumn = new QVBoxLayout;
        actionColumn->setSpacing(10);

        auto *editorPanel = makePanel(this->owner);
        auto *editorColumn = new QVBoxLayout(editorPanel);
        editorColumn->setContentsMargins(10, 8, 10, 10);
        editorColumn->setSpacing(8);
        this->selectedTitle = new SLabel(editorPanel);
        this->selectedTitle->setFont(
            makeResolvedFont(this->selectedTitle->font(), QFont::DemiBold));
        editorColumn->addWidget(this->selectedTitle);

        auto *editorGrid = new QGridLayout;
        editorGrid->setHorizontalSpacing(8);
        editorGrid->setVerticalSpacing(8);

        editorGrid->addWidget(new SLabel("Duration", editorPanel), 0, 0);
        auto *durationRow = new QHBoxLayout;
        durationRow->setSpacing(5);
        this->durationInput = new QSpinBox(editorPanel);
        this->durationInput->setMinimum(1);
        this->durationInput->setKeyboardTracking(false);
        this->durationInput->setMinimumWidth(78);
        durationRow->addWidget(this->durationInput);
        this->unitInput = new SComboBox(editorPanel);
        this->unitInput->addItem("Seconds", "s");
        this->unitInput->addItem("Minutes", "m");
        this->unitInput->addItem("Hours", "h");
        this->unitInput->addItem("Days", "d");
        this->unitInput->addItem("Weeks", "w");
        durationRow->addWidget(this->unitInput, 1);
        editorGrid->addLayout(durationRow, 0, 1);

        editorGrid->addWidget(new SLabel("Saved reason", editorPanel), 1, 0);
        this->reasonInput = new QLineEdit(editorPanel);
        this->reasonInput->setPlaceholderText("Optional reason");
        this->reasonInput->setClearButtonEnabled(true);
        this->reasonInput->setMinimumWidth(90);
        this->reasonInput->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);
        editorGrid->addWidget(this->reasonInput, 1, 1);

        editorGrid->addWidget(new SLabel("Shortcut", editorPanel), 2, 0);
        auto *shortcutRow = new QHBoxLayout;
        shortcutRow->setSpacing(6);
        this->timeoutShortcutMode = this->makeShortcutMode(editorPanel);
        this->timeoutShortcutMode->setMinimumWidth(100);
        this->timeoutShortcutMode->setMaximumWidth(120);
        shortcutRow->addWidget(this->timeoutShortcutMode);
        this->timeoutShortcut = this->makeShortcutEditor(editorPanel);
        shortcutRow->addWidget(this->timeoutShortcut, 1);
        editorGrid->addLayout(shortcutRow, 2, 1);

        this->timeoutShortcutInfo = makeMutedLabel({}, editorPanel);
        editorGrid->addWidget(this->timeoutShortcutInfo, 3, 1);
        editorGrid->setColumnStretch(1, 1);
        editorColumn->addLayout(editorGrid);
        actionColumn->addWidget(editorPanel);
        actionColumn->addWidget(this->buildBanAndUnban());
        actionColumn->addWidget(this->buildReasonPromptOptions());
        workspace->addLayout(actionColumn, 3);
        root->addLayout(workspace);
        root->addStretch(1);

        this->populateTimeoutList();
        this->timeoutList->reordered = [this](const std::vector<int> &before,
                                              const std::vector<int> &after) {
            this->reorderEntries(before, after);
        };

        QObject::connect(this->timeoutList, &QListWidget::currentRowChanged,
                         this->owner, [this](int row) {
                             this->loadSelectedTimeout(row);
                             this->updateMoveButtons();
                         });
        QObject::connect(this->moveUp, &QToolButton::clicked, this->owner,
                         [this] {
                             this->timeoutList->moveCurrent(-1);
                         });
        QObject::connect(this->moveDown, &QToolButton::clicked, this->owner,
                         [this] {
                             this->timeoutList->moveCurrent(1);
                         });
        QObject::connect(
            this->durationInput, qOverload<int>(&QSpinBox::valueChanged),
            this->owner, [this](int value) {
                if (this->updating || !this->hasSelectedTimeout())
                {
                    return;
                }
                this->entries[this->timeoutList->currentRow()].button.second =
                    value;
                this->persistEntries();
                this->refreshTimeoutRow(this->timeoutList->currentRow());
                this->refreshPreview();
            });
        QObject::connect(
            this->unitInput, &QComboBox::currentIndexChanged, this->owner,
            [this](int) {
                if (this->updating || !this->hasSelectedTimeout())
                {
                    return;
                }
                const auto unit = this->unitInput->currentData().toString();
                auto &entry = this->entries[this->timeoutList->currentRow()];
                entry.button.first = unit;
                const QSignalBlocker durationBlocker(this->durationInput);
                this->durationInput->setMaximum(maximumForUnit(unit));
                entry.button.second = this->durationInput->value();
                this->persistEntries();
                this->refreshTimeoutRow(this->timeoutList->currentRow());
                this->refreshPreview();
            });
        QObject::connect(
            this->reasonInput, &QLineEdit::textChanged, this->owner,
            [this](const QString &reason) {
                if (this->updating || !this->hasSelectedTimeout())
                {
                    return;
                }
                this->entries[this->timeoutList->currentRow()].reason = reason;
                this->persistEntries();
                this->refreshTimeoutRow(this->timeoutList->currentRow());
                this->refreshPreview();
            });
        QObject::connect(
            this->timeoutShortcutMode, &QComboBox::currentIndexChanged,
            this->owner, [this](int) {
                if (!this->updating && this->hasSelectedTimeout())
                {
                    this->changeShortcutMode(
                        QString::number(this->timeoutList->currentRow() + 1),
                        this->timeoutShortcutMode->currentData().toBool(),
                        this->timeoutShortcutMode, this->timeoutShortcut,
                        this->timeoutShortcutInfo, this->timeoutShortcutState);
                }
            });
        this->connectShortcutEditor(this->timeoutShortcut, [this] {
            if (this->hasSelectedTimeout())
            {
                this->saveShortcut(
                    QString::number(this->timeoutList->currentRow() + 1),
                    this->timeoutShortcutMode->currentData().toBool(),
                    this->timeoutShortcut->keySequence(), this->timeoutShortcut,
                    this->timeoutShortcutInfo, this->timeoutShortcutState);
            }
        });

        if (this->timeoutList->count() > 0)
        {
            this->timeoutList->setCurrentRow(0);
        }
    }

    void buildPreview(QVBoxLayout *root)
    {
        auto *panel = makePanel(this->owner);
        auto *panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(10, 8, 10, 9);
        panelLayout->setSpacing(6);

        auto *title = new SLabel("Usercard preview", panel);
        title->setFont(makeResolvedFont(title->font(), QFont::DemiBold));
        panelLayout->addWidget(title);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(14);
        actions->addStretch();

        auto *unbanColumn = new QVBoxLayout;
        unbanColumn->setSpacing(3);
        auto *unbanTitle = new SLabel("Unban", panel);
        unbanTitle->setAlignment(Qt::AlignCenter);
        unbanColumn->addWidget(unbanTitle);
        this->previewUnban = new QToolButton(panel);
        this->previewUnban->setIcon(QIcon(getResources().buttons.unban));
        this->previewUnban->setIconSize({22, 22});
        this->previewUnban->setFixedSize(34, 30);
        unbanColumn->addWidget(this->previewUnban, 0, Qt::AlignCenter);
        actions->addLayout(unbanColumn);

        auto *timeoutsColumn = new QVBoxLayout;
        timeoutsColumn->setSpacing(3);
        auto *timeoutsTitle = new SLabel("Timeouts", panel);
        timeoutsTitle->setAlignment(Qt::AlignCenter);
        timeoutsColumn->addWidget(timeoutsTitle);
        this->previewTimeoutLayout = new QHBoxLayout;
        this->previewTimeoutLayout->setSpacing(0);
        timeoutsColumn->addLayout(this->previewTimeoutLayout);
        actions->addLayout(timeoutsColumn);

        auto *banColumn = new QVBoxLayout;
        banColumn->setSpacing(3);
        auto *banTitle = new SLabel("Ban", panel);
        banTitle->setAlignment(Qt::AlignCenter);
        banColumn->addWidget(banTitle);
        this->previewBan = new QToolButton(panel);
        this->previewBan->setIcon(QIcon(getResources().buttons.ban));
        this->previewBan->setIconSize({22, 22});
        this->previewBan->setFixedSize(34, 30);
        banColumn->addWidget(this->previewBan, 0, Qt::AlignCenter);
        actions->addLayout(banColumn);
        actions->addStretch();
        panelLayout->addLayout(actions);

        QObject::connect(this->previewUnban, &QToolButton::clicked, this->owner,
                         [this] {
                             if (this->unbanShortcut != nullptr)
                             {
                                 this->unbanShortcut->setFocus();
                             }
                         });
        QObject::connect(this->previewBan, &QToolButton::clicked, this->owner,
                         [this] {
                             if (this->banShortcut != nullptr)
                             {
                                 this->banShortcut->setFocus();
                             }
                         });

        root->addWidget(panel);
        this->refreshPreview();
    }

    QFrame *buildBanAndUnban()
    {
        auto *panel = makePanel(this->owner);
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(10, 7, 10, 8);
        layout->setSpacing(5);

        auto *title = new SLabel("Ban and unban", panel);
        title->setFont(makeResolvedFont(title->font(), QFont::DemiBold));
        layout->addWidget(title);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(5);
        grid->addWidget(new SLabel("Ban reason", panel), 0, 0);
        this->banReason = new QLineEdit(panel);
        this->banReason->setPlaceholderText("Optional reason");
        this->banReason->setClearButtonEnabled(true);
        this->banReason->setMinimumWidth(90);
        this->banReason->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Fixed);
        this->banReason->setText(getSettings()->timeoutBanReason.getValue());
        grid->addWidget(this->banReason, 0, 1, 1, 2);

        grid->addWidget(new SLabel("Ban shortcut", panel), 1, 0);
        this->banShortcutMode = this->makeShortcutMode(panel);
        this->banShortcutMode->setMinimumWidth(100);
        this->banShortcutMode->setMaximumWidth(120);
        grid->addWidget(this->banShortcutMode, 1, 1);
        this->banShortcut = this->makeShortcutEditor(panel);
        grid->addWidget(this->banShortcut, 1, 2);

        grid->addWidget(new SLabel("Unban shortcut", panel), 3, 0);
        this->unbanShortcut = this->makeShortcutEditor(panel);
        grid->addWidget(this->unbanShortcut, 3, 1, 1, 2);

        this->banShortcutInfo = makeMutedLabel({}, panel);
        this->unbanShortcutInfo = makeMutedLabel({}, panel);
        grid->addWidget(this->banShortcutInfo, 2, 1, 1, 2);
        grid->addWidget(this->unbanShortcutInfo, 4, 1, 1, 2);
        grid->setColumnStretch(2, 1);
        layout->addLayout(grid);

        QObject::connect(this->banReason, &QLineEdit::textChanged, this->owner,
                         [this](const QString &reason) {
                             getSettings()->timeoutBanReason = reason;
                             this->refreshPreview();
                         });
        getSettings()->timeoutBanReason.connect(
            [this](const QString &reason) {
                const QSignalBlocker blocker(this->banReason);
                this->banReason->setText(reason);
                this->refreshPreview();
            },
            this->signalHolder, false);
        QObject::connect(
            this->banShortcutMode, &QComboBox::currentIndexChanged, this->owner,
            [this](int) {
                if (!this->updating)
                {
                    this->changeShortcutMode(
                        "ban", this->banShortcutMode->currentData().toBool(),
                        this->banShortcutMode, this->banShortcut,
                        this->banShortcutInfo, this->banShortcutState);
                }
            });
        this->connectShortcutEditor(this->banShortcut, [this] {
            this->saveShortcut(
                "ban", this->banShortcutMode->currentData().toBool(),
                this->banShortcut->keySequence(), this->banShortcut,
                this->banShortcutInfo, this->banShortcutState);
        });
        this->connectShortcutEditor(this->unbanShortcut, [this] {
            this->saveShortcut("unban", false,
                               this->unbanShortcut->keySequence(),
                               this->unbanShortcut, this->unbanShortcutInfo,
                               this->unbanShortcutState);
        });

        this->chooseInitialShortcutMode("ban", this->banShortcutMode);
        this->loadBanShortcut();
        this->loadUnbanShortcut();
        return panel;
    }

    QFrame *buildReasonPromptOptions()
    {
        auto *panel = makePanel(this->owner);
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(10, 8, 10, 10);
        layout->setSpacing(6);

        auto *title = new SLabel("Reason prompt", panel);
        title->setFont(makeResolvedFont(title->font(), QFont::DemiBold));
        layout->addWidget(title);
        layout->addWidget(makeMutedLabel(
            "Choose when to review a reason before the action is sent.",
            panel));

        auto *options = new QGridLayout;
        options->setContentsMargins(0, 0, 0, 0);
        options->setHorizontalSpacing(12);
        options->setVerticalSpacing(5);

        auto *rightClick = new SCheckBox("Review on right click", panel);
        rightClick->setChecked(
            getSettings()->timeoutReasonPromptOnRightClick.getValue());
        rightClick->setToolTip(
            "Right click a timeout or ban button to review its reason before "
            "sending it.");
        options->addWidget(rightClick, 0, 0);

        auto *modifierRow = new QHBoxLayout;
        modifierRow->setSpacing(6);
        auto *modifier = new SCheckBox("Hold", panel);
        modifier->setChecked(
            getSettings()->timeoutReasonPromptOnModifier.getValue());
        modifier->setToolTip(
            "Hold this key while selecting a timeout or ban action to review "
            "its reason.");
        modifierRow->addWidget(modifier);
        auto *modifierKey = new SComboBox(panel);
        modifierKey->addItems({"Shift", "Ctrl", "Alt"});
        modifierKey->setCurrentText(
            getSettings()->timeoutReasonPromptModifier.getValue());
        modifierKey->setMaximumWidth(120);
        modifierRow->addWidget(modifierKey);
        modifierRow->addWidget(new SLabel("to review", panel));
        modifierRow->addStretch();
        options->addLayout(modifierRow, 0, 1);

        auto *prefill = new SCheckBox("Start with the saved reason", panel);
        prefill->setChecked(
            getSettings()->timeoutReasonPromptPrefillSavedReason.getValue());
        prefill->setToolTip(
            "The saved reason is selected so you can replace it quickly.");
        options->addWidget(prefill, 1, 0);

        auto *showSend = new SCheckBox("Show the Send button", panel);
        showSend->setChecked(
            getSettings()->timeoutReasonPromptShowSendButton.getValue());
        showSend->setToolTip(
            "You can still press Enter to send or Escape to cancel.");
        options->addWidget(showSend, 1, 1);
        options->setColumnStretch(0, 1);
        options->setColumnStretch(1, 1);
        layout->addLayout(options);

        modifierKey->setEnabled(modifier->isChecked());
        QObject::connect(
            rightClick, &QCheckBox::toggled, this->owner, [](bool checked) {
                getSettings()->timeoutReasonPromptOnRightClick = checked;
            });
        QObject::connect(modifier, &QCheckBox::toggled, this->owner,
                         [](bool checked) {
                             getSettings()->timeoutReasonPromptOnModifier =
                                 checked;
                         });
        QObject::connect(modifierKey, &QComboBox::currentTextChanged,
                         this->owner, [](const QString &key) {
                             getSettings()->timeoutReasonPromptModifier = key;
                         });
        QObject::connect(
            prefill, &QCheckBox::toggled, this->owner, [](bool checked) {
                getSettings()->timeoutReasonPromptPrefillSavedReason = checked;
            });
        QObject::connect(
            showSend, &QCheckBox::toggled, this->owner, [](bool checked) {
                getSettings()->timeoutReasonPromptShowSendButton = checked;
            });

        const auto bindCheckbox = [this](BoolSetting &setting,
                                         QCheckBox *checkbox) {
            setting.connect(
                [checkbox](bool checked) {
                    const QSignalBlocker blocker(checkbox);
                    checkbox->setChecked(checked);
                },
                this->signalHolder, false);
        };
        bindCheckbox(getSettings()->timeoutReasonPromptOnRightClick, rightClick);
        bindCheckbox(getSettings()->timeoutReasonPromptOnModifier, modifier);
        bindCheckbox(getSettings()->timeoutReasonPromptPrefillSavedReason,
                     prefill);
        bindCheckbox(getSettings()->timeoutReasonPromptShowSendButton, showSend);
        getSettings()->timeoutReasonPromptOnModifier.connect(
            [modifierKey](bool enabled) {
                modifierKey->setEnabled(enabled);
            },
            this->signalHolder, false);
        getSettings()->timeoutReasonPromptModifier.connect(
            [modifierKey](const QString &key) {
                const QSignalBlocker blocker(modifierKey);
                modifierKey->setCurrentText(key);
            },
            this->signalHolder, false);

        return panel;
    }

    QComboBox *makeShortcutMode(QWidget *parent)
    {
        auto *combo = new SComboBox(parent);
        combo->addItem("Send now", false);
        combo->addItem("Review first", true);
        combo->setToolTip(
            "Choose whether this shortcut sends the saved reason immediately "
            "or lets you review it first.");
        return combo;
    }

    PhysicalKeySequenceEdit *makeShortcutEditor(QWidget *parent)
    {
        auto *editor = new PhysicalKeySequenceEdit(parent);
        editor->setClearButtonEnabled(true);
        editor->setMinimumWidth(90);
        editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        editor->setToolTip(
            "Press a key combination. You can add more than one step.");
        return editor;
    }

    void connectShortcutEditor(PhysicalKeySequenceEdit *editor,
                               std::function<void()> save)
    {
        QObject::connect(editor, &QKeySequenceEdit::editingFinished,
                         this->owner, [this, save] {
                             if (!this->updating)
                             {
                                 save();
                             }
                         });
        QObject::connect(
            editor, &QKeySequenceEdit::keySequenceChanged, this->owner,
            [this, editor, save](const QKeySequence &sequence) {
                if (!this->updating && sequence.isEmpty())
                {
                    QTimer::singleShot(0, editor, [this, editor, save] {
                        if (!this->updating && !editor->isRecording() &&
                            editor->keySequence().isEmpty())
                        {
                            save();
                        }
                    });
                }
            });
    }

    bool hasSelectedTimeout() const
    {
        const int row = this->timeoutList->currentRow();
        return row >= 0 && row < static_cast<int>(this->entries.size());
    }

    void populateTimeoutList()
    {
        this->timeoutList->clear();
        for (const auto &entry : this->entries)
        {
            auto *item = new QListWidgetItem(this->timeoutList);
            item->setData(ROLE_STABLE_ID, entry.stableID);
        }
        this->refreshAllTimeoutRows();
        this->timeoutList->fitAllRows();
    }

    void refreshAllTimeoutRows()
    {
        for (int row = 0; row < this->timeoutList->count(); ++row)
        {
            this->refreshTimeoutRow(row);
        }
    }

    void refreshTimeoutRow(int row)
    {
        if (row < 0 || row >= this->timeoutList->count() ||
            row >= static_cast<int>(this->entries.size()))
        {
            return;
        }
        const auto &entry = this->entries[row];
        auto *item = this->timeoutList->item(row);
        item->setText(longDuration(entry.button));
        item->setData(ROLE_HAS_REASON, !entry.reason.trimmed().isEmpty());

        QStringList shortcuts;
        for (const bool prompt : {false, true})
        {
            const auto sequence = getApp()->getHotkeys()->getDisplaySequence(
                HotkeyCategory::PopupWindow, actionForReasonPrompt(prompt),
                std::vector<QString>{QString::number(row + 1)});
            if (!sequence.isEmpty())
            {
                shortcuts.push_back(shortcutText(sequence) +
                                    (prompt ? " (review first)" : QString{}));
            }
        }
        item->setData(ROLE_SHORTCUT, shortcuts.join("  ·  "));
        item->setToolTip(this->timeoutTooltip(row, true));
    }

    void loadSelectedTimeout(int row)
    {
        if (row < 0 || row >= static_cast<int>(this->entries.size()))
        {
            return;
        }
        const QScopedValueRollback updating(this->updating, true);
        const auto &entry = this->entries[row];
        this->selectedTitle->setText(QString("Timeout button %1").arg(row + 1));
        const int unitIndex = this->unitInput->findData(entry.button.first);
        this->unitInput->setCurrentIndex(std::max(0, unitIndex));
        this->durationInput->setMaximum(maximumForUnit(entry.button.first));
        this->durationInput->setValue(entry.button.second);
        this->reasonInput->setText(entry.reason);
        this->chooseInitialShortcutMode(QString::number(row + 1),
                                        this->timeoutShortcutMode);
        this->loadTimeoutShortcut();
        this->syncPreviewSelection(row);
    }

    void persistEntries()
    {
        const QScopedValueRollback updating(this->updating, true);
        std::vector<TimeoutButton> buttons;
        std::vector<QString> reasons;
        buttons.reserve(this->entries.size());
        reasons.reserve(this->entries.size());
        for (const auto &entry : this->entries)
        {
            buttons.push_back(entry.button);
            reasons.push_back(entry.reason);
        }
        while (!reasons.empty() && reasons.back().trimmed().isEmpty())
        {
            reasons.pop_back();
        }
        getSettings()->timeoutButtons.setValue(buttons);
        getSettings()->timeoutButtonReasons.setValue(reasons);
    }

    void reorderEntries(const std::vector<int> &before,
                        const std::vector<int> &after)
    {
        if (before.size() != after.size() || before == after)
        {
            return;
        }

        std::map<int, TimeoutEntry> byStableID;
        for (const auto &entry : this->entries)
        {
            byStableID.emplace(entry.stableID, entry);
        }

        std::vector<TimeoutEntry> reordered;
        reordered.reserve(after.size());
        for (const int stableID : after)
        {
            const auto found = byStableID.find(stableID);
            if (found == byStableID.end())
            {
                return;
            }
            reordered.push_back(found->second);
        }

        std::vector<int> oldToNew(before.size(), -1);
        for (size_t oldIndex = 0; oldIndex < before.size(); ++oldIndex)
        {
            const auto found =
                std::find(after.begin(), after.end(), before[oldIndex]);
            if (found == after.end())
            {
                return;
            }
            oldToNew[oldIndex] =
                static_cast<int>(std::distance(after.begin(), found));
        }

        this->entries = std::move(reordered);
        this->persistEntries();
        this->remapTimeoutHotkeys(oldToNew);
        this->refreshAllTimeoutRows();
        this->refreshPreview();
        this->loadSelectedTimeout(this->timeoutList->currentRow());
        this->updateMoveButtons();
    }

    void updateMoveButtons()
    {
        const int row = this->timeoutList->currentRow();
        this->moveUp->setEnabled(row > 0);
        this->moveDown->setEnabled(row >= 0 &&
                                   row + 1 < this->timeoutList->count());
    }

    void remapTimeoutHotkeys(const std::vector<int> &oldToNew)
    {
        auto *controller = getApp()->getHotkeys();
        struct PlannedHotkey {
            std::shared_ptr<Hotkey> original;
            std::vector<QString> arguments;
            bool promptForReason = false;
            QString name;
            bool generatedName = false;
        };

        std::vector<PlannedHotkey> plans;
        for (const bool prompt : {false, true})
        {
            const auto action = actionForReasonPrompt(prompt);
            const auto matches =
                controller->getHotkeys(HotkeyCategory::PopupWindow, action);
            for (const auto &hotkey : matches)
            {
                const auto remapped =
                    remapIndexedHotkeyArguments(hotkey->arguments(), oldToNew);
                if (remapped == hotkey->arguments())
                {
                    continue;
                }
                plans.push_back(
                    {hotkey, remapped, prompt, hotkey->name(),
                     isGeneratedTimeoutHotkeyName(hotkey->name(), prompt)});
            }
        }

        if (plans.empty())
        {
            return;
        }

        std::set<QString> namesBeingReplaced;
        std::set<QString> reservedNames;
        for (const auto &plan : plans)
        {
            namesBeingReplaced.insert(plan.original->name());
            if (!plan.generatedName)
            {
                reservedNames.insert(plan.name);
            }
        }

        for (auto &plan : plans)
        {
            if (!plan.generatedName)
            {
                continue;
            }

            const auto base = generatedHotkeyName(plan.arguments.front(),
                                                  plan.promptForReason);
            auto candidate = base;
            int suffix = 2;
            while (reservedNames.contains(candidate) ||
                   (controller->getHotkeyByName(candidate) != nullptr &&
                    !namesBeingReplaced.contains(candidate)))
            {
                candidate = base + ' ' + QString::number(suffix++);
            }
            plan.name = candidate;
            reservedNames.insert(candidate);
        }

        for (const auto &plan : plans)
        {
            controller->removeHotkey(plan.original->name());
        }
        for (const auto &plan : plans)
        {
            controller->addHotkey(std::make_shared<Hotkey>(
                plan.original->category(), plan.original->keySequence(),
                plan.original->action(), plan.arguments, plan.name));
        }
        controller->save();
    }

    void chooseInitialShortcutMode(const QString &target, QComboBox *combo)
    {
        const auto immediate = getApp()->getHotkeys()->getHotkeys(
            HotkeyCategory::PopupWindow, actionForReasonPrompt(false),
            std::vector<QString>{target});
        const auto prompted = getApp()->getHotkeys()->getHotkeys(
            HotkeyCategory::PopupWindow, actionForReasonPrompt(true),
            std::vector<QString>{target});

        QSignalBlocker blocker(combo);
        if (immediate.empty() != prompted.empty())
        {
            combo->setCurrentIndex(combo->findData(immediate.empty()));
        }
        else if (combo->currentIndex() < 0)
        {
            combo->setCurrentIndex(combo->findData(false));
        }
    }

    void loadTimeoutShortcut()
    {
        if (!this->hasSelectedTimeout())
        {
            return;
        }
        this->loadShortcut(QString::number(this->timeoutList->currentRow() + 1),
                           this->timeoutShortcutMode->currentData().toBool(),
                           this->timeoutShortcut, this->timeoutShortcutInfo,
                           this->timeoutShortcutState);
    }

    void loadBanShortcut()
    {
        this->loadShortcut("ban", this->banShortcutMode->currentData().toBool(),
                           this->banShortcut, this->banShortcutInfo,
                           this->banShortcutState);
    }

    void loadUnbanShortcut()
    {
        this->loadShortcut("unban", false, this->unbanShortcut,
                           this->unbanShortcutInfo, this->unbanShortcutState);
    }

    void loadShortcut(const QString &target, bool promptForReason,
                      PhysicalKeySequenceEdit *editor, QLabel *info,
                      ShortcutEditorState &state,
                      const QString &preferredHotkeyName = {})
    {
        const auto matches = getApp()->getHotkeys()->getHotkeys(
            HotkeyCategory::PopupWindow, actionForReasonPrompt(promptForReason),
            std::vector<QString>{target});

        QString requestedName = preferredHotkeyName;
        if (requestedName.isEmpty() && state.target == target &&
            state.promptForReason == promptForReason)
        {
            requestedName = state.hotkeyName;
        }

        std::shared_ptr<Hotkey> selected;
        if (!requestedName.isEmpty())
        {
            const auto found =
                std::find_if(matches.begin(), matches.end(),
                             [&requestedName](const auto &hotkey) {
                                 return hotkey->name() == requestedName;
                             });
            if (found != matches.end())
            {
                selected = *found;
            }
        }
        if (selected == nullptr && !matches.empty())
        {
            selected = matches.front();
        }

        state = {target, promptForReason,
                 selected != nullptr ? selected->name() : QString{}};
        QSignalBlocker blocker(editor);
        editor->setExternalKeySequence(
            selected != nullptr ? selected->keySequence() : QKeySequence{});
        this->setAdditionalShortcutInfo(
            info, this->additionalShortcutCount(target, state.hotkeyName));
    }

    void changeShortcutMode(const QString &target, bool promptForReason,
                            QComboBox *mode, PhysicalKeySequenceEdit *editor,
                            QLabel *info, ShortcutEditorState &state)
    {
        auto *controller = getApp()->getHotkeys();
        const bool previousPromptForReason = state.promptForReason;
        const auto source = controller->getHotkeyByName(state.hotkeyName);
        const bool sourceBelongsToEditor =
            state.target == target &&
            previousPromptForReason != promptForReason && source != nullptr &&
            source->category() == HotkeyCategory::PopupWindow &&
            source->action() ==
                actionForReasonPrompt(previousPromptForReason) &&
            source->arguments() == std::vector<QString>{target};

        if (!sourceBelongsToEditor)
        {
            this->loadShortcut(target, promptForReason, editor, info, state);
            return;
        }

        const auto destinationAction = actionForReasonPrompt(promptForReason);
        const auto destination = controller->getHotkeys(
            HotkeyCategory::PopupWindow, destinationAction,
            std::vector<QString>{target});
        const auto sameSequence = std::find_if(
            destination.begin(), destination.end(),
            [&source](const auto &item) {
                return item->keySequence() == source->keySequence();
            });

        QString preferredName;
        if (sameSequence != destination.end())
        {
            preferredName = (*sameSequence)->name();
            controller->removeHotkey(source->name());
        }
        else
        {
            auto name = source->name();
            if (isGeneratedHotkeyName(name, target, previousPromptForReason))
            {
                name = this->uniqueHotkeyName(target, promptForReason,
                                              source->name());
            }

            auto replacement = std::make_shared<Hotkey>(
                HotkeyCategory::PopupWindow, source->keySequence(),
                destinationAction, std::vector<QString>{target}, name);
            if (controller->isDuplicate(replacement, source->name()))
            {
                const QSignalBlocker blocker(mode);
                mode->setCurrentIndex(mode->findData(previousPromptForReason));
                this->loadShortcut(target, previousPromptForReason, editor,
                                   info, state, source->name());
                this->setShortcutError(
                    info, "That shortcut is already used in another popup.");
                return;
            }

            controller->replaceHotkey(source->name(), replacement);
            preferredName = name;
        }

        controller->save();
        state = {target, promptForReason, preferredName};
        this->loadShortcut(target, promptForReason, editor, info, state,
                           preferredName);
        this->refreshAllTimeoutRows();
        this->refreshPreview();
    }

    void saveShortcut(const QString &target, bool promptForReason,
                      const QKeySequence &requested,
                      PhysicalKeySequenceEdit *editor, QLabel *info,
                      ShortcutEditorState &state)
    {
        auto *controller = getApp()->getHotkeys();
        const auto action = actionForReasonPrompt(promptForReason);
        const auto matches = controller->getHotkeys(
            HotkeyCategory::PopupWindow, action, std::vector<QString>{target});
        std::shared_ptr<Hotkey> existing;
        if (state.target == target &&
            state.promptForReason == promptForReason &&
            !state.hotkeyName.isEmpty())
        {
            const auto found = std::find_if(
                matches.begin(), matches.end(), [&state](const auto &hotkey) {
                    return hotkey->name() == state.hotkeyName;
                });
            if (found != matches.end())
            {
                existing = *found;
            }
        }
        if (existing == nullptr && !matches.empty())
        {
            existing = matches.front();
        }
        const auto normalized = normalizeKeySequence(requested);

        if (normalized.isEmpty())
        {
            if (existing == nullptr)
            {
                state = {target, promptForReason, {}};
                this->setAdditionalShortcutInfo(
                    info, this->additionalShortcutCount(target, {}));
                return;
            }
            controller->removeHotkey(existing->name());
            controller->save();
            state = {target, promptForReason, {}};
            this->refreshHotkeyUi();
            return;
        }

        if (existing != nullptr && existing->keySequence() == normalized)
        {
            if (normalized != requested)
            {
                QSignalBlocker blocker(editor);
                editor->setExternalKeySequence(normalized);
            }
            state = {target, promptForReason, existing->name()};
            this->setAdditionalShortcutInfo(
                info, this->additionalShortcutCount(target, existing->name()));
            return;
        }

        const auto name = existing != nullptr
                              ? existing->name()
                              : this->uniqueHotkeyName(target, promptForReason);
        auto replacement = std::make_shared<Hotkey>(
            HotkeyCategory::PopupWindow, normalized, action,
            std::vector<QString>{target}, name);
        if (controller->isDuplicate(replacement, existing != nullptr
                                                     ? existing->name()
                                                     : QString{}))
        {
            QSignalBlocker blocker(editor);
            editor->setExternalKeySequence(
                existing != nullptr ? existing->keySequence() : QKeySequence{});
            state = {target, promptForReason,
                     existing != nullptr ? existing->name() : QString{}};
            this->setShortcutError(
                info, "That shortcut is already used in another popup.");
            return;
        }

        if (existing != nullptr)
        {
            controller->replaceHotkey(existing->name(), replacement);
        }
        else
        {
            controller->addHotkey(replacement);
        }
        controller->save();
        state = {target, promptForReason, name};

        if (normalized != requested)
        {
            QSignalBlocker blocker(editor);
            editor->setExternalKeySequence(normalized);
        }
        this->refreshHotkeyUi();
    }

    QString uniqueHotkeyName(const QString &target, bool promptForReason,
                             const QString &ignoredName = {}) const
    {
        const auto base = generatedHotkeyName(target, promptForReason);

        QString candidate = base;
        int suffix = 2;
        while (candidate != ignoredName &&
               getApp()->getHotkeys()->getHotkeyByName(candidate) != nullptr)
        {
            candidate = base + ' ' + QString::number(suffix++);
        }
        return candidate;
    }

    size_t additionalShortcutCount(const QString &target,
                                   const QString &selectedName) const
    {
        size_t count = 0;
        bool selectedFound = false;
        for (const bool promptForReason : {false, true})
        {
            const auto matches = getApp()->getHotkeys()->getHotkeys(
                HotkeyCategory::PopupWindow,
                actionForReasonPrompt(promptForReason),
                std::vector<QString>{target});
            count += matches.size();
            selectedFound =
                selectedFound ||
                std::any_of(matches.begin(), matches.end(),
                            [&selectedName](const auto &hotkey) {
                                return !selectedName.isEmpty() &&
                                       hotkey->name() == selectedName;
                            });
        }
        return count - (selectedFound ? 1 : 0);
    }

    void setAdditionalShortcutInfo(QLabel *label, size_t additionalCount)
    {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText,
                         palette.color(QPalette::PlaceholderText));
        label->setPalette(palette);
        if (additionalCount > 0)
        {
            label->setText(
                additionalCount == 1
                    ? "1 more shortcut is set on the Hotkeys page"
                    : QString("%1 more shortcuts are set on the Hotkeys page")
                          .arg(additionalCount));
            label->show();
        }
        else
        {
            label->clear();
            label->hide();
        }
    }

    void setShortcutError(QLabel *label, const QString &text)
    {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText, QColor(224, 105, 105));
        label->setPalette(palette);
        label->setText(text);
        label->show();
    }

    void refreshHotkeyUi()
    {
        this->refreshAllTimeoutRows();
        if (this->timeoutShortcut != nullptr)
        {
            this->loadTimeoutShortcut();
        }
        if (this->banShortcut != nullptr)
        {
            this->loadBanShortcut();
        }
        if (this->unbanShortcut != nullptr)
        {
            this->loadUnbanShortcut();
        }
        this->refreshPreview();
    }

    QString timeoutTooltip(int row, bool includeSelectionHint) const
    {
        if (row < 0 || row >= static_cast<int>(this->entries.size()))
        {
            return {};
        }
        const auto &entry = this->entries[row];
        QStringList lines{
            QString("Timeout for %1").arg(longDuration(entry.button))};
        if (!entry.reason.trimmed().isEmpty())
        {
            lines.push_back("Saved reason: " + entry.reason.trimmed());
        }
        this->appendShortcutTooltip(lines, QString::number(row + 1));
        if (includeSelectionHint)
        {
            lines.push_back("Click to edit this timeout.");
        }
        return lines.join('\n');
    }

    void appendShortcutTooltip(QStringList &lines, const QString &target) const
    {
        const auto immediate = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::PopupWindow, actionForReasonPrompt(false),
            std::vector<QString>{target});
        const auto prompted = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::PopupWindow, actionForReasonPrompt(true),
            std::vector<QString>{target});
        if (!immediate.isEmpty())
        {
            lines.push_back("Shortcut: " + shortcutText(immediate));
        }
        if (!prompted.isEmpty())
        {
            lines.push_back("Review first: " + shortcutText(prompted));
        }
    }

    void refreshPreview()
    {
        if (this->previewTimeoutLayout == nullptr)
        {
            return;
        }
        if (this->previewTimeoutButtons.size() != this->entries.size())
        {
            while (auto *item = this->previewTimeoutLayout->takeAt(0))
            {
                delete item->widget();
                delete item;
            }
            this->previewTimeoutButtons.clear();

            for (int row = 0; row < static_cast<int>(this->entries.size());
                 ++row)
            {
                auto *button = new QPushButton(this->owner);
                button->setCheckable(true);
                button->setAutoExclusive(true);
                button->setFixedSize(40, 30);
                QObject::connect(button, &QPushButton::clicked, this->owner,
                                 [this, row] {
                                     this->timeoutList->setCurrentRow(row);
                                 });
                this->previewTimeoutLayout->addWidget(button);
                this->previewTimeoutButtons.push_back(button);
            }
        }

        for (int row = 0; row < static_cast<int>(this->entries.size()); ++row)
        {
            auto *button = this->previewTimeoutButtons[row];
            button->setText(shortDuration(this->entries[row].button));
            button->setToolTip(this->timeoutTooltip(row, true));
        }
        this->syncPreviewSelection(
            this->timeoutList != nullptr ? this->timeoutList->currentRow() : 0);

        if (this->previewBan != nullptr)
        {
            QStringList banLines{"Ban this user"};
            const auto reason =
                getSettings()->timeoutBanReason.getValue().trimmed();
            if (!reason.isEmpty())
            {
                banLines.push_back("Saved reason: " + reason);
            }
            this->appendShortcutTooltip(banLines, "ban");
            banLines.push_back("Click to edit its shortcut.");
            this->previewBan->setToolTip(banLines.join('\n'));
        }
        if (this->previewUnban != nullptr)
        {
            QStringList unbanLines{"Unban this user"};
            this->appendShortcutTooltip(unbanLines, "unban");
            unbanLines.push_back("Click to edit its shortcut.");
            this->previewUnban->setToolTip(unbanLines.join('\n'));
        }
    }

    void syncPreviewSelection(int row)
    {
        for (int index = 0;
             index < static_cast<int>(this->previewTimeoutButtons.size());
             ++index)
        {
            QSignalBlocker blocker(this->previewTimeoutButtons[index]);
            this->previewTimeoutButtons[index]->setChecked(index == row);
        }
    }

    UsercardModerationSettings *owner;
    std::vector<TimeoutEntry> entries;
    bool updating = false;
    bool entriesRefreshQueued = false;
    pajlada::Signals::SignalHolder signalHolder;

    TimeoutListWidget *timeoutList = nullptr;
    QToolButton *moveUp = nullptr;
    QToolButton *moveDown = nullptr;
    QLabel *selectedTitle = nullptr;
    QSpinBox *durationInput = nullptr;
    QComboBox *unitInput = nullptr;
    QLineEdit *reasonInput = nullptr;
    QComboBox *timeoutShortcutMode = nullptr;
    PhysicalKeySequenceEdit *timeoutShortcut = nullptr;
    QLabel *timeoutShortcutInfo = nullptr;
    ShortcutEditorState timeoutShortcutState;

    QLineEdit *banReason = nullptr;
    QComboBox *banShortcutMode = nullptr;
    PhysicalKeySequenceEdit *banShortcut = nullptr;
    PhysicalKeySequenceEdit *unbanShortcut = nullptr;
    QLabel *banShortcutInfo = nullptr;
    QLabel *unbanShortcutInfo = nullptr;
    ShortcutEditorState banShortcutState;
    ShortcutEditorState unbanShortcutState;

    QHBoxLayout *previewTimeoutLayout = nullptr;
    std::vector<QPushButton *> previewTimeoutButtons;
    QToolButton *previewBan = nullptr;
    QToolButton *previewUnban = nullptr;
};

UsercardModerationSettings::UsercardModerationSettings(QWidget *parent)
    : QWidget(parent)
    , impl_(std::make_unique<Impl>(this))
{
}

UsercardModerationSettings::~UsercardModerationSettings() = default;

}
