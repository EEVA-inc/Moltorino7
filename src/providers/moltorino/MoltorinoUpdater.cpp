#include "providers/moltorino/MoltorinoUpdater.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/dialogs/UpdateDialog.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVersionNumber>
#include <QtConcurrent>

#ifdef MOLTORINO_VELOPACK_ENABLED
#    include <Velopack.hpp>
#endif

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace chatterino {

Q_LOGGING_CATEGORY(moltorinoUpdater, "chatterino.moltorino.updater")

namespace {

constexpr auto UPDATE_API_BASE = "https://api.moltorino.com";
constexpr int STARTUP_CHECK_DELAY_MS = 8000;
constexpr int POLL_INTERVAL_MS = 4 * 60 * 60 * 1000;
constexpr int RETRY_INTERVAL_MS = 15 * 60 * 1000;
constexpr int READY_REVALIDATION_INTERVAL_MS = 5 * 60 * 1000;
constexpr int READY_VALIDATION_LEASE_MS = 10 * 60 * 1000;
constexpr auto REMOTE_RESTART_ARGUMENT = "--remote-restart";

QString updaterPlatformSlug()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return {};
#endif
}

QString updaterFeedPath()
{
    return QStringLiteral("/v2/updates/%1").arg(updaterPlatformSlug());
}

QString normalizedVersion(QString version)
{
    version.remove(QChar::Null);
    version = version.trimmed();
    if (version.startsWith('v', Qt::CaseInsensitive))
    {
        version.remove(0, 1);
    }
    return version;
}

std::optional<int> comparePackageVersions(const QString &left,
                                          const QString &right)
{
    const auto normalizedLeft = normalizedVersion(left);
    const auto normalizedRight = normalizedVersion(right);
    qsizetype leftSuffix = 0;
    qsizetype rightSuffix = 0;
    const auto leftVersion =
        QVersionNumber::fromString(normalizedLeft, &leftSuffix);
    const auto rightVersion =
        QVersionNumber::fromString(normalizedRight, &rightSuffix);
    if (leftVersion.segmentCount() < 3 || rightVersion.segmentCount() < 3 ||
        leftSuffix != normalizedLeft.size() ||
        rightSuffix != normalizedRight.size())
    {
        return std::nullopt;
    }
    return QVersionNumber::compare(leftVersion, rightVersion);
}

QString absoluteUpdateUrl(const QString &pathOrUrl)
{
    if (pathOrUrl.trimmed().isEmpty())
    {
        return {};
    }

    const QUrl candidate(pathOrUrl);
    if (candidate.isValid() && !candidate.scheme().isEmpty())
    {
        return candidate.toString();
    }

    return QUrl(QString::fromLatin1(UPDATE_API_BASE))
        .resolved(QUrl(pathOrUrl))
        .toString();
}

bool isTrustedAutomaticFeed(const QString &url)
{
    const QUrl candidate(url);
    const QUrl api(QString::fromLatin1(UPDATE_API_BASE));
    const auto expectedPath = updaterFeedPath();
    return candidate.isValid() && candidate.scheme() == "https" &&
           candidate.host().compare(api.host(), Qt::CaseInsensitive) == 0 &&
           candidate.port(-1) == -1 &&
           candidate.path() == expectedPath &&
           candidate.userInfo().isEmpty() && candidate.query().isEmpty() &&
           candidate.fragment().isEmpty();
}

QStringList restartArguments()
{
    QStringList args;
    const auto currentArgs = QCoreApplication::arguments();
    for (int i = 1; i < currentArgs.size(); ++i)
    {
        const auto &arg = currentArgs.at(i);
        if (arg == REMOTE_RESTART_ARGUMENT || arg == "--crash-recovery")
        {
            continue;
        }
        if (arg == "--cr-exception-code" || arg == "--cr-exception-message")
        {
            ++i;
            continue;
        }
        if (arg == "--moltorino-migration-ready-file")
        {
            ++i;
            continue;
        }
        if (arg.startsWith("--cr-exception-code=") ||
            arg.startsWith("--cr-exception-message=") ||
            arg.startsWith("--moltorino-migration-ready-file="))
        {
            continue;
        }
        args.append(arg);
    }
    args.append(REMOTE_RESTART_ARGUMENT);
    return args;
}

QString velopackChannelFor(const QString &logicalChannel)
{
    QString platformChannel;
#if defined(Q_OS_WIN)
    platformChannel = QStringLiteral("win-x64");
#elif defined(Q_OS_MACOS)
    platformChannel = QStringLiteral("osx-universal");
#elif defined(Q_OS_LINUX)
    platformChannel = QStringLiteral("linux-x64");
#endif
    return platformChannel +
           (logicalChannel == "internal" ? QStringLiteral("-internal")
                                          : QStringLiteral("-stable"));
}

QString velopackPackageIdFor(const QString &logicalChannel)
{
    return logicalChannel == "internal"
               ? "MoltoBenne.Moltorino7UpdaterTest"
               : "MoltoBenne.Moltorino7";
}

#ifdef MOLTORINO_VELOPACK_ENABLED
Velopack::UpdateOptions updateOptions(const QString &channel,
                                      bool allowVersionDowngrade = false)
{
    Velopack::UpdateOptions options{};
    options.AllowVersionDowngrade = allowVersionDowngrade;
    options.ExplicitChannel = channel.toStdString();
    options.MaximumDeltasBeforeFallback = 10;
    return options;
}
#endif

}

