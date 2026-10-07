#pragma once

#include "ForwardDecl.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/ModerationActionLogs.hpp"

#include <QDialog>
#include <QVector>

#include <optional>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace chatterino {

struct ModerationReportContext {
    QString channelId;
    QString channelLogin;
    QString rangeText;
    QDateTime cutoffUtc;
    QDateTime generatedAtUtc;
    ModerationActionLogScanSnapshot snapshot;
    std::vector<HelixModerator> currentModerators;
    std::weak_ptr<Channel> outputChannel;
    bool currentRosterAvailable = false;
};

struct ModerationReportRequest {
    QString channelLogin;
    int days = 7;
    QString rangeText = QStringLiteral("last 7 days");
};

enum class ModerationReportFileFormat {
    Csv,
    Json,
};

enum class ModerationReportSort {
    Total,
    Bans,
    Timeouts,
    Deletes,
    Unbans,
    Untimeouts,
    Other,
    AddedNewest,
    AddedOldest,
    Name,
};

struct ModerationReportOutputOptions {
    bool bans = true;
    bool timeouts = true;
    bool deletes = false;
    bool unbans = false;
    bool untimeouts = false;
    bool other = false;
    bool includeOutsideTeam = false;
    bool includeInactive = true;
    int addedWithinDays = 0;
    ModerationReportSort sort = ModerationReportSort::Total;
    bool includeEvents = false;
    bool includeTargets = false;
    bool includeActionText = false;
    ModerationReportFileFormat fileFormat = ModerationReportFileFormat::Csv;
};

std::optional<ModerationReportRequest> requestModerationReport(
    QWidget *parent, const QString &defaultChannel);
QString rememberModerationReport(ModerationReportContext context);
bool openRememberedModerationReport(const QString &reportId,
                                    QWidget *parent = nullptr);

class ModerationReportDialog final : public QDialog
{
public:
    explicit ModerationReportDialog(ModerationReportContext context,
                                    QWidget *parent = nullptr);

private:
    struct Row {
        ModerationActionLogModeratorSummary summary;
        bool currentModerator = false;
        bool broadcaster = false;
        QDateTime grantedAt;
    };

    void buildRows();
    void refreshRows();
    void refreshThemeColors();
    void refreshActivity();
    void updateControlState();
    void openExportDialog();
    void openShareDialog();
    void shareReport(const ModerationReportOutputOptions &options);
    void addSharedReportMessage(
        const QString &url,
        const ModerationReportOutputOptions &options) const;

    [[nodiscard]] bool rowMatchesFilters(const Row &row) const;
    [[nodiscard]] bool actionEnabled(GqlModerationActionKind kind) const;
    [[nodiscard]] ModerationReportOutputOptions currentOutputOptions() const;
    [[nodiscard]] QVector<int> rowIndexesForOptions(
        const ModerationReportOutputOptions &options) const;
    [[nodiscard]] QString reportText(
        const ModerationReportOutputOptions &options) const;
    [[nodiscard]] QByteArray reportJson(
        const ModerationReportOutputOptions &options) const;
    [[nodiscard]] QByteArray reportCsv(
        const ModerationReportOutputOptions &options) const;

    ModerationReportContext context_;
    QVector<Row> rows_;

    QLabel *totalLabel_{};
    QLabel *statusLabel_{};
    QLabel *operationStatusLabel_{};
    QLineEdit *search_{};
    QComboBox *scope_{};
    QCheckBox *showZero_{};
    QComboBox *addedSince_{};
    QCheckBox *bans_{};
    QCheckBox *timeouts_{};
    QCheckBox *deletes_{};
    QCheckBox *unbans_{};
    QCheckBox *untimeouts_{};
    QCheckBox *other_{};
    QTableWidget *moderators_{};
    QTableWidget *activity_{};
    QLabel *activityEmpty_{};
    QPushButton *exportButton_{};
    QPushButton *shareButton_{};
};

}
