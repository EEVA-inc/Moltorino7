// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "BrowserExtension.hpp"
#include "common/Args.hpp"
#include "common/Env.hpp"
#include "common/Modes.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/IvrApi.hpp"
#include "providers/NetworkConfigurationProvider.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "RunGui.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/FileLogger.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "util/AttachToConsole.hpp"
#include "util/IpcQueue.hpp"
#include "util/SettingsTransfer.hpp"

#ifdef Q_OS_MACOS
#    include "util/MacOsHelpers.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>
#include <QSslSocket>
#include <QStringList>
#include <QSysInfo>
#include <QtCore/QtPlugin>
#ifdef Q_OS_WIN
#    include <shobjidl_core.h>
#endif

#ifdef MOLTORINO_VELOPACK_ENABLED
#    include <Velopack.hpp>
#endif

#include <exception>
#include <iostream>
#include <memory>

#ifdef CHATTERINO_WITH_AVIF_PLUGIN
Q_IMPORT_PLUGIN(QAVIFPlugin)
#endif

using namespace chatterino;

int main(int argc, char **argv)
{
#ifdef MOLTORINO_VELOPACK_ENABLED
    try
    {
        Velopack::VelopackApp::Build().SetAutoApplyOnStartup(false).Run();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Velopack startup failed: " << error.what() << '\n';
    }
#endif

    bool releaseIdentityRequested = false;
    for (int i = 1; i < argc; ++i)
    {
        if (QString::fromLocal8Bit(argv[i]) ==
            QStringLiteral("--moltorino-release-identity"))
        {
            releaseIdentityRequested = true;
            break;
        }
    }
    if (releaseIdentityRequested)
    {
        QCoreApplication identityApp(argc, argv);
        const auto &version = Version::instance();
        QJsonObject identity{
            {"schemaVersion", 1},
            {"publicVersion", version.version()},
            {"buildId", version.internalVersion()},
            {"channel", version.updateChannel()},
#if defined(Q_OS_WIN)
            {"platform", "windows"},
            {"appUserModelId", QString::fromStdWString(version.appUserModelID())},
#elif defined(Q_OS_MACOS)
            {"platform", "macos"},
#elif defined(Q_OS_LINUX)
            {"platform", "linux"},
#else
            {"platform", QSysInfo::productType()},
#endif
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
            {"architecture", "win-x64"},
#elif defined(Q_OS_MACOS)
            {"architecture", "osx-universal"},
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
            {"architecture", "linux-x64"},
#else
            {"architecture", QSysInfo::buildCpuArchitecture()},
#endif
            {"sourceCommit", version.fullCommit()},
            {"sourceDirty", version.isModified()},
            {"qtVersion", QString::fromLatin1(qVersion())},
        };
        QSaveFile output(QCoreApplication::applicationDirPath() +
                         "/.moltorino-release-identity-probe.json");
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(
                QJsonDocument(identity).toJson(QJsonDocument::Compact)) < 0 ||
            !output.commit())
        {
            return 1;
        }
        return 0;
    }

#ifdef Q_OS_LINUX

    const auto platformPreference = qgetenv("QT_QPA_PLATFORM");
    if ((platformPreference.isEmpty() || platformPreference == "wayland;xcb") &&
        !qEnvironmentVariableIsEmpty("DISPLAY"))
    {
        qputenv("QT_QPA_PLATFORM", "xcb;wayland");
    }
#endif

    QApplication a(argc, argv);

    QCoreApplication::setApplicationName("chatterino");
    QCoreApplication::setApplicationVersion(Version::instance().version());
    QCoreApplication::setOrganizationDomain("chatterino.com");
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)

    QGuiApplication::setDesktopFileName("com.moltobenne.moltorino");
#endif
#ifdef Q_OS_WIN
    SetCurrentProcessExplicitAppUserModelID(
        Version::instance().appUserModelID().c_str());
#endif

    const Modes modes;
    std::unique_ptr<Paths> paths;

    FileLogger logger;

    try
    {
        paths = std::make_unique<Paths>(modes);
    }
    catch (std::runtime_error &error)
    {
        QMessageBox box;
        if (modes.isPortable)
        {
            auto errorMessage =
                error.what() +
                QStringLiteral(
                    "\n\nInfo: Portable mode requires the application to "
                    "be in a writeable location. If you don't want "
                    "portable mode reinstall the application. "
                    "https://chatterino.com.");
            std::cerr << errorMessage.toLocal8Bit().constData() << '\n';
            std::cerr.flush();
            box.setText(errorMessage);
        }
        else
        {
            box.setText(error.what());
        }
        box.exec();
        return 1;
    }
    ipc::initPaths(paths.get());

    const Args args(a, *paths);

#ifdef CHATTERINO_WITH_CRASHPAD
    const auto crashpadHandler = installCrashHandler(args, *paths);
#endif

    if (args.shouldRunBrowserExtensionHost)
    {
#ifdef Q_OS_MACOS
        ::chatterinoSetMacOsActivationPolicyProhibited();
#endif
        runBrowserExtensionHost();
    }
    else if (args.printVersion)
    {
        attachToConsole();

        auto version = Version::instance();
        auto versionMessage =
            QString("%1 (commit %2%3)")
                .arg(version.fullVersion())
                .arg(version.commitHash())
                .arg(version.isNightly() ? ", " + version.dateOfBuild() : "");
        std::cout << versionMessage.toLocal8Bit().constData() << '\n';
        std::cout.flush();
    }
    else
    {
        if (args.verbose)
        {
            attachToConsole();
        }

        qCInfo(chatterinoApp).noquote()
            << "Chatterino Qt SSL library build version:"
            << QSslSocket::sslLibraryBuildVersionString();
        qCInfo(chatterinoApp).noquote()
            << "Chatterino Qt SSL library version:"
            << QSslSocket::sslLibraryVersionString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
        qCInfo(chatterinoApp).noquote()
            << "Chatterino Qt SSL active backend:"
            << QSslSocket::activeBackend() << "of"
            << QSslSocket::availableBackends().join(", ");
#    if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        qCInfo(chatterinoApp) << "Chatterino Qt SSL active backend features:"
                              << QSslSocket::supportedFeatures();
#    endif
        qCInfo(chatterinoApp) << "Chatterino Qt SSL active backend protocols:"
                              << QSslSocket::supportedProtocols();
#endif

        QString pendingRestoreError;
        if (!settingsbackup::applyPendingRestore(paths->settingsDirectory,
                                                 &pendingRestoreError))
        {
            qCCritical(chatterinoSettings)
                << "Could not apply pending settings restore:"
                << pendingRestoreError;
            QMessageBox::warning(
                nullptr, QStringLiteral("Settings restore failed"),
                QStringLiteral("Moltorino could not finish the pending "
                               "restore safely.\n\n%1")
                    .arg(pendingRestoreError));
        }

        Settings settings(modes, args, paths->settingsDirectory);
#ifndef Q_OS_MACOS
        if (!args.remoteRestart && !args.isFramelessEmbed &&
            !args.migrationReadyFile.has_value() &&
            settings.trayHideOnClose.getValue() &&
            activateExistingGuiInstance(*paths))
        {
            return 0;
        }
#endif

        Updates updates(*paths, settings);

        NetworkConfigurationProvider::applyFromEnv(Env::get());

        IvrApi::initialize();
        Helix::initialize();

        runGui(a, modes, *paths, settings, args, updates);
    }
    return 0;
}