MoltorinoUpdater::MoltorinoUpdater()
{
    this->updatePool_.setMaxThreadCount(1);
    this->updatePool_.setExpiryTimeout(30000);
    this->pollTimer_.setInterval(POLL_INTERVAL_MS);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this, [this] {
        this->checkForUpdates(false);
    });
    this->retryTimer_.setInterval(RETRY_INTERVAL_MS);
    this->retryTimer_.setSingleShot(true);
    QObject::connect(&this->retryTimer_, &QTimer::timeout, this, [this] {
        this->checkForUpdates(false);
    });
    this->readyValidationTimer_.setInterval(READY_REVALIDATION_INTERVAL_MS);
    QObject::connect(&this->readyValidationTimer_, &QTimer::timeout, this,
                     [this] {
                         this->checkForUpdates(false);
                     });
}

MoltorinoUpdater &MoltorinoUpdater::instance()
{
    static auto *instance = new MoltorinoUpdater();
    return *instance;
}

void MoltorinoUpdater::init(bool automaticUpdatesEnabled)
{
    if (this->initialized_)
    {
        return;
    }
    this->initialized_ = true;
    this->automaticUpdatesEnabled_ = automaticUpdatesEnabled;
#if defined(Q_OS_LINUX) && !defined(Q_PROCESSOR_X86_64)
    this->automaticUpdatesEnabled_ = false;
#endif

#ifdef MOLTORINO_DEFAULT_UPDATE_CHANNEL
    this->channel_ =
        QStringLiteral(MOLTORINO_DEFAULT_UPDATE_CHANNEL).trimmed().toLower();
#else
    this->channel_ = Version::instance().isNightly() ? "internal" : "stable";
#endif
    if (this->channel_ != "internal" && this->channel_ != "stable")
    {
        qCWarning(moltorinoUpdater)
            << "Invalid compiled update channel; falling back to stable:"
            << this->channel_;
        this->channel_ = "stable";
    }

#ifdef MOLTORINO_VELOPACK_ENABLED
    if (!this->automaticUpdatesEnabled_)
    {
        this->setStatus(MoltorinoUpdateStatus::Disabled);
        return;
    }

    this->loadPendingState();
    const auto currentPackageVersion = this->installedPackageVersion();
    if (currentPackageVersion.isEmpty())
    {
        this->pendingUpdate_.reset();
        this->setStatus(MoltorinoUpdateStatus::Disabled);
        return;
    }
    if (this->pendingUpdate_.has_value() &&
        normalizedVersion(this->pendingUpdate_->packageVersion) ==
            normalizedVersion(currentPackageVersion))
    {
        this->clearPendingState();
        this->setStatus(MoltorinoUpdateStatus::UpToDate);
    }
    else if (this->pendingUpdate_.has_value())
    {
        const auto comparison = comparePackageVersions(
            this->pendingUpdate_->packageVersion, currentPackageVersion);
        if (!comparison.has_value() || *comparison == 0 ||
            (*comparison < 0 && !this->pendingUpdate_->rollback))
        {
            qCWarning(moltorinoUpdater)
                << "Discarding stale or invalid updater state for"
                << this->pendingUpdate_->packageVersion;
            this->clearPendingState();
            this->setStatus(MoltorinoUpdateStatus::Idle);
        }
        else if (this->hasStagedPackage())
        {
            this->setStatus(MoltorinoUpdateStatus::Idle);
        }
        else
        {
            qCWarning(moltorinoUpdater)
                << "Discarding updater state because the staged package is missing.";
            this->clearPendingState();
            this->setStatus(MoltorinoUpdateStatus::Idle);
        }
    }
    else
    {
        this->setStatus(MoltorinoUpdateStatus::Idle);
    }

    QTimer::singleShot(STARTUP_CHECK_DELAY_MS, this, [this] {
        this->checkForUpdates(false);
    });
    QTimer::singleShot(15000, this, [this] {
        this->cleanupLegacyUpdaterArtifacts();
    });
    this->pollTimer_.start();
#else
    this->setStatus(MoltorinoUpdateStatus::Disabled);
#endif
}

void MoltorinoUpdater::checkForUpdates(bool userInitiated)
{
    if (this->quitCoordinated_ || isAppAboutToQuit())
    {
        return;
    }
#ifdef MOLTORINO_VELOPACK_ENABLED
    if (!this->initialized_)
    {
        qCWarning(moltorinoUpdater)
            << "Ignoring an update check before updater initialization.";
        return;
    }
    if (!this->automaticUpdatesEnabled_ ||
        this->status_ == MoltorinoUpdateStatus::Disabled)
    {
        if (userInitiated)
        {
            this->showUpdateDialog();
        }
        return;
    }
    if (this->operationInFlight_ || this->applyScheduled_)
    {
        if (userInitiated)
        {
            this->showUpdateDialog();
        }
        return;
    }
    if (this->status_ == MoltorinoUpdateStatus::Ready && userInitiated &&
        this->canRestartToUpdate())
    {
        this->showUpdateDialog();
        return;
    }

    this->operationInFlight_ = true;
    this->retryTimer_.stop();
    this->progress_ = 0;
    this->errorMessage_.clear();
    const int generation = ++this->operationGeneration_;
    this->setStatus(MoltorinoUpdateStatus::Checking);
    if (userInitiated)
    {
        this->showUpdateDialog();
    }
    this->fetchMetadata(userInitiated, generation);
#else
    if (userInitiated)
    {
        this->showUpdateDialog();
    }
#endif
}

