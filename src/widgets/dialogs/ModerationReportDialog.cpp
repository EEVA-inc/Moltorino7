#include "widgets/dialogs/ModerationReportDialog.hpp"

#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Fonts.hpp"
#include "util/Clipboard.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>

namespace chatterino {

namespace {

constexpr auto REPORT_FORMAT = "moltorino-moderation-report";
constexpr int REPORT_VERSION = 1;
constexpr int MAX_VISIBLE_ACTIVITY_ROWS = 1000;
constexpr qsizetype MAX_SHARED_REPORT_BYTES = 2 * 1024 * 1024;
constexpr int MAX_REMEMBERED_REPORTS = 4;
constexpr qsizetype MAX_REMEMBERED_EVENTS = 40000;

QHash<QString, std::shared_ptr<const ModerationReportContext>>
    rememberedReports;
QQueue<QString> rememberedReportOrder;
qsizetype rememberedEventCount = 0;

void showReportMessage(QWidget *parent, QMessageBox::Icon icon,
                       const QString &title, const QString &text)
{
    QPointer<QMessageBox> box =
        new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    installMoltorinoDialogTheme(box);
    box->exec();
    delete box.data();
}

QString normalized(QString value)
{
    return value.trimmed().toLower();
}

QString cleanChannelName(QString value)
{
    value = value.trimmed();
    while (value.startsWith(QLatin1Char('#')) ||
           value.startsWith(QLatin1Char('@')))
    {
        value.remove(0, 1);
    }
    return value.trimmed().toLower();
}

QString hasteRawUrl(const QJsonObject &root)
{
    auto value = root.value(QStringLiteral("rawUrl")).toString().trimmed();
    if (value.isEmpty())
    {
        value = root.value(QStringLiteral("url")).toString().trimmed();
    }

    QUrl url(value);
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        !url.userInfo().isEmpty() || url.port(443) != 443 ||
        url.host().compare(QStringLiteral("h.moltorino.com"),
                           Qt::CaseInsensitive) != 0)
    {
        return {};
    }

    auto path = url.path();
    while (path.endsWith(QLatin1Char('/')))
    {
        path.chop(1);
    }
    if (!path.endsWith(QStringLiteral("/raw")))
    {
        path += QStringLiteral("/raw");
    }
    url.setPath(path);
    url.setQuery(QString{});
    url.setFragment(QString{});
    return url.toString(QUrl::FullyEncoded);
}

QString displayName(const ModerationActionLogModeratorSummary &summary)
{
    if (!summary.displayName.trimmed().isEmpty())
    {
        return summary.displayName.trimmed();
    }
    if (!summary.login.trimmed().isEmpty())
    {
        return summary.login.trimmed();
    }
    return QStringLiteral("Unknown");
}

QString identityKey(const QString &id, const QString &login,
                    const QString &display)
{
    if (!id.trimmed().isEmpty())
    {
        return QStringLiteral("id:") + id.trimmed();
    }
    if (!login.trimmed().isEmpty())
    {
        return QStringLiteral("login:") + normalized(login);
    }
    if (!display.trimmed().isEmpty())
    {
        return QStringLiteral("display:") + normalized(display);
    }
    return QStringLiteral("unknown");
}

template <typename Row>
bool sameIdentity(const Row &row, const ModerationActionLogEvent &event)
{
    if (!row.summary.id.isEmpty() && !event.moderatorId.isEmpty())
    {
        return row.summary.id == event.moderatorId;
    }
    const auto rowLogin = normalized(row.summary.login);
    if (!rowLogin.isEmpty() &&
        (rowLogin == normalized(event.moderatorLogin) ||
         rowLogin == normalized(event.moderatorDisplayName)))
    {
        return true;
    }
    const auto rowDisplay = normalized(row.summary.displayName);
    return !rowDisplay.isEmpty() &&
           rowDisplay == normalized(event.moderatorDisplayName);
}

QString actionLabel(GqlModerationActionKind kind)
{
    switch (kind)
    {
        case GqlModerationActionKind::Ban:
            return QStringLiteral("Ban");
        case GqlModerationActionKind::Unban:
            return QStringLiteral("Unban");
        case GqlModerationActionKind::Timeout:
            return QStringLiteral("Timeout");
        case GqlModerationActionKind::Untimeout:
            return QStringLiteral("Untimeout");
        case GqlModerationActionKind::Delete:
            return QStringLiteral("Delete");
        case GqlModerationActionKind::Message:
            return QStringLiteral("Message");
        case GqlModerationActionKind::Other:
            return QStringLiteral("Other");
    }
    return QStringLiteral("Other");
}

QString targetName(const ModerationActionLogEvent &event)
{
    if (!event.targetLogin.trimmed().isEmpty())
    {
        return event.targetLogin.trimmed();
    }
    return event.targetDisplayName.trimmed();
}

bool includesKind(const ModerationReportOutputOptions &options,
                  GqlModerationActionKind kind)
{
    switch (kind)
    {
        case GqlModerationActionKind::Ban:
            return options.bans;
        case GqlModerationActionKind::Timeout:
            return options.timeouts;
        case GqlModerationActionKind::Delete:
            return options.deletes;
        case GqlModerationActionKind::Unban:
            return options.unbans;
        case GqlModerationActionKind::Untimeout:
            return options.untimeouts;
        case GqlModerationActionKind::Other:
            return options.other;
        case GqlModerationActionKind::Message:
            return false;
    }
    return false;
}

bool includesAnyKind(const ModerationReportOutputOptions &options)
{
    return options.bans || options.timeouts || options.deletes ||
           options.unbans || options.untimeouts || options.other;
}

ModerationActionLogCounts selectedCounts(
    const ModerationActionLogCounts &counts,
    const ModerationReportOutputOptions &options)
{
    return {
        .bans = options.bans ? counts.bans : 0,
        .timeouts = options.timeouts ? counts.timeouts : 0,
        .deletes = options.deletes ? counts.deletes : 0,
        .unbans = options.unbans ? counts.unbans : 0,
        .untimeouts = options.untimeouts ? counts.untimeouts : 0,
        .other = options.other ? counts.other : 0,
    };
}

QJsonObject countsJson(const ModerationActionLogCounts &counts)
{
    return {
        {QStringLiteral("total"), counts.rawTotal()},
        {QStringLiteral("bans"), counts.bans},
        {QStringLiteral("timeouts"), counts.timeouts},
        {QStringLiteral("deletes"), counts.deletes},
        {QStringLiteral("unbans"), counts.unbans},
        {QStringLiteral("untimeouts"), counts.untimeouts},
        {QStringLiteral("other"), counts.other},
    };
}

void addCounts(ModerationActionLogCounts &target,
               const ModerationActionLogCounts &source)
{
    target.bans += source.bans;
    target.timeouts += source.timeouts;
    target.deletes += source.deletes;
    target.unbans += source.unbans;
    target.untimeouts += source.untimeouts;
    target.other += source.other;
}

QString csvCell(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString plainTableCell(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('|'), QLatin1Char('/'));
    return value.simplified();
}

QStringList renderPlainTextTable(const QStringList &headers,
                                 const QVector<QStringList> &rows,
                                 const QVector<bool> &rightAligned)
{
    QVector<int> widths(headers.size());
    for (int column = 0; column < headers.size(); ++column)
    {
        widths[column] = headers.at(column).size();
    }
    for (const auto &row : rows)
    {
        for (int column = 0; column < row.size() && column < widths.size();
             ++column)
        {
            widths[column] =
                std::max(widths.at(column), int(row.at(column).size()));
        }
    }

    const auto renderRow = [&widths, &rightAligned](const QStringList &cells) {
        QStringList rendered;
        rendered.reserve(cells.size());
        for (int column = 0; column < cells.size(); ++column)
        {
            const auto cell = plainTableCell(cells.at(column));
            const bool alignRight =
                column < rightAligned.size() && rightAligned.at(column);
            rendered.push_back(alignRight ? cell.rightJustified(widths.at(column))
                                          : cell.leftJustified(widths.at(column)));
        }
        return rendered.join(QStringLiteral(" | "));
    };

    QStringList output;
    const auto header = renderRow(headers);
    const auto divider = QString(header.size(), QLatin1Char('-'));
    output.push_back(divider);
    output.push_back(header);
    output.push_back(divider);
    for (const auto &row : rows)
    {
        output.push_back(renderRow(row));
    }
    output.push_back(divider);
    return output;
}

class NumberItem final : public QTableWidgetItem
{
public:
    explicit NumberItem(int value)
        : QTableWidgetItem(QLocale().toString(value))
    {
        this->setData(Qt::UserRole, value);
        this->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return this->data(Qt::UserRole).toInt() <
               other.data(Qt::UserRole).toInt();
    }
};

class DateItem final : public QTableWidgetItem
{
public:
    DateItem(QString text, qint64 sortValue)
        : QTableWidgetItem(std::move(text))
    {
        this->setData(Qt::UserRole, sortValue);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return this->data(Qt::UserRole).toLongLong() <
               other.data(Qt::UserRole).toLongLong();
    }
};

class ModerationReportRequestDialog final : public QDialog
{
public:
    explicit ModerationReportRequestDialog(QString defaultChannel,
                                           QWidget *parent)
        : QDialog(parent)
    {
        this->setWindowTitle(QStringLiteral("Create moderation report"));
        this->setModal(true);
        this->setMinimumWidth(410);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(
            QStringLiteral("Choose a Twitch channel and range."), this));

