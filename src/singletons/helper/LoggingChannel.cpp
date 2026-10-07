// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/helper/LoggingChannel.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "messages/Message.hpp"
#include "messages/MessageThread.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"

#include <QDateTime>
#include <QDir>

namespace {

const QByteArray ENDLINE("\n");

void appendLine(QFile &fileHandle, const QString &line)
{
    assert(fileHandle.isOpen());
    assert(fileHandle.isWritable());

    fileHandle.write(line.toUtf8());
    fileHandle.flush();
}

QString generateOpeningString(
    const QDateTime &now = QDateTime::currentDateTime())
{
    QString ret("# Start logging at ");

    ret.append(now.toString("yyyy-MM-dd HH:mm:ss "));
    ret.append(now.timeZoneAbbreviation());
    ret.append(ENDLINE);

    return ret;
}

QString generateClosingString(
    const QDateTime &now = QDateTime::currentDateTime())
{
    QString ret("# Stop logging at ");

    ret.append(now.toString("yyyy-MM-dd HH:mm:ss "));
    ret.append(now.timeZoneAbbreviation());
    ret.append(ENDLINE);

    return ret;
}

QString filesystemSafeComponent(const QString &value)
{
    QString result;
    result.reserve(value.size());

    static const auto invalidCharacters = QStringLiteral("<>:\"/\\|?*%");
    for (qsizetype index = 0; index < value.size(); ++index)
    {
        const auto character = value.at(index);
        const bool invalid = character.unicode() < 0x20 ||
                             invalidCharacters.contains(character) ||
                             (index == value.size() - 1 &&
                              (character == u'.' || character == u' '));
        if (invalid)
        {
            result += u'%';
            result += QString::number(character.unicode(), 16)
                          .rightJustified(4, u'0')
                          .toUpper();
        }
        else
        {
            result += character;
        }
    }

    if (result.isEmpty())
    {
        return QStringLiteral("_");
    }

    const auto deviceName = result.section(u'.', 0, 0).toUpper();
    if (deviceName == QStringLiteral("CON") ||
        deviceName == QStringLiteral("PRN") ||
        deviceName == QStringLiteral("AUX") ||
        deviceName == QStringLiteral("NUL") ||
        (deviceName.size() == 4 &&
         (deviceName.startsWith(QStringLiteral("COM")) ||
          deviceName.startsWith(QStringLiteral("LPT"))) &&
         deviceName.at(3) >= u'1' && deviceName.at(3) <= u'9'))
    {
        result.prepend(u'_');
    }

    return result;
}

QString generateDateString(const QDateTime &now)
{
    return now.toString("yyyy-MM-dd");
}

}