void MoltorinoUpdater::retry()
{
    this->checkForUpdates(true);
}

bool MoltorinoUpdater::requestUpdatePrompt()
{
    if (!this->automaticUpdatesEnabled_ ||
        this->status_ == MoltorinoUpdateStatus::Disabled ||
        this->applyScheduled_ || this->quitCoordinated_ || isAppAboutToQuit())
    {
        return false;
    }
    if (this->pendingUpdate_.has_value() &&
        !this->pendingValidatedThisRun_ && !this->operationInFlight_)
    {
        this->checkForUpdates(true);
    }
    else if (this->isAvailable() || this->isBusy() || this->isError())
    {
        this->showUpdateDialog();
    }
    else
    {
        this->checkForUpdates(true);
    }
    return true;
}

void MoltorinoUpdater::fetchMetadata(bool userInitiated, int generation)
{
    QUrl url(QString::fromLatin1(UPDATE_API_BASE) + updaterFeedPath() +
             QStringLiteral("/metadata"));
    QUrlQuery query;
    query.addQueryItem("channel", this->channel_);
    url.setQuery(query);

    NetworkRequest(url)
        .header("Accept", "application/json")
        .timeout(10000)
        .maximumResponseSize(1024 * 1024)
        .caller(this)
        .onSuccess([this, userInitiated, generation](const NetworkResult &result) {
            if (generation != this->operationGeneration_)
            {
                return;
            }

            const auto root = result.parseJson();
            const auto serviceProtocol = root.value("updaterProtocol").toInt();
            if (!root.value("ok").toBool(false) ||
                root.value("platform").toString() != updaterPlatformSlug() ||
                serviceProtocol != this->protocol())
            {
                this->finishError(
                    generation,
                    "The update service returned an unsupported protocol response.",
                    userInitiated);
                return;
            }
            auto release = root.value("release").toObject();
            if (release.isEmpty())
            {
                release = root.value("activeRelease").toObject();
            }
            if (release.isEmpty() ||
                root.value("available").toBool(true) == false)
            {
                this->clearPendingState();
                this->finishNoUpdate(generation, userInitiated);
                return;
            }

            ReleaseMetadata metadata;
            metadata.id = release.value("id").toString().trimmed();
            metadata.channel =
                release.value("channel")
                    .toString(root.value("channel").toString(this->channel_))
                    .trimmed();
            metadata.velopackChannel =
                release.value("velopackChannel").toString().trimmed();
            metadata.build = release.value("build").toString().trimmed();
            if (metadata.build.isEmpty())
            {
                metadata.build = release.value("buildId").toString().trimmed();
            }
            if (metadata.build.isEmpty())
            {
                metadata.build = release.value("version").toString().trimmed();
            }
            metadata.packageVersion =
                release.value("packageVersion").toString().trimmed();
            metadata.summaryMarkdown =
                release.value("summaryMarkdown").toString().trimmed();
            if (metadata.summaryMarkdown.isEmpty())
            {
                metadata.summaryMarkdown =
                    release.value("shortSummary").toString().trimmed();
            }
            if (metadata.summaryMarkdown.isEmpty())
            {
                metadata.summaryMarkdown =
                    release.value("summary").toString().trimmed();
            }
            metadata.feedBaseUrl = absoluteUpdateUrl(
                release.value("feedBaseUrl").toString().trimmed());
            if (metadata.feedBaseUrl.isEmpty())
            {
                metadata.feedBaseUrl = absoluteUpdateUrl(
                    root.value("feedBaseUrl").toString().trimmed());
            }

            const auto minimumUpdaterVersion =
                release.value("minimumUpdaterVersion");
            metadata.minimumUpdaterVersion =
                minimumUpdaterVersion.isDouble()
                    ? minimumUpdaterVersion.toInt(-1)
                    : -1;
            const bool allowDowngrade =
                release.contains("allowDowngrade")
                    ? release.value("allowDowngrade").toBool(false)
                    : release.value("rollback").toBool(false);
            const auto expectedFeedFile =
                "releases." + metadata.velopackChannel + ".json";
            const auto immutableState =
                release.value("immutableState").toString();
            const bool validReleaseState =
                immutableState == QStringLiteral("active");

            if (metadata.minimumUpdaterVersion > this->protocol())
            {
                this->finishError(
                    generation,
                    "This update needs a newer version of Moltorino's updater.",
                    userInitiated);
                return;
            }

            if (metadata.id.isEmpty() || metadata.build.isEmpty() ||
                metadata.packageVersion.isEmpty() ||
                metadata.summaryMarkdown.isEmpty() ||
                metadata.minimumUpdaterVersion < 1 ||
                metadata.channel != this->channel_ ||
                metadata.velopackChannel !=
                    velopackChannelFor(metadata.channel) ||
                release.value("preview").toBool(false) ||
                !validReleaseState ||
                release.value("feedFileName").toString() != expectedFeedFile ||
                !isTrustedAutomaticFeed(metadata.feedBaseUrl))
            {
                this->finishError(generation,
                                  "The update service returned incomplete release details.",
                                  userInitiated);
                return;
            }

            const auto currentPackageVersion = this->installedPackageVersion();
            const auto versionComparison = comparePackageVersions(
                metadata.packageVersion, currentPackageVersion);
            const auto shouldAccept = shouldAcceptPackageVersion(
                metadata.packageVersion, currentPackageVersion,
                allowDowngrade);
            if (!shouldAccept.has_value())
            {
                this->finishError(generation,
                                  "The update service returned an invalid package version.",
                                  userInitiated);
                return;
            }
            if (!*shouldAccept)
            {
                if (versionComparison.value_or(0) < 0)
                {
                    qCWarning(moltorinoUpdater)
                        << "Ignoring an updater downgrade without explicit rollback authorization from"
                        << currentPackageVersion << "to"
                        << metadata.packageVersion;
                }
                this->clearPendingState();
                this->pendingValidatedThisRun_ = false;
                this->finishNoUpdate(generation, userInitiated);
                return;
            }

            metadata.rollback =
                allowDowngrade && versionComparison.value_or(0) < 0;

            if (this->pendingUpdate_.has_value())
            {
                const bool samePendingRelease =
                    this->pendingUpdate_->releaseId == metadata.id &&
                    this->pendingUpdate_->channel == metadata.channel &&
                    this->pendingUpdate_->velopackChannel ==
                        metadata.velopackChannel &&
                    normalizedVersion(this->pendingUpdate_->packageVersion) ==
                        normalizedVersion(metadata.packageVersion) &&
                    this->pendingUpdate_->feedBaseUrl == metadata.feedBaseUrl &&
                    this->pendingUpdate_->rollback == metadata.rollback;
                if (samePendingRelease && this->hasStagedPackage())
                {
                    this->pendingUpdate_->build = metadata.build;
                    this->pendingUpdate_->summaryMarkdown =
                        metadata.summaryMarkdown;
                    this->pendingUpdate_->rollback = metadata.rollback;
                    this->pendingValidatedThisRun_ = true;
                    this->pendingValidationAge_.restart();
                    this->operationInFlight_ = false;
                    this->progress_ = 100;
                    this->savePendingState();
                    this->setStatus(MoltorinoUpdateStatus::Ready);
                    const bool showDialog =
                        userInitiated || this->showReadyDialogAfterValidation_;
                    this->showReadyDialogAfterValidation_ = false;
                    if (showDialog)
                    {
                        this->showUpdateDialog();
                    }
                    return;
                }
                this->clearPendingState();
                this->pendingValidatedThisRun_ = false;
            }

            this->metadata_ = metadata;
            this->startVelopackUpdate(metadata, userInitiated, generation);
        })
        .onError([this, userInitiated, generation](
                     const NetworkResult &result) {
            this->finishError(
                generation,
                QString("Could not contact the update service: %1")
                    .arg(result.formatError()),
                userInitiated);
        })
        .execute();
}