        auto *form = new QFormLayout;
        this->channel_ = new QLineEdit(cleanChannelName(defaultChannel), this);
        this->channel_->setPlaceholderText(QStringLiteral("channel name"));
        form->addRow(QStringLiteral("Channel:"), this->channel_);

        auto *rangeRow = new QWidget(this);
        auto *rangeLayout = new QHBoxLayout(rangeRow);
        rangeLayout->setContentsMargins(0, 0, 0, 0);
        rangeLayout->setSpacing(8);
        this->range_ = new QComboBox(rangeRow);
        this->range_->addItem(QStringLiteral("Last 24 hours"), 1);
        this->range_->addItem(QStringLiteral("Last 7 days"), 7);
        this->range_->addItem(QStringLiteral("Last 30 days"), 30);
        this->range_->addItem(QStringLiteral("Last 90 days"), 90);
        this->range_->addItem(QStringLiteral("Last year"), 365);
        this->range_->addItem(QStringLiteral("Custom"), -1);
        this->range_->setCurrentIndex(1);
        this->customDays_ = new QSpinBox(rangeRow);
        this->customDays_->setRange(1, 3650);
        this->customDays_->setValue(7);
        this->customDays_->setSuffix(QStringLiteral(" days"));
        this->customDays_->setVisible(false);
        rangeLayout->addWidget(this->range_, 1);
        rangeLayout->addWidget(this->customDays_);
        form->addRow(QStringLiteral("Range:"), rangeRow);
        layout->addLayout(form);

        this->error_ = new QLabel(this);
        this->error_->setWordWrap(true);
        this->error_->setVisible(false);
        this->error_->setStyleSheet(
            QStringLiteral("color: palette(bright-text);"));
        layout->addWidget(this->error_);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *create = buttons->addButton(QStringLiteral("Create report"),
                                          QDialogButtonBox::AcceptRole);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(create, &QPushButton::clicked, this, [this] {
            const auto channel = cleanChannelName(this->channel_->text());
            if (!QRegularExpression(QStringLiteral("^[a-z0-9_]{3,25}$"))
                     .match(channel)
                     .hasMatch())
            {
                this->error_->setText(
                    QStringLiteral("Enter a valid Twitch channel name."));
                this->error_->setVisible(true);
                this->channel_->setFocus(Qt::OtherFocusReason);
                return;
            }
            this->error_->setVisible(false);
            this->accept();
        });
        QObject::connect(this->range_,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this] {
                             this->customDays_->setVisible(
                                 this->range_->currentData().toInt() < 0);
                         });
        layout->addWidget(buttons);
        installMoltorinoDialogTheme(this);
    }

    ModerationReportRequest request() const
    {
        const int preset = this->range_->currentData().toInt();
        const int days = preset < 0 ? this->customDays_->value() : preset;
        return {
            .channelLogin = cleanChannelName(this->channel_->text()),
            .days = days,
            .rangeText = days == 1 ? QStringLiteral("last 24 hours")
                                   : QStringLiteral("last %1 days").arg(days),
        };
    }

private:
    QLineEdit *channel_{};
    QComboBox *range_{};
    QSpinBox *customDays_{};
    QLabel *error_{};
};

class ReportOptionsWidget final : public QWidget
{
public:
    ReportOptionsWidget(const ModerationReportOutputOptions &defaults,
                        bool currentRosterAvailable, QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *actions = new QGroupBox(QStringLiteral("Actions"), this);
        auto *actionLayout = new QGridLayout(actions);
        this->bans_ = new QCheckBox(QStringLiteral("Bans"), actions);
        this->timeouts_ = new QCheckBox(QStringLiteral("Timeouts"), actions);
        this->deletes_ = new QCheckBox(QStringLiteral("Deletes"), actions);
        this->unbans_ = new QCheckBox(QStringLiteral("Unbans"), actions);
        this->untimeouts_ =
            new QCheckBox(QStringLiteral("Untimeouts"), actions);
        this->other_ = new QCheckBox(QStringLiteral("Other"), actions);
        this->other_->setToolTip(QStringLiteral(
            "Mostly raids, Shared Chat events, and role changes rather than "
            "direct moderation actions."));
        const std::array<QCheckBox *, 6> actionBoxes{
            this->bans_,   this->timeouts_,   this->deletes_,
            this->unbans_, this->untimeouts_, this->other_,
        };
        for (int i = 0; i < int(actionBoxes.size()); ++i)
        {
            actionLayout->addWidget(actionBoxes.at(i), i / 3, i % 3);
        }
        this->bans_->setChecked(defaults.bans);
        this->timeouts_->setChecked(defaults.timeouts);
        this->deletes_->setChecked(defaults.deletes);
        this->unbans_->setChecked(defaults.unbans);
        this->untimeouts_->setChecked(defaults.untimeouts);
        this->other_->setChecked(defaults.other);
        for (auto *box : actionBoxes)
        {
            QObject::connect(box, &QCheckBox::toggled, this, [this, box](bool) {
                if (!includesAnyKind(this->options()))
                {
                    const QSignalBlocker blocker(box);
                    box->setChecked(true);
                }
            });
        }
        layout->addWidget(actions);

        auto *moderators = new QGroupBox(QStringLiteral("Moderators"), this);
        auto *moderatorLayout = new QFormLayout(moderators);
        this->scope_ = new QComboBox(moderators);
        this->scope_->addItem(QStringLiteral("Channel moderators"), true);
        this->scope_->addItem(QStringLiteral("Everyone in history"), false);
        this->scope_->setCurrentIndex(
            defaults.includeOutsideTeam || !currentRosterAvailable ? 1 : 0);
        this->scope_->setToolTip(QStringLiteral(
            "Everyone in history also shows former moderators and moderators "
            "from Shared Chat."));
        this->scope_->setEnabled(currentRosterAvailable);
        if (!currentRosterAvailable)
        {
            this->scope_->setToolTip(
                QStringLiteral("The channel moderator list could not be "
                               "loaded, so everyone in Twitch's logs is "
                               "included."));
        }
        moderatorLayout->addRow(QStringLiteral("Show:"), this->scope_);

        this->addedSince_ = new QComboBox(moderators);
        this->addedSince_->addItem(QStringLiteral("Any time"), 0);
        this->addedSince_->addItem(QStringLiteral("Last 7 days"), 7);
        this->addedSince_->addItem(QStringLiteral("Last 30 days"), 30);
        this->addedSince_->addItem(QStringLiteral("Last 90 days"), 90);
        this->addedSince_->setCurrentIndex(std::max(
            0, this->addedSince_->findData(defaults.addedWithinDays)));
        moderatorLayout->addRow(QStringLiteral("Added:"), this->addedSince_);

        this->sort_ = new QComboBox(moderators);
        const auto updateSortChoices = [this, currentRosterAvailable] {
            const auto current = this->sort_->currentData();
            this->sort_->clear();
            this->sort_->addItem(QStringLiteral("Most actions first"),
                                int(ModerationReportSort::Total));
            if (this->bans_->isChecked())
            {
                this->sort_->addItem(QStringLiteral("Most bans first"),
                                    int(ModerationReportSort::Bans));
            }
            if (this->timeouts_->isChecked())
            {
                this->sort_->addItem(QStringLiteral("Most timeouts first"),
                                    int(ModerationReportSort::Timeouts));
            }
            if (this->deletes_->isChecked())
            {
                this->sort_->addItem(QStringLiteral("Most deletes first"),
                                    int(ModerationReportSort::Deletes));
            }
            if (this->unbans_->isChecked())
            {
                this->sort_->addItem(QStringLiteral("Most unbans first"),
                                    int(ModerationReportSort::Unbans));
            }
            if (this->untimeouts_->isChecked())
            {
                this->sort_->addItem(QStringLiteral("Most untimeouts first"),
                                    int(ModerationReportSort::Untimeouts));
            }
            if (this->other_->isChecked())
            {
                this->sort_->addItem(
                    QStringLiteral("Most other actions first"),
                    int(ModerationReportSort::Other));
            }
            if (currentRosterAvailable && this->scope_->currentData().toBool())
            {
                this->sort_->addItem(
                    QStringLiteral("Newest moderators first"),
                    int(ModerationReportSort::AddedNewest));
                this->sort_->addItem(
                    QStringLiteral("Oldest moderators first"),
                    int(ModerationReportSort::AddedOldest));
            }
            this->sort_->addItem(QStringLiteral("Name, A to Z"),
                                 int(ModerationReportSort::Name));
            const int index = this->sort_->findData(current);
            this->sort_->setCurrentIndex(std::max(0, index));
        };
        for (auto *box : actionBoxes)
        {
            QObject::connect(box, &QCheckBox::toggled, this,
                             updateSortChoices);
        }
        QObject::connect(this->scope_,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         updateSortChoices);
        updateSortChoices();
        const int defaultSort = this->sort_->findData(int(defaults.sort));
        this->sort_->setCurrentIndex(std::max(0, defaultSort));
        moderatorLayout->addRow(QStringLiteral("Sort:"), this->sort_);

        this->includeInactive_ = new QCheckBox(
            QStringLiteral("Include moderators with no matching actions"),
            moderators);
        this->includeInactive_->setChecked(defaults.includeInactive);
        moderatorLayout->addRow(QString(), this->includeInactive_);

        const auto updateModeratorState = [this, currentRosterAvailable] {
            this->addedSince_->setEnabled(
                currentRosterAvailable && this->scope_->currentData().toBool());
        };
        QObject::connect(this->scope_,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         updateModeratorState);
        updateModeratorState();
        layout->addWidget(moderators);

        this->activity_ = new QGroupBox(QStringLiteral("Activity"), this);
        auto *activityLayout = new QVBoxLayout(this->activity_);

        this->events_ = new QCheckBox(
            QStringLiteral("Include activity history"), this->activity_);
        this->events_->setToolTip(
            QStringLiteral("Shows each action and when it happened."));
        this->events_->setChecked(defaults.includeEvents);
        activityLayout->addWidget(this->events_);

        this->targets_ =
            new QCheckBox(QStringLiteral("Show affected users"),
                          this->activity_);
        this->targets_->setToolTip(
            QStringLiteral("Shows who each action was taken against."));
        this->targets_->setChecked(defaults.includeTargets);
        activityLayout->addWidget(this->targets_);

        this->actionText_ = new QCheckBox(
            QStringLiteral("Show Twitch details"), this->activity_);
        this->actionText_->setToolTip(QStringLiteral(
            "May include moderation reasons and deleted message text."));
        this->actionText_->setChecked(defaults.includeActionText);
        activityLayout->addWidget(this->actionText_);

        const auto updateDetailState = [this] {
            const bool enabled = this->events_->isChecked();
            this->targets_->setEnabled(enabled);
            this->actionText_->setEnabled(enabled);
            if (!enabled)
            {
                this->targets_->setChecked(false);
                this->actionText_->setChecked(false);
            }
        };
        QObject::connect(this->events_, &QCheckBox::toggled, this,
                         updateDetailState);
        updateDetailState();
        layout->addWidget(this->activity_);
    }