namespace chatterino {

LoggingChannel::LoggingChannel(QString _channelName, QString _platform)
    : channelName(std::move(_channelName))
    , fileSystemName(filesystemSafeComponent(this->channelName))
    , platform(std::move(_platform))
{
    if (this->channelName.startsWith("/whispers"))
    {
        this->subDirectory = "Whispers";
    }
    else if (this->channelName.startsWith("/mentions"))
    {
        this->subDirectory = "Mentions";
    }
    else if (this->channelName.startsWith("/live"))
    {
        this->subDirectory = "Live";
    }
    else if (this->channelName.startsWith("/automod"))
    {
        this->subDirectory = "AutoMod";
    }
    else
    {
        this->subDirectory = QStringLiteral("Channels") + QDir::separator() +
                             this->fileSystemName;
    }

    this->subDirectory = this->platform[0].toUpper() +
                         this->platform.mid(1).toLower() + QDir::separator() +
                         this->subDirectory;

    getSettings()->logPath.connect(
        [this](const QString &logPath, auto) {
            this->baseDirectory =
                logPath.isEmpty() ? getApp()->getPaths().messageLogDirectory
                                  : logPath;
            this->openLogFile();
            this->currentStreamFileHandle.close();
            this->currentStreamID.clear();
        },
        this->settingConnections_);
}

LoggingChannel::~LoggingChannel()
{
    if (this->fileHandle.isOpen())
    {
        appendLine(this->fileHandle, generateClosingString());
    }
    this->fileHandle.close();
    this->currentStreamFileHandle.close();
}

void LoggingChannel::openLogFile()
{
    QDateTime now = QDateTime::currentDateTime();
    this->dateString = generateDateString(now);

    if (this->fileHandle.isOpen())
    {
        this->fileHandle.flush();
        this->fileHandle.close();
    }

    QString baseFileName =
        this->fileSystemName + "-" + this->dateString + ".log";

    QString directory =
        this->baseDirectory + QDir::separator() + this->subDirectory;

    if (!QDir().mkpath(directory))
    {
        qCDebug(chatterinoHelper) << "Unable to create logging path";
        return;
    }

    QString fileName = directory + QDir::separator() + baseFileName;
    qCDebug(chatterinoHelper) << "Logging to" << fileName;
    this->fileHandle.setFileName(fileName);

    if (!this->fileHandle.open(QIODevice::Append))
    {
        qCDebug(chatterinoHelper)
            << "Failed to open file" << this->fileHandle.errorString();
        return;
    }

    appendLine(this->fileHandle, generateOpeningString(now));
}

void LoggingChannel::openStreamLogFile(const QString &streamID)
{
    QDateTime now = QDateTime::currentDateTime();
    this->currentStreamID = streamID;

    if (this->currentStreamFileHandle.isOpen())
    {
        this->currentStreamFileHandle.flush();
        this->currentStreamFileHandle.close();
    }

    QString baseFileName =
        this->fileSystemName + "-" + filesystemSafeComponent(streamID) + ".log";

    QString directory =
        this->baseDirectory + QDir::separator() + this->subDirectory;

    if (!QDir().mkpath(directory))
    {
        qCDebug(chatterinoHelper) << "Unable to create logging path";
        return;
    }

    QString fileName = directory + QDir::separator() + baseFileName;
    qCDebug(chatterinoHelper) << "Logging stream to" << fileName;
    this->currentStreamFileHandle.setFileName(fileName);

    if (!this->currentStreamFileHandle.open(QIODevice::Append))
    {
        qCDebug(chatterinoHelper)
            << "Failed to open file"
            << this->currentStreamFileHandle.errorString();
        return;
    }
    appendLine(this->currentStreamFileHandle, generateOpeningString(now));
}

void LoggingChannel::addMessage(const MessagePtr &message,
                                const QString &streamID)
{
    QDateTime messageTimestamp;
    if (getSettings()->tryUseTwitchTimestamps &&
        !message->serverReceivedTime.isNull())
    {
        messageTimestamp = message->serverReceivedTime;
    }
    else
    {
        messageTimestamp = QDateTime::currentDateTime();
    }

    QString messageDateString = generateDateString(messageTimestamp);
    if (messageDateString != this->dateString)
    {
        this->dateString = messageDateString;
        this->openLogFile();
    }

    QString str;
    if (this->channelName.startsWith("/mentions") ||
        this->channelName.startsWith("/automod"))
    {
        str.append("#" + message->channelName + " ");
    }

    QString logTimestampFormat = getSettings()->logTimestampFormat;
    if (logTimestampFormat != "Disable")
    {
        str.append('[');
        str.append(messageTimestamp.toString(logTimestampFormat));
        str.append("] ");
    }

    QString messageText;
    if (message->loginName.isEmpty())
    {

        messageText = message->messageText;
    }
    else
    {
        if (message->localizedName.isEmpty())
        {
            messageText = message->loginName + ": " + message->messageText;
        }
        else
        {
            messageText = message->localizedName + " " + message->loginName +
                          ": " + message->messageText;
        }
    }

    if ((message->flags.has(MessageFlag::ReplyMessage) &&
         getSettings()->stripReplyMention) &&
        !getSettings()->hideReplyContext)
    {
        qsizetype colonIndex = messageText.indexOf(':');
        if (colonIndex != -1)
        {
            QString rootMessageChatter;
            if (message->replyParent)
            {
                rootMessageChatter = message->replyParent->loginName;
            }
            else if (message->replyThread)
            {

                rootMessageChatter = message->replyThread->root()->loginName;
            }
            if (!rootMessageChatter.isEmpty())
            {
                messageText.insert(colonIndex + 1, " @" + rootMessageChatter);
            }
        }
    }
    str.append(messageText);
    str.append(ENDLINE);

    if (this->fileHandle.isOpen())
    {
        appendLine(this->fileHandle, str);
    }

    if (!streamID.isEmpty() && getSettings()->separatelyStoreStreamLogs)
    {
        if (this->currentStreamID != streamID)
        {
            this->openStreamLogFile(streamID);
        }

        if (this->currentStreamFileHandle.isOpen())
        {
            appendLine(this->currentStreamFileHandle, str);
        }
    }
}

}