void MoltorinoUpdater::startVelopackUpdate(const ReleaseMetadata &metadata,
                                            bool userInitiated,
                                            int generation)
{
#ifdef MOLTORINO_VELOPACK_ENABLED
    std::ignore = QtConcurrent::run(
        &this->updatePool_,
        [this, metadata, userInitiated, generation] {
            try
            {
                auto options = updateOptions(metadata.velopackChannel,
                                             metadata.rollback);
                Velopack::UpdateManager manager(metadata.feedBaseUrl.toStdString(),
                                                &options);
                auto update = manager.CheckForUpdates();
                if (!update.has_value())
                {
                    if (isAppAboutToQuit())
                    {
                        return;
                    }
                    postToThread([this, generation, userInitiated] {
                        this->finishNoUpdate(generation, userInitiated);
                    });
                    return;
                }

                const auto &target = update->TargetFullRelease;
                if (QString::fromStdString(target.PackageId) !=
                        velopackPackageIdFor(metadata.channel) ||
                    normalizedVersion(QString::fromStdString(target.Version)) !=
                        normalizedVersion(metadata.packageVersion))
                {
                    throw std::runtime_error(
                        "The published update feed does not match its release metadata.");
                }

                if (!isAppAboutToQuit())
                {
                    postToThread([this, generation] {
                        if (generation != this->operationGeneration_)
                        {
                            return;
                        }
                        this->setStatus(MoltorinoUpdateStatus::Downloading);
                    });
                }

                struct ProgressContext {
                    MoltorinoUpdater *updater;
                    int generation;
                } context{this, generation};

                manager.DownloadUpdates(
                    *update,
                    [](void *raw, size_t progress) {
                        auto *context = static_cast<ProgressContext *>(raw);
                        const auto bounded = static_cast<int>(
                            std::min<size_t>(progress, 100));
                        if (isAppAboutToQuit())
                        {
                            return;
                        }
                        postToThread([updater = context->updater,
                                      generation = context->generation,
                                      bounded] {
                            updater->updateProgress(generation, bounded);
                        });
                    },
                    &context);

                PendingUpdate pending;
                pending.releaseId = metadata.id;
                pending.channel = metadata.channel;
                pending.velopackChannel = metadata.velopackChannel;
                pending.build = metadata.build;
                pending.packageVersion =
                    QString::fromStdString(target.Version);
                pending.summaryMarkdown = metadata.summaryMarkdown;
                pending.feedBaseUrl = metadata.feedBaseUrl;
                pending.rollback = metadata.rollback;

                if (!isAppAboutToQuit())
                {
                    postToThread([this, generation,
                                  pending = std::move(pending)]() mutable {
                        this->finishReady(generation, std::move(pending));
                    });
                }
            }
            catch (const std::exception &error)
            {
                const auto message = QString::fromUtf8(error.what());
                if (!isAppAboutToQuit())
                {
                    postToThread([this, generation, userInitiated, message] {
                        this->finishError(generation, message, userInitiated);
                    });
                }
            }
        });
#else
    Q_UNUSED(metadata)
    Q_UNUSED(userInitiated)
    Q_UNUSED(generation)
#endif
}