    ModerationReportOutputOptions options() const
    {
        return {
            .bans = this->bans_->isChecked(),
            .timeouts = this->timeouts_->isChecked(),
            .deletes = this->deletes_->isChecked(),
            .unbans = this->unbans_->isChecked(),
            .untimeouts = this->untimeouts_->isChecked(),
            .other = this->other_->isChecked(),
            .includeOutsideTeam = !this->scope_->currentData().toBool(),
            .includeInactive = this->includeInactive_->isChecked(),
            .addedWithinDays = this->addedSince_->currentData().toInt(),
            .sort = static_cast<ModerationReportSort>(
                this->sort_->currentData().toInt()),
            .includeEvents = this->events_->isChecked(),
            .includeTargets =
                this->events_->isChecked() && this->targets_->isChecked(),
            .includeActionText =
                this->events_->isChecked() && this->actionText_->isChecked(),
        };
    }

    void setActivityAvailable(bool available)
    {
        this->activity_->setEnabled(available);
        this->activity_->setToolTip(
            available ? QString{}
                      : QStringLiteral(
                            "Choose JSON to include activity history."));
        if (!available)
        {
            this->events_->setChecked(false);
        }
    }

private:
    QCheckBox *bans_{};
    QCheckBox *timeouts_{};
    QCheckBox *deletes_{};
    QCheckBox *unbans_{};
    QCheckBox *untimeouts_{};
    QCheckBox *other_{};
    QComboBox *scope_{};
    QComboBox *addedSince_{};
    QComboBox *sort_{};
    QCheckBox *includeInactive_{};
    QGroupBox *activity_{};
    QCheckBox *events_{};
    QCheckBox *targets_{};
    QCheckBox *actionText_{};
};

class ExportReportDialog final : public QDialog
{
public:
    ExportReportDialog(const ModerationReportOutputOptions &defaults,
                       bool currentRosterAvailable, QWidget *parent)
        : QDialog(parent)
    {
        this->setWindowTitle(QStringLiteral("Export moderation data"));
        this->setModal(true);
        this->setMinimumWidth(500);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(
            new QLabel(QStringLiteral("Choose what to save."), this));
        this->options_ =
            new ReportOptionsWidget(defaults, currentRosterAvailable, this);
        layout->addWidget(this->options_);

        auto *file = new QGroupBox(QStringLiteral("File"), this);
        auto *fileLayout = new QFormLayout(file);
        this->format_ = new QComboBox(file);
        this->format_->addItem(QStringLiteral("Moderator totals (.csv)"),
                               int(ModerationReportFileFormat::Csv));
        this->format_->addItem(QStringLiteral("Full report data (.json)"),
                               int(ModerationReportFileFormat::Json));
        this->format_->setCurrentIndex(
            std::max(0, this->format_->findData(int(defaults.fileFormat))));
        this->format_->setToolTip(
            QStringLiteral("CSV saves the moderator table. JSON can also "
                           "include the activity history."));
        fileLayout->addRow(QStringLiteral("Format:"), this->format_);
        layout->addWidget(file);

        const auto updateFormatState = [this] {
            const auto format = static_cast<ModerationReportFileFormat>(
                this->format_->currentData().toInt());
            this->options_->setActivityAvailable(
                format == ModerationReportFileFormat::Json);
        };
        QObject::connect(this->format_,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         updateFormatState);
        updateFormatState();

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *save = buttons->addButton(QStringLiteral("Save file"),
                                        QDialogButtonBox::AcceptRole);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(save, &QPushButton::clicked, this, &QDialog::accept);
        layout->addWidget(buttons);
        installMoltorinoDialogTheme(this);
    }

    ModerationReportOutputOptions options() const
    {
        auto options = this->options_->options();
        options.fileFormat = static_cast<ModerationReportFileFormat>(
            this->format_->currentData().toInt());
        return options;
    }

private:
    ReportOptionsWidget *options_{};
    QComboBox *format_{};
};

class ShareReportDialog final : public QDialog
{
public:
    ShareReportDialog(const ModerationReportOutputOptions &defaults,
                      bool currentRosterAvailable, QWidget *parent)
        : QDialog(parent)
    {
        this->setWindowTitle(QStringLiteral("Share moderation report"));
        this->setModal(true);
        this->setMinimumWidth(500);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(
            QStringLiteral("Choose what appears in the Haste report."),
            this));
        this->options_ =
            new ReportOptionsWidget(defaults, currentRosterAvailable, this);
        layout->addWidget(this->options_);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *share = buttons->addButton(QStringLiteral("Create link"),
                                         QDialogButtonBox::AcceptRole);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(share, &QPushButton::clicked, this, &QDialog::accept);
        layout->addWidget(buttons);
        installMoltorinoDialogTheme(this);
    }

    ModerationReportOutputOptions options() const
    {
        return this->options_->options();
    }

private:
    ReportOptionsWidget *options_{};
};

}

std::optional<ModerationReportRequest> requestModerationReport(
    QWidget *parent, const QString &defaultChannel)
{
    QPointer<ModerationReportRequestDialog> dialog =
        new ModerationReportRequestDialog(defaultChannel, parent);
    const auto result = dialog->exec();
    if (!dialog)
    {
        return std::nullopt;
    }
    const auto request = result == QDialog::Accepted
                             ? std::make_optional(dialog->request())
                             : std::nullopt;
    delete dialog;
    return request;
}

QString rememberModerationReport(ModerationReportContext context)
{
    const auto reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto report =
        std::make_shared<const ModerationReportContext>(std::move(context));
    rememberedEventCount += report->snapshot.events.size();
    rememberedReports.insert(reportId, std::move(report));
    rememberedReportOrder.enqueue(reportId);

    while (rememberedReportOrder.size() > 1 &&
           (rememberedReportOrder.size() > MAX_REMEMBERED_REPORTS ||
            rememberedEventCount > MAX_REMEMBERED_EVENTS))
    {
        const auto oldest = rememberedReportOrder.dequeue();
        const auto removed = rememberedReports.take(oldest);
        if (removed)
        {
            rememberedEventCount -= removed->snapshot.events.size();
        }
    }
    return reportId;
}

bool openRememberedModerationReport(const QString &reportId, QWidget *parent)
{
    const auto report = rememberedReports.value(reportId);
    if (!report)
    {
        showReportMessage(
            parent, QMessageBox::Information,
            QStringLiteral("Report no longer available"),
            QStringLiteral("Run /modlogs again to create a new report."));
        return false;
    }

    auto *dialog = new ModerationReportDialog(*report, parent);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return true;
}

