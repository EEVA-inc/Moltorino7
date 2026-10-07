#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QProcessEnvironment>
#include <QThreadPool>
#include <QTimer>

#include <optional>

#include <pajlada/signals/signal.hpp>

class QJsonObject;

namespace chatterino {

class BaseWindow;
class UpdateDialog;

enum class MoltorinoUpdateStatus {
    Disabled,
    Idle,
    Checking,
    Downloading,
    Ready,
    Applying,
    UpToDate,
    Error,
};

class MoltorinoUpdater final : public QObject
{
public:
    static MoltorinoUpdater &instance();

    void init(bool automaticUpdatesEnabled);
    void checkForUpdates(bool userInitiated = false);
    void retry();
    bool requestUpdatePrompt();

    void showUpdateDialog();
    void showFullChangelog();
    void restartToUpdate();
    bool restartApplication(
        const QProcessEnvironment &environment = QProcessEnvironment());

    void prepareForApplicationQuit();

    MoltorinoUpdateStatus status() const;
    QString statusText() const;
    QString channel() const;
    QString platform() const;
    int protocol() const;
    QString targetBuild() const;
    QString targetPackageVersion() const;
    QString summaryMarkdown() const;
    QString errorMessage() const;
    int progress() const;
    bool isRollback() const;

    bool isAvailable() const;
    bool isBusy() const;
    bool isError() const;
    bool canRestartToUpdate() const;
    bool shouldShowUpdateButton() const;

    pajlada::Signals::NoArgSignal stateChanged;

private:
    struct ReleaseMetadata {
        QString id;
        QString channel;
        QString velopackChannel;
        QString build;
        QString packageVersion;
        QString summaryMarkdown;
        QString feedBaseUrl;
        int minimumUpdaterVersion{2};
        bool rollback{};
    };

    struct PendingUpdate {
        QString releaseId;
        QString channel;
        QString velopackChannel;
        QString build;
        QString packageVersion;
        QString summaryMarkdown;
        QString feedBaseUrl;
        bool rollback{};
    };

    MoltorinoUpdater();

    void fetchMetadata(bool userInitiated, int generation);
    void startVelopackUpdate(const ReleaseMetadata &metadata,
                             bool userInitiated, int generation);
    void finishNoUpdate(int generation, bool userInitiated);
    void finishReady(int generation, PendingUpdate update);
    void revalidateDownloadedUpdate();
    void finishError(int generation, QString message, bool userInitiated);
    void updateProgress(int generation, int progress);

    bool scheduleApply(bool restart, QString &error);
    void forceApplicationQuit();

    QString stateFilePath() const;
    void loadPendingState();
    void savePendingState() const;
    void clearPendingState();
    void cleanupLegacyUpdaterArtifacts() const;
    QString installedPackageVersion() const;
    bool hasStagedPackage() const;
    static std::optional<bool> shouldAcceptPackageVersion(
        const QString &targetVersion, const QString &currentVersion,
        bool rollback);
    void setStatus(MoltorinoUpdateStatus status,
                   const QString &error = {});

    MoltorinoUpdateStatus status_{MoltorinoUpdateStatus::Disabled};
    QString channel_;
    QString errorMessage_;
    int progress_{};
    int operationGeneration_{};
    bool initialized_{};
    bool automaticUpdatesEnabled_{};
    bool operationInFlight_{};
    bool applyScheduled_{};
    bool restartRequested_{};
    bool quitCoordinated_{};
    bool pendingValidatedThisRun_{};
    bool showReadyDialogAfterValidation_{};
    bool deferUpdateForExplicitRestart_{};
    std::optional<QProcessEnvironment> restartEnvironment_;

    std::optional<ReleaseMetadata> metadata_;
    std::optional<PendingUpdate> pendingUpdate_;
    QTimer pollTimer_;
    QTimer retryTimer_;
    QTimer readyValidationTimer_;
    QElapsedTimer pendingValidationAge_;
    QThreadPool updatePool_;
    QPointer<UpdateDialog> updateDialog_;
};

MoltorinoUpdater *getMoltorinoUpdater();

}