void MoltorinoUpdater::finishNoUpdate(int generation, bool userInitiated)
{
    if (generation != this->operationGeneration_)
    {
        return;
    }
    this->operationInFlight_ = false;
    this->retryTimer_.stop();
    this->metadata_.reset();
    this->progress_ = 0;
    this->pendingValidatedThisRun_ = false;
    this->pendingValidationAge_.invalidate();
    this->setStatus(MoltorinoUpdateStatus::UpToDate);
    if (userInitiated)
    {
        this->showUpdateDialog();
    }
}

void MoltorinoUpdater::finishReady(int generation, PendingUpdate update)
{
    if (generation != this->operationGeneration_)
    {
        return;
    }
    this->operationInFlight_ = false;
    this->progress_ = 100;
    this->pendingUpdate_ = std::move(update);
    if (!this->hasStagedPackage())
    {
        this->clearPendingState();
        this->setStatus(
            MoltorinoUpdateStatus::Error,
            "Could not verify the downloaded update.");
        this->retryTimer_.start();
        return;
    }
    this->revalidateDownloadedUpdate();
}

void MoltorinoUpdater::revalidateDownloadedUpdate()
{
    this->pendingValidatedThisRun_ = false;
    this->pendingValidationAge_.invalidate();
    this->showReadyDialogAfterValidation_ = true;
    this->savePendingState();
    this->checkForUpdates(false);
}

void MoltorinoUpdater::finishError(int generation, QString message,
                                    bool userInitiated)
{
    if (generation != this->operationGeneration_)
    {
        return;
    }
    this->operationInFlight_ = false;
    this->progress_ = 0;
    message = message.trimmed();
    if (message.isEmpty())
    {
        message = "The update operation failed.";
    }

    qCWarning(moltorinoUpdater) << message;
    if (!userInitiated && !this->metadata_.has_value() &&
        !this->pendingUpdate_.has_value())
    {
        this->setStatus(MoltorinoUpdateStatus::Idle);
        if (!this->retryTimer_.isActive())
        {
            this->retryTimer_.start();
        }
        return;
    }

    this->setStatus(MoltorinoUpdateStatus::Error, message);
    if (!userInitiated && !this->retryTimer_.isActive())
    {
        this->retryTimer_.start();
    }
    if (userInitiated)
    {
        this->showUpdateDialog();
    }
}

void MoltorinoUpdater::updateProgress(int generation, int progress)
{
    if (generation != this->operationGeneration_ ||
        progress == this->progress_)
    {
        return;
    }
    this->progress_ = progress;
    this->stateChanged.invoke();
}

bool MoltorinoUpdater::scheduleApply(bool restart, QString &error)
{
    if (this->applyScheduled_)
    {
        return true;
    }
    if (!this->canRestartToUpdate())
    {
        error = "The downloaded update needs a fresh release validation.";
        return false;
    }

#ifdef MOLTORINO_VELOPACK_ENABLED
    try
    {
        auto options = updateOptions(this->pendingUpdate_->velopackChannel,
                                     this->pendingUpdate_->rollback);
        Velopack::UpdateManager manager(
            this->pendingUpdate_->feedBaseUrl.toStdString(), &options);
        const auto asset = manager.UpdatePendingRestart();
        if (!asset.has_value() ||
            QString::fromStdString(asset->PackageId) !=
                velopackPackageIdFor(this->pendingUpdate_->channel) ||
            normalizedVersion(QString::fromStdString(asset->Version)) !=
                normalizedVersion(this->pendingUpdate_->packageVersion))
        {
            error = "The staged update package is missing or no longer matches. Check for updates again.";
            return false;
        }

        std::vector<std::string> restartArgs;
        if (restart)
        {
            const auto qtArgs = restartArguments();
            restartArgs.reserve(static_cast<size_t>(qtArgs.size()));
            for (const auto &arg : qtArgs)
            {
                restartArgs.emplace_back(arg.toStdString());
            }
        }
        manager.WaitExitThenApplyUpdates(*asset, true, restart, restartArgs);
        this->applyScheduled_ = true;
        this->setStatus(MoltorinoUpdateStatus::Applying);
        return true;
    }
    catch (const std::exception &exception)
    {
        error = QString::fromUtf8(exception.what()).trimmed();
        if (error.isEmpty())
        {
            error = "Could not start the update helper.";
        }
        return false;
    }
#else
    Q_UNUSED(restart)
    error = "Automatic updates are not available in this build.";
    return false;
#endif
}