ModerationReportDialog::ModerationReportDialog(ModerationReportContext context,
                                               QWidget *parent)
    : QDialog(parent)
    , context_(std::move(context))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(QStringLiteral("Moderation report - #%1")
                             .arg(this->context_.channelLogin));
    this->resize(860, 660);
    this->setMinimumSize(820, 600);

    if (!this->context_.generatedAtUtc.isValid())
    {
        this->context_.generatedAtUtc = QDateTime::currentDateTimeUtc();
    }
    this->buildRows();

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    auto *heading = new QLabel(QStringLiteral("Moderation report"), this);
    QFont headingFont = heading->font();
    headingFont = makeResolvedFont(headingFont, QFont::Bold);
    headingFont.setPointSizeF(headingFont.pointSizeF() + 1);
    heading->setFont(headingFont);
    titleRow->addWidget(heading);

    auto *contextLabel = new QLabel(
        QStringLiteral("#%1  ·  %2  ·  %3")
            .arg(this->context_.channelLogin, this->context_.rangeText,
                 QLocale().toString(this->context_.generatedAtUtc.toLocalTime(),
                                    QLocale::ShortFormat)),
        this);
    titleRow->addWidget(contextLabel);
    titleRow->addStretch(1);
    this->operationStatusLabel_ = new QLabel(this);
    this->operationStatusLabel_->setVisible(false);
    titleRow->addWidget(this->operationStatusLabel_);
    this->totalLabel_ = new QLabel(this);
    QFont totalFont = this->totalLabel_->font();
    totalFont = makeResolvedFont(totalFont, QFont::Bold);
    this->totalLabel_->setFont(totalFont);
    titleRow->addWidget(this->totalLabel_);
    layout->addLayout(titleRow);

    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setWordWrap(true);
    QStringList status;
    if (this->context_.snapshot.truncated)
    {
        status.push_back(QStringLiteral(
            "This report is incomplete because Twitch's history limit was "
            "reached."));
    }
    if (this->context_.snapshot.eventsTruncated)
    {
        status.push_back(QStringLiteral(
            "Some older activity is hidden, but it remains included in the "
            "totals."));
    }
    if (!this->context_.currentRosterAvailable)
    {
        status.push_back(QStringLiteral(
            "The channel moderator list could not be loaded. Everyone found "
            "in Twitch's logs is shown."));
    }
    this->statusLabel_->setText(status.join(QLatin1Char(' ')));
    this->statusLabel_->setVisible(!status.isEmpty());
    layout->addWidget(this->statusLabel_);

    auto *splitter = new QSplitter(Qt::Vertical, this);

    auto *moderatorGroup =
        new QGroupBox(QStringLiteral("Moderators"), splitter);
    auto *moderatorLayout = new QVBoxLayout(moderatorGroup);
    moderatorLayout->setSpacing(6);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    this->search_ = new QLineEdit(moderatorGroup);
    this->search_->setPlaceholderText(QStringLiteral("Find a moderator"));
    this->search_->setClearButtonEnabled(true);
    filters->addWidget(this->search_, 1);
    filters->addWidget(new QLabel(QStringLiteral("Show:"), moderatorGroup));
    this->scope_ = new QComboBox(moderatorGroup);
    this->scope_->addItem(QStringLiteral("Channel moderators"), true);
    this->scope_->addItem(QStringLiteral("Everyone in history"), false);
    this->scope_->setToolTip(QStringLiteral(
        "Channel moderators shows the current moderator list. Everyone in "
        "history also includes former moderators and Shared Chat moderators."));
    filters->addWidget(this->scope_);
    filters->addWidget(new QLabel(QStringLiteral("Added:"), moderatorGroup));
    this->addedSince_ = new QComboBox(moderatorGroup);
    this->addedSince_->addItem(QStringLiteral("Any time"), 0);
    this->addedSince_->addItem(QStringLiteral("Last 7 days"), 7);
    this->addedSince_->addItem(QStringLiteral("Last 30 days"), 30);
    this->addedSince_->addItem(QStringLiteral("Last 90 days"), 90);
    filters->addWidget(this->addedSince_);
    this->showZero_ =
        new QCheckBox(QStringLiteral("Include inactive"), moderatorGroup);
    this->showZero_->setToolTip(
        QStringLiteral("Show channel moderators with no actions in this "
                       "report."));
    this->showZero_->setChecked(true);
    filters->addWidget(this->showZero_);
    moderatorLayout->addLayout(filters);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(14);
    actions->addWidget(new QLabel(QStringLiteral("Actions:"), moderatorGroup));
    const auto &totals = this->context_.snapshot.totals;
    this->bans_ = new QCheckBox(
        QStringLiteral("Bans %1").arg(QLocale().toString(totals.bans)),
        moderatorGroup);
    this->timeouts_ = new QCheckBox(
        QStringLiteral("Timeouts %1").arg(QLocale().toString(totals.timeouts)),
        moderatorGroup);
    this->deletes_ = new QCheckBox(
        QStringLiteral("Deletes %1").arg(QLocale().toString(totals.deletes)),
        moderatorGroup);
    this->unbans_ = new QCheckBox(
        QStringLiteral("Unbans %1").arg(QLocale().toString(totals.unbans)),
        moderatorGroup);
    this->untimeouts_ =
        new QCheckBox(QStringLiteral("Untimeouts %1")
                          .arg(QLocale().toString(totals.untimeouts)),
                      moderatorGroup);
    this->other_ = new QCheckBox(
        QStringLiteral("Other %1").arg(QLocale().toString(totals.other)),
        moderatorGroup);
    this->other_->setToolTip(QStringLiteral(
        "Mostly raids, Shared Chat events, and role changes rather than "
        "direct moderation actions."));
    const std::array<QCheckBox *, 6> actionBoxes{
        this->bans_,   this->timeouts_,   this->deletes_,
        this->unbans_, this->untimeouts_, this->other_,
    };
    for (auto *box : actionBoxes)
    {
        box->setChecked(true);
    }
    this->other_->setChecked(false);
    for (int index = 0; index < int(actionBoxes.size()); ++index)
    {
        actions->addWidget(actionBoxes.at(index));
    }
    actions->addStretch(1);
    moderatorLayout->addLayout(actions);

    this->moderators_ = new QTableWidget(0, 9, moderatorGroup);
    this->moderators_->setHorizontalHeaderLabels(
        {QStringLiteral("Moderator"), QStringLiteral("Total"),
         QStringLiteral("Bans"), QStringLiteral("Timeouts"),
         QStringLiteral("Deletes"), QStringLiteral("Unbans"),
         QStringLiteral("Untimeouts"), QStringLiteral("Other"),
         QStringLiteral("Added")});
    this->moderators_->horizontalHeaderItem(7)->setToolTip(QStringLiteral(
        "Mostly raids, Shared Chat events, and role changes rather than "
        "direct moderation actions."));
    this->moderators_->setAlternatingRowColors(true);
    this->moderators_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->moderators_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->moderators_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->moderators_->verticalHeader()->hide();
    this->moderators_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    this->moderators_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    this->moderators_->horizontalHeader()->setMinimumSectionSize(48);
    this->moderators_->horizontalHeader()->setSortIndicator(
        1, Qt::DescendingOrder);
    this->moderators_->setSortingEnabled(true);
    this->moderators_->setMinimumHeight(
        this->moderators_->horizontalHeader()->sizeHint().height() +
        this->moderators_->verticalHeader()->defaultSectionSize() * 6 + 6);
    moderatorLayout->addWidget(this->moderators_);
    splitter->addWidget(moderatorGroup);

    auto *activityGroup = new QGroupBox(QStringLiteral("Activity"), splitter);
    auto *activityLayout = new QVBoxLayout(activityGroup);
    activityLayout->setSpacing(6);
    this->activityEmpty_ =
        new QLabel(QStringLiteral("Select a moderator to see their actions."),
                   activityGroup);
    activityLayout->addWidget(this->activityEmpty_);

    this->activity_ = new QTableWidget(0, 4, activityGroup);
    this->activity_->setHorizontalHeaderLabels(
        {QStringLiteral("Time"), QStringLiteral("Action"),
         QStringLiteral("Target"), QStringLiteral("Details")});
    this->activity_->setAlternatingRowColors(true);
    this->activity_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->activity_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->activity_->verticalHeader()->hide();
    this->activity_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    this->activity_->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);
    this->activity_->setMinimumHeight(
        this->activity_->horizontalHeader()->sizeHint().height() +
        this->activity_->verticalHeader()->defaultSectionSize() * 4 + 6);
    activityLayout->addWidget(this->activity_);
    splitter->addWidget(activityGroup);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    splitter->setSizes({330, 240});
    layout->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    this->exportButton_ = buttons->addButton(
        QStringLiteral("Export"), QDialogButtonBox::ActionRole);
    this->shareButton_ = buttons->addButton(
        QStringLiteral("Share"), QDialogButtonBox::ActionRole);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::close);
    QObject::connect(this->exportButton_, &QPushButton::clicked, this,
                     &ModerationReportDialog::openExportDialog);
    QObject::connect(this->shareButton_, &QPushButton::clicked, this,
                     &ModerationReportDialog::openShareDialog);
    layout->addWidget(buttons);

    const auto refresh = [this] {
        this->updateControlState();
        this->refreshRows();
    };
    QObject::connect(this->search_, &QLineEdit::textChanged, this, refresh);
    QObject::connect(this->scope_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     refresh);
    QObject::connect(this->showZero_, &QCheckBox::toggled, this, refresh);
    QObject::connect(this->addedSince_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     refresh);
    for (auto *box : actionBoxes)
    {
        QObject::connect(box, &QCheckBox::toggled, this, [this, box](bool) {
            if (!includesAnyKind(this->currentOutputOptions()))
            {
                const QSignalBlocker blocker(box);
                box->setChecked(true);
            }
            this->refreshRows();
        });
    }
    QObject::connect(this->moderators_, &QTableWidget::itemSelectionChanged,
                     this, &ModerationReportDialog::refreshActivity);

    for (auto *label : this->findChildren<QLabel *>())
    {
        label->setTextFormat(Qt::PlainText);
    }
    this->updateControlState();
    this->refreshRows();
    installMoltorinoDialogTheme(this, [this] {
        this->refreshThemeColors();
    });
}

