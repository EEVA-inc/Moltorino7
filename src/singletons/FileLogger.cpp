#include "singletons/FileLogger.hpp"

#include "common/DiagnosticPrivacy.hpp"
#include "common/Env.hpp"

#include <QFileInfo>

#include <cassert>
#include <iostream>

namespace {

bool isPresenceLog(const QMessageLogContext &, const QString &msg)
{
    const auto host = chatterino::diagnostics::presenceHost();
    static const auto badgeHost =
        QUrl(qEnvironmentVariable("MOLTORINO_BADGE_SOCKET_URL")).host();
    return (!host.isEmpty() && msg.contains(host, Qt::CaseInsensitive)) ||
           (!badgeHost.isEmpty() &&
            msg.contains(badgeHost, Qt::CaseInsensitive));
}

void logMessage(QtMsgType type, const QMessageLogContext &context,
                const QString &msg)
{
    chatterino::FileLogger::instance().log(type, context, msg);
}

}

namespace chatterino {

FileLogger *FileLogger::INSTANCE = nullptr;

FileLogger::FileLogger()
{
    assert(FileLogger::INSTANCE == nullptr);

    FileLogger::INSTANCE = this;

    this->originalHandler = qInstallMessageHandler(logMessage);

    const auto &env = Env::get();
    if (!env.logToFile.isEmpty())
    {
        auto result = this->enable(env.logToFile);
        if (!result.has_value())
        {
            auto error = result.error();
            QString errorMessage = QString("Unable to open log file %1. Error "
                                           "reported by the system was: %2")
                                       .arg(error.absFilePath, error.errorDesc);

            std::cerr << errorMessage.toLocal8Bit().constData() << '\n'
                      << std::flush;
        }
    }
}

FileLogger::~FileLogger()
{
    qInstallMessageHandler(this->originalHandler);
    this->disable();
    FileLogger::INSTANCE = nullptr;
}

void FileLogger::disable()
{
    std::scoped_lock lk(this->logLock);

    this->logFile = nullptr;
}

Expected<void, FileLogger::Error> FileLogger::enable(const QString &filePath)
{
    std::scoped_lock lk(this->logLock);

    QFileInfo finfo(filePath);
    QString absFilePath = finfo.absoluteFilePath();

    if (this->logFile != nullptr)
    {
        QFileInfo finfoCurrent(*this->logFile);

        if (finfoCurrent.absoluteFilePath() == absFilePath)
        {
            return {};
        }
    }

    auto f = std::make_unique<QFile>(absFilePath);
    bool success = f->open(QIODevice::WriteOnly);
    if (!success)
    {
        Error error{};
        error.absFilePath = absFilePath;
        error.errorDesc = f->errorString();

        return makeUnexpected(std::move(error));
    }

    this->logFile = std::move(f);

    return {};
}

FileLogger &FileLogger::instance()
{
    assert(INSTANCE != nullptr &&
           "Attempted to get instance of FileLogger prior to initializing it");

    return *INSTANCE;
}

void FileLogger::log(QtMsgType type, const QMessageLogContext &context,
                     const QString &msg)
{
    if (isPresenceLog(context, msg))
    {
        return;
    }
    {
        std::scoped_lock lk(this->logLock);
        if (this->logFile)
        {
            auto formatted = qFormatLogMessage(type, context, msg);
            this->logFile->write((formatted + "\n").toUtf8());
            this->logFile->flush();
        }
    }
    if (this->originalHandler)
    {
        this->originalHandler(type, context, msg);
    }
}

}