void MoltorinoUpdater::restartToUpdate()
{
    if (!this->canRestartToUpdate())
    {
        this->checkForUpdates(true);
        return;
    }
    this->restartRequested_ = true;
    this->forceApplicationQuit();
}

bool MoltorinoUpdater::restartApplication(
    const QProcessEnvironment &environment)
{
    const bool hasExplicitEnvironment = !environment.isEmpty();
    if (this->canRestartToUpdate() && !hasExplicitEnvironment)
    {
        this->restartToUpdate();
        return this->restartRequested_;
    }

    const auto program = QCoreApplication::applicationFilePath();
    if (program.isEmpty())
    {
        this->setStatus(MoltorinoUpdateStatus::Error,
                        "Moltorino could not resolve its executable path.");
        this->showUpdateDialog();
        return false;
    }

    if (hasExplicitEnvironment)
    {
        this->restartEnvironment_ = environment;
        this->deferUpdateForExplicitRestart_ = true;
    }
    this->restartRequested_ = true;
    this->forceApplicationQuit();
    return true;
}

void MoltorinoUpdater::prepareForApplicationQuit()
{
    if (this->quitCoordinated_)
    {
        return;
    }
    this->quitCoordinated_ = true;
    this->pollTimer_.stop();
    this->retryTimer_.stop();
    this->readyValidationTimer_.stop();
    ++this->operationGeneration_;
    this->operationInFlight_ = false;

    if (this->applyScheduled_)
    {
        return;
    }

    if (this->canRestartToUpdate() &&
        !this->deferUpdateForExplicitRestart_)
    {
        QString error;
        if (this->scheduleApply(this->restartRequested_, error))
        {
            return;
        }
        qCWarning(moltorinoUpdater)
            << "Could not schedule the ready update during shutdown:" << error;
    }

    if (!this->restartRequested_)
    {
        return;
    }

    const auto program = QCoreApplication::applicationFilePath();
    const auto workingDirectory = QCoreApplication::applicationDirPath();
    QProcess replacement;
    replacement.setProgram(program);
    replacement.setArguments(restartArguments());
    replacement.setWorkingDirectory(workingDirectory);
    if (this->restartEnvironment_.has_value())
    {
        replacement.setProcessEnvironment(*this->restartEnvironment_);
    }
    if (program.isEmpty() || !replacement.startDetached())
    {
        qCWarning(moltorinoUpdater)
            << "Could not launch the replacement Moltorino process.";
    }
}

void MoltorinoUpdater::forceApplicationQuit()
{
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        requestApplicationQuit();
    });
}

QString MoltorinoUpdater::stateFilePath() const
{
    if (auto *app = tryGetApp())
    {
        return app->getPaths().miscDirectory +
               QString("/moltorino-updater-v2-%1-state.json")
                   .arg(this->channel_);
    }
    return {};
}