void ModerationReportDialog::buildRows()
{
    QHash<QString, int> rowByKey;
    const auto indexRow = [&rowByKey](const Row &row, int index) {
        const QStringList keys{
            identityKey(row.summary.id, {}, {}),
            identityKey({}, row.summary.login, {}),
            identityKey({}, {}, row.summary.displayName),
        };
        for (const auto &key : keys)
        {
            if (key != QStringLiteral("unknown"))
            {
                rowByKey.insert(key, index);
            }
        }
    };
    for (const auto &summary : this->context_.snapshot.moderators)
    {
        const auto index = this->rows_.size();
        this->rows_.push_back({.summary = summary});
        indexRow(this->rows_.back(), index);
    }

    const auto markCurrent = [&](const QString &id, const QString &login,
                                 const QString &display,
                                 const QDateTime &grantedAt, bool broadcaster) {
        int index = -1;
        const QStringList keys{
            identityKey(id, {}, {}),
            identityKey({}, login, {}),
            identityKey({}, {}, display),
        };
        for (const auto &key : keys)
        {
            const auto it = rowByKey.constFind(key);
            if (it != rowByKey.cend())
            {
                const auto &candidateID = this->rows_.at(*it).summary.id;
                if (!id.isEmpty() && !candidateID.isEmpty() && candidateID != id)
                {
                    continue;
                }
                index = *it;
                break;
            }
        }
        if (index < 0)
        {
            ModerationActionLogModeratorSummary summary;
            summary.id = id;
            summary.login = login;
            summary.displayName = display;
            index = this->rows_.size();
            this->rows_.push_back({.summary = std::move(summary)});
            indexRow(this->rows_.back(), index);
        }

        auto &row = this->rows_[index];
        row.currentModerator = true;
        row.broadcaster = broadcaster;
        row.grantedAt = grantedAt;
        if (row.summary.id.isEmpty())
        {
            row.summary.id = id;
        }
        if (row.summary.login.isEmpty())
        {
            row.summary.login = login;
        }
        if (row.summary.displayName.isEmpty())
        {
            row.summary.displayName = display;
        }
    };

    if (this->context_.currentRosterAvailable)
    {
        markCurrent(this->context_.channelId, this->context_.channelLogin,
                    this->context_.channelLogin, {}, true);
        for (const auto &moderator : this->context_.currentModerators)
        {
            markCurrent(moderator.userId, moderator.userLogin,
                        moderator.userName, moderator.grantedAt, false);
        }
    }
}

bool ModerationReportDialog::actionEnabled(GqlModerationActionKind kind) const
{
    return includesKind(this->currentOutputOptions(), kind);
}

ModerationReportOutputOptions ModerationReportDialog::currentOutputOptions()
    const
{
    return {
        .bans = this->bans_->isChecked(),
        .timeouts = this->timeouts_->isChecked(),
        .deletes = this->deletes_->isChecked(),
        .unbans = this->unbans_->isChecked(),
        .untimeouts = this->untimeouts_->isChecked(),
        .other = this->other_->isChecked(),
        .includeOutsideTeam = !this->context_.currentRosterAvailable ||
                              !this->scope_->currentData().toBool(),
        .includeInactive = this->showZero_->isChecked(),
        .addedWithinDays = this->addedSince_->currentData().toInt(),
        .sort = ModerationReportSort::Total,
    };
}

void ModerationReportDialog::refreshRows()
{
    const auto selectedRows =
        this->moderators_->selectionModel()->selectedRows();
    QString selectedKey;
    if (!selectedRows.isEmpty())
    {
        const auto *item =
            this->moderators_->item(selectedRows.front().row(), 0);
        if (item)
        {
            const auto index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < this->rows_.size())
            {
                const auto &row = this->rows_.at(index);
                selectedKey = identityKey(row.summary.id, row.summary.login,
                                          row.summary.displayName);
            }
        }
    }

    const QSignalBlocker tableSignals(this->moderators_);

    const int sortColumn =
        this->moderators_->horizontalHeader()->sortIndicatorSection();
    const auto sortOrder =
        this->moderators_->horizontalHeader()->sortIndicatorOrder();
    const auto options = this->currentOutputOptions();
    this->moderators_->setSortingEnabled(false);
    this->moderators_->setRowCount(0);
    ModerationActionLogCounts visibleTotals;
    for (int i = 0; i < this->rows_.size(); ++i)
    {
        const auto &row = this->rows_.at(i);
        if (!this->rowMatchesFilters(row))
        {
            continue;
        }

        const auto counts = selectedCounts(row.summary.counts, options);
        addCounts(visibleTotals, counts);
        const int tableRow = this->moderators_->rowCount();
        this->moderators_->insertRow(tableRow);
        auto name = displayName(row.summary);
        if (row.broadcaster)
        {
            name += QStringLiteral(" (broadcaster)");
        }
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, i);
        this->moderators_->setItem(tableRow, 0, nameItem);

        const auto addNumber = [&](int column, int value) {
            auto *item = new NumberItem(value);
            this->moderators_->setItem(tableRow, column, item);
        };
        addNumber(1, counts.rawTotal());
        addNumber(2, counts.bans);
        addNumber(3, counts.timeouts);
        addNumber(4, counts.deletes);
        addNumber(5, counts.unbans);
        addNumber(6, counts.untimeouts);
        addNumber(7, counts.other);

        const auto granted =
            row.broadcaster ? QStringLiteral("Broadcaster")
            : row.grantedAt.isValid()
                ? QLocale().toString(row.grantedAt.toLocalTime(),
                                     QLocale::ShortFormat)
                : QStringLiteral("Unknown");
        const qint64 grantedSortValue =
            row.broadcaster           ? std::numeric_limits<qint64>::max()
            : row.grantedAt.isValid() ? row.grantedAt.toMSecsSinceEpoch()
                                      : std::numeric_limits<qint64>::min();
        this->moderators_->setItem(tableRow, 8,
                                   new DateItem(granted, grantedSortValue));
    }
    this->moderators_->setSortingEnabled(true);
    this->moderators_->sortItems(sortColumn, sortOrder);

    if (!selectedKey.isEmpty())
    {
        for (int row = 0; row < this->moderators_->rowCount(); ++row)
        {
            const auto *item = this->moderators_->item(row, 0);
            const auto index = item ? item->data(Qt::UserRole).toInt() : -1;
            if (index < 0 || index >= this->rows_.size())
            {
                continue;
            }
            const auto &candidate = this->rows_.at(index);
            if (identityKey(candidate.summary.id, candidate.summary.login,
                            candidate.summary.displayName) == selectedKey)
            {
                this->moderators_->selectRow(row);
                break;
            }
        }
    }
    if (this->moderators_->rowCount() > 0 &&
        this->moderators_->selectedItems().isEmpty())
    {
        this->moderators_->selectRow(0);
    }

    this->totalLabel_->setText(
        QStringLiteral("%1 actions  ·  %2 moderators")
            .arg(QLocale().toString(visibleTotals.rawTotal()))
            .arg(QLocale().toString(this->moderators_->rowCount())));
    this->refreshThemeColors();
    this->refreshActivity();
}

void ModerationReportDialog::refreshThemeColors()
{
    const auto palette = this->moderators_->palette();
    const auto disabled =
        palette.color(QPalette::Disabled, QPalette::Text);
    const auto options = this->currentOutputOptions();
    const std::array<bool, 7> enabledColumns{
        true,           options.bans,       options.timeouts, options.deletes,
        options.unbans, options.untimeouts, options.other,
    };

    for (int column = 1; column <= 7; ++column)
    {
        const bool enabled = enabledColumns.at(column - 1);
        if (auto *header = this->moderators_->horizontalHeaderItem(column))
        {
            header->setForeground(
                enabled ? palette.color(QPalette::Active, QPalette::Text)
                        : disabled);
        }
        for (int row = 0; row < this->moderators_->rowCount(); ++row)
        {
            if (auto *item = this->moderators_->item(row, column))
            {
                item->setForeground(enabled ? QBrush{} : QBrush(disabled));
            }
        }
    }
}