void MoltorinoUpdater::loadPendingState()
{
    QFile file(this->stateFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    if (file.size() <= 0 || file.size() > 1024 * 1024)
    {
        qCWarning(moltorinoUpdater)
            << "Ignoring updater state with an invalid size:" << file.size();
        file.close();
        QFile::remove(file.fileName());
        return;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        qCWarning(moltorinoUpdater)
            << "Ignoring invalid persisted updater state:" << parseError.errorString();
        file.close();
        QFile::remove(file.fileName());
        return;
    }

    const auto object = document.object();
    PendingUpdate pending;
    pending.releaseId = object.value("releaseId").toString();
    pending.channel = object.value("channel").toString();
    pending.velopackChannel =
        object.value("velopackChannel").toString().trimmed();
    pending.build = object.value("build").toString();
    pending.packageVersion = object.value("packageVersion").toString();
    pending.summaryMarkdown = object.value("summaryMarkdown").toString();
    pending.feedBaseUrl = object.value("feedBaseUrl").toString();
    pending.rollback = object.value("rollback").toBool(false);
    if (pending.velopackChannel.isEmpty())
    {
        pending.velopackChannel = velopackChannelFor(pending.channel);
    }
    if (!object.value("preview").toBool(false) &&
        object.value("previewAuthorizationToken").toString().isEmpty() &&
        object.value("previewRollbackToken").toString().isEmpty() &&
        !pending.releaseId.isEmpty() &&
        !pending.build.isEmpty() &&
        !pending.packageVersion.isEmpty() &&
        pending.channel == this->channel_ &&
        pending.velopackChannel == velopackChannelFor(pending.channel) &&
        isTrustedAutomaticFeed(pending.feedBaseUrl))
    {
        this->pendingUpdate_ = std::move(pending);
        return;
    }
    file.close();
    QFile::remove(file.fileName());
}

void MoltorinoUpdater::savePendingState() const
{
    if (!this->pendingUpdate_.has_value())
    {
        return;
    }
    const auto path = this->stateFilePath();
    if (path.isEmpty())
    {
        return;
    }

    const auto &pending = *this->pendingUpdate_;
    QJsonObject object{
        {"releaseId", pending.releaseId},
        {"channel", pending.channel},
        {"velopackChannel", pending.velopackChannel},
        {"build", pending.build},
        {"packageVersion", pending.packageVersion},
        {"summaryMarkdown", pending.summaryMarkdown},
        {"feedBaseUrl", pending.feedBaseUrl},
        {"rollback", pending.rollback},
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 ||
        !file.commit())
    {
        qCWarning(moltorinoUpdater)
            << "Could not persist updater state to" << path;
    }
}

void MoltorinoUpdater::clearPendingState()
{
    this->pendingUpdate_.reset();
    this->pendingValidatedThisRun_ = false;
    this->showReadyDialogAfterValidation_ = false;
    this->pendingValidationAge_.invalidate();
    const auto path = this->stateFilePath();
    if (!path.isEmpty())
    {
        QFile::remove(path);
    }
}

void MoltorinoUpdater::cleanupLegacyUpdaterArtifacts() const
{
#ifdef Q_OS_WIN
    QStringList directories{
        getApp()->getPaths().miscDirectory + "/Updates",
    };
    const auto tempRoot =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (!tempRoot.isEmpty())
    {
        directories.append(QDir(tempRoot).filePath("Moltorino/Updates"));
    }

    for (const auto &directoryPath : directories)
    {
        QDir directory(directoryPath);
        const auto files = directory.entryInfoList(
            {"Moltorino Updater*.exe", "Moltorino7-update-install*.log"},
            QDir::Files);
        for (const auto &file : files)
        {
            QFile::remove(file.absoluteFilePath());
        }
    }
#endif
}

QString MoltorinoUpdater::installedPackageVersion() const
{
#ifdef MOLTORINO_VELOPACK_ENABLED
    try
    {
        const auto feed =
            QString::fromLatin1(UPDATE_API_BASE) + updaterFeedPath();
        auto options = updateOptions(velopackChannelFor(this->channel_));
        Velopack::UpdateManager manager(feed.toStdString(), &options);
#ifdef Q_OS_WIN
        if (manager.IsPortable())
        {
            return {};
        }
#endif
        auto installedPackageId = QString::fromStdString(manager.GetAppId());
        installedPackageId.remove(QChar::Null);
        if (installedPackageId != velopackPackageIdFor(this->channel_))
        {
            qCWarning(moltorinoUpdater)
                << "Installed Velopack package ID does not match channel"
                << this->channel_;
            return {};
        }
        return normalizedVersion(
            QString::fromStdString(manager.GetCurrentVersion()));
    }
    catch (const std::exception &ex)
    {
        qCWarning(moltorinoUpdater)
            << "Could not inspect the installed Velopack package:"
            << ex.what();
        return {};
    }
#else
    return {};
#endif
}

std::optional<bool> MoltorinoUpdater::shouldAcceptPackageVersion(
    const QString &targetVersion, const QString &currentVersion, bool rollback)
{
    const auto comparison =
        comparePackageVersions(targetVersion, currentVersion);
    if (!comparison.has_value())
    {
        return std::nullopt;
    }
    if (*comparison > 0)
    {
        return true;
    }
    if (*comparison < 0)
    {
        return rollback;
    }
    return false;
}

bool MoltorinoUpdater::hasStagedPackage() const
{
#ifdef MOLTORINO_VELOPACK_ENABLED
    if (!this->pendingUpdate_.has_value())
    {
        return false;
    }
    try
    {
        auto options = updateOptions(this->pendingUpdate_->velopackChannel,
                                     this->pendingUpdate_->rollback);
        Velopack::UpdateManager manager(
            this->pendingUpdate_->feedBaseUrl.toStdString(), &options);
        const auto asset = manager.UpdatePendingRestart();
        return asset.has_value() &&
               QString::fromStdString(asset->PackageId) ==
                   velopackPackageIdFor(this->pendingUpdate_->channel) &&
               normalizedVersion(QString::fromStdString(asset->Version)) ==
                   normalizedVersion(this->pendingUpdate_->packageVersion);
    }
    catch (const std::exception &)
    {
        return false;
    }
#else
    return false;
#endif
}

void MoltorinoUpdater::showUpdateDialog()
{
    if (this->updateDialog_)
    {
        this->updateDialog_->raise();
        this->updateDialog_->activateWindow();
        return;
    }

    auto *dialog = new UpdateDialog();
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        this->updateDialog_.clear();
    });
    this->updateDialog_ = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MoltorinoUpdater::showFullChangelog()
{
    const auto build = this->targetBuild().isEmpty()
                           ? Version::instance().internalVersion()
                           : this->targetBuild();
    QMessageBox::information(
        QApplication::activeWindow(), "Moltorino Changelog",
        QString("No changelog is available for %1.")
            .arg(build.toHtmlEscaped()));
}

void MoltorinoUpdater::setStatus(MoltorinoUpdateStatus status,
                                  const QString &error)
{
    const bool changed = this->status_ != status ||
                         this->errorMessage_ != error;
    this->status_ = status;
    this->errorMessage_ = error;
    if (status == MoltorinoUpdateStatus::Ready &&
        this->pendingUpdate_.has_value())
    {
        if (!this->readyValidationTimer_.isActive())
        {
            this->readyValidationTimer_.start();
        }
    }
    else
    {
        this->readyValidationTimer_.stop();
    }
    if (changed)
    {
        this->stateChanged.invoke();
    }
}

MoltorinoUpdateStatus MoltorinoUpdater::status() const
{
    return this->status_;
}

QString MoltorinoUpdater::statusText() const
{
    switch (this->status_)
    {
        case MoltorinoUpdateStatus::Disabled:
            return "Automatic updates are unavailable in this build.";
        case MoltorinoUpdateStatus::Idle:
            return "Moltorino checks for updates quietly in the background.";
        case MoltorinoUpdateStatus::Checking:
            return "Checking for a Moltorino update...";
        case MoltorinoUpdateStatus::Downloading:
            return QString("Downloading the %1... %2%")
                .arg(this->isRollback() ? "rollback" : "update")
                .arg(this->progress_);
        case MoltorinoUpdateStatus::Ready:
            return this->isRollback()
                       ? "The rollback is downloaded and will install when Moltorino closes."
                       : "The update is downloaded and will install when Moltorino closes.";
        case MoltorinoUpdateStatus::Applying:
            return this->isRollback()
                       ? "Moltorino will finish rolling back after it closes."
                       : "Moltorino will finish updating after it closes.";
        case MoltorinoUpdateStatus::UpToDate:
            return "Moltorino is up to date.";
        case MoltorinoUpdateStatus::Error:
            return this->errorMessage_.isEmpty()
                       ? "The update could not be prepared."
                       : this->errorMessage_;
    }
    return {};
}

QString MoltorinoUpdater::channel() const
{
    return this->channel_;
}

QString MoltorinoUpdater::platform() const
{
#if defined(Q_OS_WIN)
    return "windows-velopack-x64";
#elif defined(Q_OS_MACOS)
#    ifdef MOLTORINO_VELOPACK_ENABLED
    return "macos-velopack-universal";
#    else
    return "macos-dmg-universal-30";
#    endif
#else
#    ifdef MOLTORINO_VELOPACK_ENABLED
    return "linux-velopack-x64";
#    else
    return "linux-appimage-x64-30";
#    endif
#endif
}

int MoltorinoUpdater::protocol() const
{
#if defined(MOLTORINO_VELOPACK_ENABLED) || defined(Q_OS_WIN)
    return 2;
#else
    return 1;
#endif
}

QString MoltorinoUpdater::targetBuild() const
{
    if (this->pendingUpdate_.has_value())
    {
        return this->pendingUpdate_->build;
    }
    return this->metadata_.has_value() ? this->metadata_->build : QString{};
}

QString MoltorinoUpdater::targetPackageVersion() const
{
    if (this->pendingUpdate_.has_value())
    {
        return this->pendingUpdate_->packageVersion;
    }
    return this->metadata_.has_value() ? this->metadata_->packageVersion
                                       : QString{};
}

QString MoltorinoUpdater::summaryMarkdown() const
{
    if (this->pendingUpdate_.has_value())
    {
        return this->pendingUpdate_->summaryMarkdown;
    }
    return this->metadata_.has_value() ? this->metadata_->summaryMarkdown
                                       : QString{};
}

QString MoltorinoUpdater::errorMessage() const
{
    return this->errorMessage_;
}

int MoltorinoUpdater::progress() const
{
    return this->progress_;
}

bool MoltorinoUpdater::isRollback() const
{
    if (this->pendingUpdate_.has_value())
    {
        return this->pendingUpdate_->rollback;
    }
    return this->metadata_.has_value() && this->metadata_->rollback;
}

bool MoltorinoUpdater::isAvailable() const
{
    return this->metadata_.has_value() || this->pendingUpdate_.has_value();
}

bool MoltorinoUpdater::isBusy() const
{
    return this->status_ == MoltorinoUpdateStatus::Checking ||
           this->status_ == MoltorinoUpdateStatus::Downloading ||
           this->status_ == MoltorinoUpdateStatus::Applying;
}

bool MoltorinoUpdater::isError() const
{
    return this->status_ == MoltorinoUpdateStatus::Error;
}

bool MoltorinoUpdater::canRestartToUpdate() const
{
#ifdef MOLTORINO_VELOPACK_ENABLED
    return this->status_ == MoltorinoUpdateStatus::Ready &&
           this->pendingUpdate_.has_value() &&
           this->pendingValidatedThisRun_ &&
           this->pendingValidationAge_.isValid() &&
           this->pendingValidationAge_.elapsed() <=
               READY_VALIDATION_LEASE_MS;
#else
    return false;
#endif
}

bool MoltorinoUpdater::shouldShowUpdateButton() const
{
    return this->isAvailable() || this->isError();
}

MoltorinoUpdater *getMoltorinoUpdater()
{
    return &MoltorinoUpdater::instance();
}

}