void ModerationReportDialog::refreshActivity()
{
    this->activity_->setRowCount(0);
    const auto selected = this->moderators_->selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        this->activityEmpty_->setText(
            QStringLiteral("Select a moderator to see their actions."));
        return;
    }
    const auto *item = this->moderators_->item(selected.front().row(), 0);
    const int rowIndex = item ? item->data(Qt::UserRole).toInt() : -1;
    if (rowIndex < 0 || rowIndex >= this->rows_.size())
    {
        return;
    }

    const auto &row = this->rows_.at(rowIndex);
    this->activityEmpty_->setText(displayName(row.summary));
    int matchingActions = 0;
    for (const auto &event : this->context_.snapshot.events)
    {
        if (!sameIdentity(row, event) || !this->actionEnabled(event.kind))
        {
            continue;
        }

        ++matchingActions;
        if (this->activity_->rowCount() >= MAX_VISIBLE_ACTIVITY_ROWS)
        {
            continue;
        }

        const int tableRow = this->activity_->rowCount();
        this->activity_->insertRow(tableRow);
        const auto timestamp =
            event.createdAt.isValid()
                ? QLocale().toString(event.createdAt.toLocalTime(),
                                     QLocale::ShortFormat)
                : QStringLiteral("Unknown");
        this->activity_->setItem(tableRow, 0, new QTableWidgetItem(timestamp));
        this->activity_->setItem(tableRow, 1,
                                 new QTableWidgetItem(actionLabel(event.kind)));
        this->activity_->setItem(tableRow, 2,
                                 new QTableWidgetItem(targetName(event)));
        auto *details = new QTableWidgetItem(event.text);
        details->setToolTip(event.text);
        this->activity_->setItem(tableRow, 3, details);
    }

    if (this->activity_->rowCount() == 0)
    {
        this->activityEmpty_->setText(
            QStringLiteral("%1 has no matching actions.")
                .arg(displayName(row.summary)));
    }
    else if (matchingActions > MAX_VISIBLE_ACTIVITY_ROWS)
    {
        this->activityEmpty_->setText(
            QStringLiteral("%1  ·  Showing %2 of %3 actions")
                .arg(displayName(row.summary))
                .arg(QLocale().toString(MAX_VISIBLE_ACTIVITY_ROWS))
                .arg(QLocale().toString(matchingActions)));
    }
}

void ModerationReportDialog::updateControlState()
{
    const bool hasRoster = this->context_.currentRosterAvailable;
    this->scope_->setEnabled(hasRoster);
    if (!hasRoster)
    {
        const QSignalBlocker blocker(this->scope_);
        this->scope_->setCurrentIndex(1);
        this->scope_->setToolTip(
            QStringLiteral("The channel moderator list could not be loaded."));
    }
    const bool currentTeam = hasRoster && this->scope_->currentData().toBool();
    this->addedSince_->setEnabled(currentTeam);
}

bool ModerationReportDialog::rowMatchesFilters(const Row &row) const
{
    const bool currentOnly = this->context_.currentRosterAvailable &&
                             this->scope_->currentData().toBool();
    if (currentOnly && !row.currentModerator)
    {
        return false;
    }

    const auto counts =
        selectedCounts(row.summary.counts, this->currentOutputOptions());
    if (!this->showZero_->isChecked() && counts.rawTotal() == 0)
    {
        return false;
    }

    const int days = this->addedSince_->currentData().toInt();
    if (currentOnly && days > 0)
    {
        if (!row.grantedAt.isValid() ||
            row.grantedAt < QDateTime::currentDateTimeUtc().addDays(-days))
        {
            return false;
        }
    }

    const auto search = normalized(this->search_->text());
    return search.isEmpty() || normalized(row.summary.login).contains(search) ||
           normalized(row.summary.displayName).contains(search);
}

QVector<int> ModerationReportDialog::rowIndexesForOptions(
    const ModerationReportOutputOptions &options) const
{
    QVector<int> indexes;
    const bool currentOnly =
        this->context_.currentRosterAvailable && !options.includeOutsideTeam;
    const int days = options.addedWithinDays;
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-days);

    for (int index = 0; index < this->rows_.size(); ++index)
    {
        const auto &row = this->rows_.at(index);
        if (currentOnly && !row.currentModerator)
        {
            continue;
        }
        if (!options.includeInactive &&
            selectedCounts(row.summary.counts, options).rawTotal() == 0)
        {
            continue;
        }
        if (currentOnly && days > 0 &&
            (!row.grantedAt.isValid() || row.grantedAt < cutoff))
        {
            continue;
        }
        indexes.push_back(index);
    }

    const auto sortValue = [&options](
                               const ModerationActionLogCounts &source) {
        const auto counts = selectedCounts(source, options);
        switch (options.sort)
        {
            case ModerationReportSort::Total:
                return counts.rawTotal();
            case ModerationReportSort::Bans:
                return counts.bans;
            case ModerationReportSort::Timeouts:
                return counts.timeouts;
            case ModerationReportSort::Deletes:
                return counts.deletes;
            case ModerationReportSort::Unbans:
                return counts.unbans;
            case ModerationReportSort::Untimeouts:
                return counts.untimeouts;
            case ModerationReportSort::Other:
                return counts.other;
            case ModerationReportSort::AddedNewest:
            case ModerationReportSort::AddedOldest:
            case ModerationReportSort::Name:
                return 0;
        }
        return 0;
    };
    std::stable_sort(
        indexes.begin(), indexes.end(),
        [this, &options, &sortValue](int left, int right) {
            const auto &leftRow = this->rows_.at(left);
            const auto &rightRow = this->rows_.at(right);
            if (options.sort == ModerationReportSort::AddedNewest ||
                options.sort == ModerationReportSort::AddedOldest)
            {
                const bool leftHasDate = leftRow.grantedAt.isValid();
                const bool rightHasDate = rightRow.grantedAt.isValid();
                if (leftHasDate != rightHasDate)
                {
                    return leftHasDate;
                }
                if (leftHasDate &&
                    leftRow.grantedAt != rightRow.grantedAt)
                {
                    return options.sort == ModerationReportSort::AddedNewest
                               ? leftRow.grantedAt > rightRow.grantedAt
                               : leftRow.grantedAt < rightRow.grantedAt;
                }
            }
            const int leftValue = sortValue(leftRow.summary.counts);
            const int rightValue = sortValue(rightRow.summary.counts);
            if (leftValue != rightValue)
            {
                return leftValue > rightValue;
            }
            return displayName(leftRow.summary)
                       .compare(displayName(rightRow.summary),
                                Qt::CaseInsensitive) < 0;
        });
    return indexes;
}

QString ModerationReportDialog::reportText(
    const ModerationReportOutputOptions &options) const
{
    const auto indexes = this->rowIndexesForOptions(options);
    ModerationActionLogCounts totals;
    const auto number = [](int value) { return QLocale().toString(value); };
    auto rangeText = this->context_.rangeText.trimmed();
    if (!rangeText.isEmpty())
    {
        rangeText[0] = rangeText.at(0).toUpper();
    }

    bool showStatus = false;
    for (const int index : indexes)
    {
        const auto &row = this->rows_.at(index);
        showStatus = showStatus ||
                     (options.includeOutsideTeam && !row.currentModerator);
    }

    QStringList headers{QStringLiteral("Rank"), QStringLiteral("Moderator")};
    if (options.bans)
    {
        headers.push_back(QStringLiteral("Bans"));
    }
    if (options.timeouts)
    {
        headers.push_back(QStringLiteral("Timeouts"));
    }
    if (options.deletes)
    {
        headers.push_back(QStringLiteral("Deletes"));
    }
    if (options.unbans)
    {
        headers.push_back(QStringLiteral("Unbans"));
    }
    if (options.untimeouts)
    {
        headers.push_back(QStringLiteral("Untimeouts"));
    }
    if (options.other)
    {
        headers.push_back(QStringLiteral("Other"));
    }
    headers.push_back(QStringLiteral("Total"));
    if (showStatus)
    {
        headers.push_back(QStringLiteral("Status"));
    }

    QVector<bool> rightAligned(headers.size(), true);
    rightAligned[1] = false;
    if (showStatus)
    {
        rightAligned.back() = false;
    }

    QVector<QStringList> tableRows;
    tableRows.reserve(indexes.size());

    for (int rank = 0; rank < indexes.size(); ++rank)
    {
        const int index = indexes.at(rank);
        const auto &row = this->rows_.at(index);
        const auto counts = selectedCounts(row.summary.counts, options);
        addCounts(totals, counts);
        const auto moderator = !row.summary.login.trimmed().isEmpty()
                                   ? row.summary.login.trimmed()
                                   : displayName(row.summary);
        QStringList cells{QString::number(rank + 1), moderator};
        if (options.bans)
        {
            cells.push_back(number(counts.bans));
        }
        if (options.timeouts)
        {
            cells.push_back(number(counts.timeouts));
        }
        if (options.deletes)
        {
            cells.push_back(number(counts.deletes));
        }
        if (options.unbans)
        {
            cells.push_back(number(counts.unbans));
        }
        if (options.untimeouts)
        {
            cells.push_back(number(counts.untimeouts));
        }
        if (options.other)
        {
            cells.push_back(number(counts.other));
        }
        cells.push_back(number(counts.rawTotal()));
        if (showStatus)
        {
            cells.push_back(row.currentModerator
                                ? QStringLiteral("Channel")
                                : QStringLiteral("Former / Shared Chat"));
        }
        tableRows.push_back(std::move(cells));
    }

    QStringList actionSummary;
    if (options.bans)
    {
        actionSummary.push_back(QStringLiteral("Bans %1").arg(number(totals.bans)));
    }
    if (options.timeouts)
    {
        actionSummary.push_back(
            QStringLiteral("Timeouts %1").arg(number(totals.timeouts)));
    }
    if (options.deletes)
    {
        actionSummary.push_back(
            QStringLiteral("Deletes %1").arg(number(totals.deletes)));
    }
    if (options.unbans)
    {
        actionSummary.push_back(
            QStringLiteral("Unbans %1").arg(number(totals.unbans)));
    }
    if (options.untimeouts)
    {
        actionSummary.push_back(
            QStringLiteral("Untimeouts %1").arg(number(totals.untimeouts)));
    }
    if (options.other)
    {
        actionSummary.push_back(
            QStringLiteral("Other %1").arg(number(totals.other)));
    }

    QStringList lines{
        QStringLiteral("Moderation report for #%1")
            .arg(this->context_.channelLogin),
        QStringLiteral("%1 · Generated %2")
            .arg(rangeText,
                 QLocale().toString(this->context_.generatedAtUtc.toLocalTime(),
                                    QLocale::ShortFormat)),
        QString(),
        QStringLiteral("%1 moderators · %2 actions")
            .arg(number(indexes.size()), number(totals.rawTotal())),
        actionSummary.join(QStringLiteral(" · ")),
        QString(),
    };
    int statusIndex = 2;
    if (this->context_.snapshot.truncated)
    {
        lines.insert(statusIndex++, QStringLiteral(
                                        "Incomplete report: Twitch's history "
                                        "limit was reached."));
    }
    if (options.includeEvents && this->context_.snapshot.eventsTruncated)
    {
        lines.insert(statusIndex++,
                     QStringLiteral("Activity history is partial. Some older "
                                    "actions were not retained."));
    }
    if (!this->context_.currentRosterAvailable)
    {
        lines.insert(statusIndex++, QStringLiteral(
                                        "The channel moderator list was not "
                                        "available, so everyone in Twitch's "
                                        "logs is included."));
    }
    if (tableRows.isEmpty())
    {
        lines.push_back(QStringLiteral("No moderators matched these choices."));
    }
    else
    {
        lines.append(renderPlainTextTable(headers, tableRows, rightAligned));
    }

    if (options.includeEvents)
    {
        QStringList eventHeaders{QStringLiteral("Time"),
                                 QStringLiteral("Moderator"),
                                 QStringLiteral("Action")};
        if (options.includeTargets)
        {
            eventHeaders.push_back(QStringLiteral("Target"));
        }
        if (options.includeActionText)
        {
            eventHeaders.push_back(QStringLiteral("Details"));
        }

        QVector<QStringList> eventRows;

        for (const auto &event : this->context_.snapshot.events)
        {
            if (!includesKind(options, event.kind) ||
                !std::ranges::any_of(indexes, [this, &event](int index) {
                    return sameIdentity(this->rows_.at(index), event);
                }))
            {
                continue;
            }

            const auto timestamp =
                event.createdAt.isValid()
                    ? QLocale().toString(event.createdAt.toLocalTime(),
                                         QLocale::ShortFormat)
                    : QStringLiteral("Unknown");
            auto moderator = event.moderatorLogin.trimmed();
            if (moderator.isEmpty())
            {
                moderator = event.moderatorDisplayName.trimmed();
            }
            QStringList cells{
                timestamp,
                moderator.isEmpty() ? QStringLiteral("Unknown") : moderator,
                actionLabel(event.kind),
            };
            if (options.includeTargets)
            {
                const auto target = targetName(event);
                cells.push_back(target.isEmpty() ? QStringLiteral("Unknown")
                                                 : target);
            }
            if (options.includeActionText)
            {
                auto details = event.text.simplified();
                if (details.size() > 96)
                {
                    details = details.left(93) + QStringLiteral("...");
                }
                cells.push_back(details);
            }
            eventRows.push_back(std::move(cells));
        }

        lines.push_back(QString());
        lines.push_back(QStringLiteral("Activity"));
        if (eventRows.isEmpty())
        {
            lines.push_back(QStringLiteral("No activity matched these choices."));
        }
        else
        {
            lines.append(renderPlainTextTable(
                eventHeaders, eventRows,
                QVector<bool>(eventHeaders.size(), false)));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QByteArray ModerationReportDialog::reportJson(
    const ModerationReportOutputOptions &options) const
{
    QStringList includedActions;
    if (options.bans)
    {
        includedActions.push_back(QStringLiteral("ban"));
    }
    if (options.timeouts)
    {
        includedActions.push_back(QStringLiteral("timeout"));
    }
    if (options.deletes)
    {
        includedActions.push_back(QStringLiteral("delete"));
    }
    if (options.unbans)
    {
        includedActions.push_back(QStringLiteral("unban"));
    }
    if (options.untimeouts)
    {
        includedActions.push_back(QStringLiteral("untimeout"));
    }
    if (options.other)
    {
        includedActions.push_back(QStringLiteral("other"));
    }

    const auto indexes = this->rowIndexesForOptions(options);
    QJsonArray moderators;
    ModerationActionLogCounts totals;
    for (const int index : indexes)
    {
        const auto &row = this->rows_.at(index);
        const auto counts = selectedCounts(row.summary.counts, options);
        addCounts(totals, counts);
        moderators.append(QJsonObject{
            {QStringLiteral("id"), row.summary.id},
            {QStringLiteral("login"), row.summary.login},
            {QStringLiteral("displayName"), row.summary.displayName},
            {QStringLiteral("currentModerator"), row.currentModerator},
            {QStringLiteral("broadcaster"), row.broadcaster},
            {QStringLiteral("grantedAt"),
             row.grantedAt.isValid()
                 ? row.grantedAt.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
            {QStringLiteral("counts"), countsJson(counts)},
        });
    }

    QJsonArray events;
    if (options.includeEvents)
    {
        for (const auto &event : this->context_.snapshot.events)
        {
            if (!event.createdAt.isValid() ||
                !includesKind(options, event.kind))
            {
                continue;
            }
            const bool actorIncluded =
                std::ranges::any_of(indexes, [this, &event](int index) {
                    return sameIdentity(this->rows_.at(index), event);
                });
            if (!actorIncluded)
            {
                continue;
            }

            QJsonObject item{
                {QStringLiteral("id"), event.id},
                {QStringLiteral("kind"), moderationActionKindText(event.kind)},
                {QStringLiteral("createdAt"),
                 event.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                {QStringLiteral("moderatorId"), event.moderatorId},
                {QStringLiteral("moderatorLogin"), event.moderatorLogin},
                {QStringLiteral("moderatorDisplayName"),
                 event.moderatorDisplayName},
            };
            if (options.includeTargets)
            {
                item.insert(QStringLiteral("targetId"), event.targetId);
                item.insert(QStringLiteral("targetLogin"), event.targetLogin);
                item.insert(QStringLiteral("targetDisplayName"),
                            event.targetDisplayName);
            }
            if (options.includeActionText)
            {
                item.insert(QStringLiteral("text"), event.text);
            }
            events.append(item);
        }
    }

    const auto root = QJsonObject{
        {QStringLiteral("format"), QString::fromLatin1(REPORT_FORMAT)},
        {QStringLiteral("version"), REPORT_VERSION},
        {QStringLiteral("generatedAt"),
         this->context_.generatedAtUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("channel"),
         QJsonObject{{QStringLiteral("id"), this->context_.channelId},
                     {QStringLiteral("login"), this->context_.channelLogin}}},
        {QStringLiteral("range"),
         QJsonObject{
             {QStringLiteral("label"), this->context_.rangeText},
             {QStringLiteral("cutoff"),
              this->context_.cutoffUtc.isValid()
                  ? this->context_.cutoffUtc.toUTC().toString(Qt::ISODateWithMs)
                  : QString()},
             {QStringLiteral("complete"), !this->context_.snapshot.truncated},
             {QStringLiteral("pagesRead"), this->context_.snapshot.pagesRead},
         }},
        {QStringLiteral("totals"), countsJson(totals)},
        {QStringLiteral("moderators"), moderators},
        {QStringLiteral("events"), events},
        {QStringLiteral("details"),
         QJsonObject{
             {QStringLiteral("included"), options.includeEvents},
             {QStringLiteral("includedActions"),
              QJsonArray::fromStringList(includedActions)},
             {QStringLiteral("currentRosterAvailable"),
              this->context_.currentRosterAvailable},
             {QStringLiteral("outsideTeamIncluded"),
              options.includeOutsideTeam},
             {QStringLiteral("targetsIncluded"), options.includeTargets},
             {QStringLiteral("actionTextIncluded"), options.includeActionText},
             {QStringLiteral("truncated"),
              options.includeEvents && this->context_.snapshot.eventsTruncated},
         }},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray ModerationReportDialog::reportCsv(
    const ModerationReportOutputOptions &options) const
{
    QStringList headers{QStringLiteral("Moderator")};
    if (options.includeOutsideTeam)
    {
        headers.push_back(QStringLiteral("Channel moderator"));
    }
    headers.push_back(QStringLiteral("Total"));
    if (options.bans)
    {
        headers.push_back(QStringLiteral("Bans"));
    }
    if (options.timeouts)
    {
        headers.push_back(QStringLiteral("Timeouts"));
    }
    if (options.deletes)
    {
        headers.push_back(QStringLiteral("Deletes"));
    }
    if (options.unbans)
    {
        headers.push_back(QStringLiteral("Unbans"));
    }
    if (options.untimeouts)
    {
        headers.push_back(QStringLiteral("Untimeouts"));
    }
    if (options.other)
    {
        headers.push_back(QStringLiteral("Other"));
    }
    headers.push_back(QStringLiteral("Added"));

    QStringList lines{headers.join(QLatin1Char(','))};
    for (const int index : this->rowIndexesForOptions(options))
    {
        const auto &row = this->rows_.at(index);
        const auto counts = selectedCounts(row.summary.counts, options);
        QStringList cells{csvCell(displayName(row.summary))};
        if (options.includeOutsideTeam)
        {
            cells.push_back(row.currentModerator ? QStringLiteral("Yes")
                                                 : QStringLiteral("No"));
        }
        cells.push_back(QString::number(counts.rawTotal()));
        if (options.bans)
        {
            cells.push_back(QString::number(counts.bans));
        }
        if (options.timeouts)
        {
            cells.push_back(QString::number(counts.timeouts));
        }
        if (options.deletes)
        {
            cells.push_back(QString::number(counts.deletes));
        }
        if (options.unbans)
        {
            cells.push_back(QString::number(counts.unbans));
        }
        if (options.untimeouts)
        {
            cells.push_back(QString::number(counts.untimeouts));
        }
        if (options.other)
        {
            cells.push_back(QString::number(counts.other));
        }
        cells.push_back(
            csvCell(row.grantedAt.isValid()
                        ? row.grantedAt.toUTC().toString(Qt::ISODateWithMs)
                        : QString()));
        lines.push_back(cells.join(QLatin1Char(',')));
    }
    return lines.join(QLatin1Char('\n')).toUtf8();
}

void ModerationReportDialog::openExportDialog()
{
    const QPointer<ModerationReportDialog> self(this);
    QPointer<ExportReportDialog> dialog = new ExportReportDialog(
        this->currentOutputOptions(), this->context_.currentRosterAvailable, this);
    const auto result = dialog->exec();
    if (!self || !dialog)
    {
        return;
    }
    const auto options = dialog->options();
    delete dialog;
    if (result != QDialog::Accepted)
    {
        return;
    }

    QString extension;
    QString filter;
    QByteArray data;
    switch (options.fileFormat)
    {
        case ModerationReportFileFormat::Csv:
            extension = QStringLiteral("csv");
            filter = QStringLiteral("CSV table (*.csv)");
            data = this->reportCsv(options);
            break;
        case ModerationReportFileFormat::Json:
            extension = QStringLiteral("json");
            filter = QStringLiteral("JSON report (*.json)");
            data = this->reportJson(options);
            break;
    }

    auto suggested = QStringLiteral("%1-moderation-report.%2")
                         .arg(this->context_.channelLogin, extension);
    auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save moderation data"), suggested, filter);
    if (!self || path.isEmpty())
    {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty())
    {
        path += QLatin1Char('.') + extension;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(data) != data.size() || !file.commit())
    {
        showReportMessage(
            this, QMessageBox::Warning,
            QStringLiteral("Could not save the file"),
            QStringLiteral("Choose another location and try again."));
        return;
    }
    this->operationStatusLabel_->setText(QStringLiteral("Export saved."));
    this->operationStatusLabel_->setToolTip({});
    this->operationStatusLabel_->setVisible(true);
}

void ModerationReportDialog::openShareDialog()
{
    const QPointer<ModerationReportDialog> self(this);
    QPointer<ShareReportDialog> dialog = new ShareReportDialog(
        this->currentOutputOptions(), this->context_.currentRosterAvailable, this);
    const auto result = dialog->exec();
    if (!self || !dialog)
    {
        return;
    }
    const auto options = dialog->options();
    delete dialog;
    if (result == QDialog::Accepted)
    {
        this->shareReport(options);
    }
}

void ModerationReportDialog::addSharedReportMessage(
    const QString &url, const ModerationReportOutputOptions &options) const
{
    const auto channel = this->context_.outputChannel.lock();
    if (channel == nullptr)
    {
        return;
    }

    const auto indexes = this->rowIndexesForOptions(options);
    ModerationActionLogCounts totals;
    for (const int index : indexes)
    {
        addCounts(totals,
                  selectedCounts(this->rows_.at(index).summary.counts, options));
    }

    const auto moderatorCount = indexes.size() == 1
                                    ? QStringLiteral("1 moderator")
                                    : QStringLiteral("%1 moderators")
                                          .arg(QLocale().toString(indexes.size()));
    const auto actionCount = totals.rawTotal() == 1
                                 ? QStringLiteral("1 action")
                                 : QStringLiteral("%1 actions")
                                       .arg(QLocale().toString(
                                           totals.rawTotal()));
    const auto prefix =
        QStringLiteral("Shared moderation report for #%1, %2. %3, %4. ")
            .arg(this->context_.channelLogin, this->context_.rangeText,
                 moderatorCount, actionCount);
    const auto linkText = QStringLiteral("Open shared report");

    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(QStringList{linkText}, MessageElementFlag::Text,
                              MessageColor::Link, FontStyle::ChatMediumBold)
        ->setLink({Link::Url, url});
    builder->messageText = prefix + linkText;
    builder->searchText = builder->messageText;
    channel->addMessage(builder.release(), MessageContext::Original);
}

void ModerationReportDialog::shareReport(
    const ModerationReportOutputOptions &options)
{
    if (!this->shareButton_->isEnabled())
    {
        return;
    }
    const auto report = this->reportText(options);
    if (report.toUtf8().size() > MAX_SHARED_REPORT_BYTES)
    {
        showReportMessage(
            this, QMessageBox::Information,
            QStringLiteral("Report is too large"),
            QStringLiteral("Turn off activity history or include fewer "
                           "actions, then try again."));
        return;
    }
    QJsonObject payload{
        {QStringLiteral("source"), QStringLiteral("client")},
        {QStringLiteral("title"), QStringLiteral("Moderation report for #%1")
                                      .arg(this->context_.channelLogin)},
        {QStringLiteral("language"), QStringLiteral("text")},
        {QStringLiteral("content"), report},
    };

    this->shareButton_->setEnabled(false);
    this->operationStatusLabel_->setText(QStringLiteral("Creating link..."));
    this->operationStatusLabel_->setToolTip({});
    this->operationStatusLabel_->setVisible(true);
    const QPointer<ModerationReportDialog> self(this);
    NetworkRequest(QUrl(QStringLiteral("https://h.moltorino.com/api/paste")),
                   NetworkRequestType::Post)
        .timeout(10000)
        .maximumResponseSize(1024 * 1024)
        .hideRequestBody()
        .json(payload)
        .onSuccess([self, options](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            const auto url = hasteRawUrl(result.parseJson());
            if (url.isEmpty())
            {
                self->operationStatusLabel_->setText(
                    QStringLiteral("The service returned an invalid link."));
                self->operationStatusLabel_->setToolTip({});
                return;
            }

            self->operationStatusLabel_->setText(QStringLiteral("Link ready."));
            self->operationStatusLabel_->setToolTip({});
            self->addSharedReportMessage(url, options);
            QPointer<QMessageBox> box = new QMessageBox(self);
            box->setWindowTitle(QStringLiteral("Report ready"));
            box->setText(QStringLiteral("Your sharing link is ready."));
            auto *copy = box->addButton(QStringLiteral("Copy link"),
                                       QMessageBox::AcceptRole);
            auto *open =
                box->addButton(QStringLiteral("Open"), QMessageBox::ActionRole);
            box->addButton(QMessageBox::Close);
            installMoltorinoDialogTheme(box);
            box->exec();
            if (!self || !box)
            {
                return;
            }
            const auto *clicked = box->clickedButton();
            const bool copyLink = clicked == copy;
            const bool openLink = clicked == open;
            delete box;
            if (copyLink)
            {
                crossPlatformCopy(url);
                self->operationStatusLabel_->setText(
                    QStringLiteral("Link copied."));
                self->operationStatusLabel_->setToolTip({});
            }
            else if (openLink)
            {
                QDesktopServices::openUrl(QUrl(url));
            }
        })
        .onError([self](const NetworkResult &result) {
            if (!self)
            {
                return;
            }
            const auto root = result.parseJson();
            const auto detail =
                root.value(QStringLiteral("error")).toString().trimmed();
            self->operationStatusLabel_->setText(
                QStringLiteral("Could not create a sharing link."));
            self->operationStatusLabel_->setToolTip(detail);
        })
        .finally([self] {
            if (self)
            {
                self->shareButton_->setEnabled(true);
            }
        })
        .caller(this)
        .execute();
}

}
