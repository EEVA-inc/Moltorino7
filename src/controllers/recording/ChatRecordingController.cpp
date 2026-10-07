#include "controllers/recording/ChatRecordingController.hpp"

#include "Application.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/recording/ChatRecordingMessage.hpp"
#include "controllers/recording/ChatRecordingWriter.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Paths.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/dialogs/EditHotkeyDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <map>
#include <vector>

namespace chatterino {
namespace {
class RecordingSettings : public QWidget
{
public:
    using QWidget::QWidget;
    pajlada::Signals::SignalHolder connections;
};

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::vector<ChannelPtr> sources(Split *pane)
{
    std::vector<ChannelPtr> result;
    if (!pane)
    {
        return result;
    }
    const auto channel = pane->getChannel();
    if (const auto *multi = dynamic_cast<const MultiChannel *>(channel.get()))
    {
        for (const auto &child : multi->channels())
        {
            if (recording::isSupportedSource(*child.channel))
            {
                result.push_back(child.channel);
            }
        }
    }
    else if (channel && recording::isSupportedSource(*channel))
    {
        result.push_back(channel);
    }
    return result;
}

QJsonArray sourceMetadata(Split *pane)
{
    QJsonArray result;
    for (const auto &source : sources(pane))
    {
        result.append(recording::describeSource(*source));
    }
    return result;
}
}

SplitContainer *recordingTabFor(Split *pane)
{
    return pane ? dynamic_cast<SplitContainer *>(pane->parentWidget())
                : nullptr;
}

struct ChatRecordingController::Impl {
    struct Group {
        QString id;
        QPointer<SplitContainer> tab;
        QJsonObject options;
        QMetaObject::Connection tabDestroyed;
        int nextPane = 1;
        bool preparing = true;
        bool stopping = false;
        bool failed = false;

        ~Group()
        {
            QObject::disconnect(tabDestroyed);
        }
    };
    struct File {
        QString id;
        QString group;
        QPointer<Split> pane;
        QJsonObject metadata;
        QElapsedTimer timer;
        QMetaObject::Connection paneDestroyed;
        bool accepting = false;
        bool stopping = false;
        bool prepared = false;
        qint64 acceptedMessages = 0;
        QString status = "Starting";
        QJsonArray sourceSnapshot;
        std::vector<ChannelPtr> watchedSources;
        std::map<const Channel *, bool> connected;
        std::map<const Channel *, bool> live;
        pajlada::Signals::SignalHolder sourceConnections;
        std::shared_ptr<std::atomic<qint64>> queued =
            std::make_shared<std::atomic<qint64>>(0);
        pajlada::Signals::SignalHolder connections;

        ~File()
        {
            QObject::disconnect(paneDestroyed);
        }
    };

    ChatRecordingController *owner;
    QString optionsPath;
    QJsonObject options;
    QThread thread;
    recording::Writer *writer;
    QTimer *sourceTimer = nullptr;
    std::map<QString, std::unique_ptr<Group>> groups;
    std::map<QString, std::unique_ptr<File>> files;
    std::map<QString, QStringList> batches;
    std::shared_ptr<std::atomic<qint64>> queued =
        std::make_shared<std::atomic<qint64>>(0);
    std::vector<std::function<void()>> quitCallbacks;
    std::unique_ptr<QMessageBox> noticeBox;
    bool quitting = false;
    bool shutdown = false;
    bool controlsRefreshQueued = false;

    Impl(ChatRecordingController *owner, QString root, QString optionsPath,
         QString folder)
        : owner(owner)
        , optionsPath(std::move(optionsPath))
        , options{{"folder", folder},
                  {"format", "twitchDownloader"},
                  {"embedImages", false},
                  {"showPlatformBadges", true},
                  {"showHeaderButton", false}}
        , writer(new recording::Writer(std::move(root)))
    {
        QFile file(this->optionsPath);
        if (file.open(QIODevice::ReadOnly))
        {
            const auto saved =
                QJsonDocument::fromJson(file.read(64 * 1024)).object();
            for (const auto &key : {"folder", "format", "embedImages",
                                    "showPlatformBadges", "showHeaderButton"})
            {
                if (saved.contains(key))
                {
                    options.insert(key, saved.value(key));
                }
            }
        }
        writer->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, writer,
                         &QObject::deleteLater);
        thread.setObjectName("Chat recording");
        thread.start();
    }

    Group *group(SplitContainer *tab) const
    {
        if (!tab)
        {
            return nullptr;
        }
        for (const auto &[id, group] : groups)
        {
            if (group->tab == tab)
            {
                return group.get();
            }
        }
        return nullptr;
    }

    void refreshControlsSoon()
    {
        if (controlsRefreshQueued)
        {
            return;
        }
        controlsRefreshQueued = true;

        QTimer::singleShot(0, owner, [this] {
            controlsRefreshQueued = false;
            Q_EMIT owner->stateChanged();
        });
    }

    File *file(Split *pane) const
    {
        if (!pane)
        {
            return nullptr;
        }
        for (const auto &[id, file] : files)
        {
            if (file->pane == pane && !file->stopping)
            {
                return file.get();
            }
        }
        return nullptr;
    }

    void notify(Split *pane, const QString &text)
    {
        this->notify(pane, MessageBuilder(systemMessage, text).release());
    }

    void notify(Split *pane, const MessagePtrMut &message)
    {
        if (quitting)
        {
            return;
        }
        if (!pane)
        {
            for (auto *window : QApplication::topLevelWidgets())
            {
                for (auto *candidate : window->findChildren<Split *>())
                {
                    if (candidate->isVisible())
                    {
                        pane = candidate;
                        break;
                    }
                }
                if (pane)
                {
                    break;
                }
            }
        }
        if (pane)
        {
            message->serverReceivedTime = QDateTime::currentDateTimeUtc();
            message->flags.set(MessageFlag::DoNotLog);
            pane->getChannelView().addRecordingNotice(message);
        }
        else
        {
            if (!noticeBox)
            {
                noticeBox = std::make_unique<QMessageBox>(
                    QMessageBox::Information, "Chat recording", QString{},
                    QMessageBox::Ok);
                noticeBox->setTextFormat(Qt::RichText);
            }
            auto text = message->messageText.toHtmlEscaped();
            for (const auto &element : message->elements)
            {
                const auto link = element->getLink();
                if (link.type == Link::Url && QUrl(link.value).isLocalFile())
                {
                    text.replace("Open folder",
                                 QString("<a href=\"%1\">Open folder</a>")
                                     .arg(link.value.toHtmlEscaped()));
                    break;
                }
            }
            const auto previous =
                noticeBox->isVisible() ? noticeBox->text() : QString{};
            noticeBox->setText(
                previous.isEmpty() ? text : previous + "<br><br>" + text);
            noticeBox->open();
        }
    }

    void changed()
    {
        bool accepting = false;
        for (const auto &[id, file] : files)
        {
            accepting |= file->accepting;
        }
        recording::LiveMessageScope::setEnabled(accepting);
        if (accepting && !sourceTimer->isActive())
        {
            sourceTimer->start();
        }
        else if (!accepting)
        {
            sourceTimer->stop();
        }
        Q_EMIT owner->stateChanged();
        if (files.empty())
        {
            auto callbacks = std::move(quitCallbacks);
            quitCallbacks.clear();
            for (auto &callback : callbacks)
            {
                callback();
            }
        }
    }

    QJsonObject createFile(Group &group, Split *pane)
    {
        auto file = std::make_unique<File>();
        file->id = uuid();
        file->group = group.id;
        file->pane = pane;
        file->metadata = group.options;
        file->metadata.insert("id", file->id);
        file->metadata.insert("groupId", group.id);
        file->metadata.insert("paneNumber", group.nextPane++);
        file->metadata.insert("tabTitle", group.tab && group.tab->getTab()
                                              ? group.tab->getTab()->getTitle()
                                              : "Chat");
        file->metadata.insert("sources", sourceMetadata(pane));
        const auto id = file->id;
        file->connections.managedConnect(pane->channelChanged, [this, id] {
            const auto it = files.find(id);
            if (it != files.end())
            {
                updateSources(*it->second);
            }
        });
        file->paneDestroyed =
            QObject::connect(pane, &QObject::destroyed, owner, [this, id] {
                stopFile(id);
            });
        const auto metadata = file->metadata;
        files.emplace(id, std::move(file));
        return metadata;
    }

    void enqueue(File &file, QJsonObject record)
    {
        if (!file.accepting)
        {
            return;
        }
        record.insert("receivedAt", QDateTime::currentDateTimeUtc().toString(
                                        Qt::ISODateWithMs));
        record.insert("offsetSeconds", file.timer.nsecsElapsed() / 1.0e9);
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);

        constexpr qint64 PER_FILE = 4 * 1024 * 1024;
        constexpr qint64 GLOBAL = 32 * 1024 * 1024;
        if (file.queued->load() + bytes.size() > PER_FILE ||
            queued->load() + bytes.size() > GLOBAL)
        {
            if (const auto group = groups.find(file.group);
                group != groups.end())
            {
                group->second->failed = true;
            }
            notify(file.pane, "Chat recording stopped because storage could "
                              "not keep up. Queued chat is being saved, but "
                              "the recording is incomplete.");
            file.metadata.insert("incomplete", true);
            file.metadata.insert(
                "failure",
                "The recording queue reached its storage backlog limit.");
            stopFile(file.id);
            return;
        }
        const auto id = file.id;
        auto fileQueued = file.queued;
        auto allQueued = queued;
        fileQueued->fetch_add(bytes.size());
        allQueued->fetch_add(bytes.size());
        if (record.value("recordType") == "message")
        {
            ++file.acceptedMessages;
        }
        QMetaObject::invokeMethod(
            writer, [writer = writer, id, bytes, fileQueued, allQueued] {
                writer->append(id, bytes);
                fileQueued->fetch_sub(bytes.size());
                allQueued->fetch_sub(bytes.size());
            });
    }

    void updateSources(File &file)
    {
        const auto currentSources = sources(file.pane);
        if (currentSources.empty())
        {
            stopFile(file.id);
            return;
        }
        if (currentSources != file.watchedSources)
        {
            file.sourceConnections.clear();
            file.watchedSources = currentSources;
            file.connected.clear();
            file.live.clear();
            for (const auto &channel : currentSources)
            {
                file.connected[channel.get()] = true;
                file.live[channel.get()] = channel->isLive();
                file.sourceConnections.managedConnect(
                    channel->messageAppended,
                    [this, id = file.id, source = channel.get()](
                        const MessagePtr &message, auto) {
                        if (!message->flags.hasAny(
                                MessageFlag::ConnectedMessage,
                                MessageFlag::DisconnectedMessage))
                        {
                            return;
                        }
                        const auto it = files.find(id);
                        if (it == files.end() || !it->second->accepting)
                        {
                            return;
                        }
                        const bool connected =
                            message->flags.has(MessageFlag::ConnectedMessage);
                        it->second->connected[source] = connected;
                        enqueue(
                            *it->second,
                            {{"recordType", "event"},
                             {"kind", connected ? "connected" : "disconnected"},
                             {"source", recording::describeSource(*source)},
                             {"text", connected ? "Chat connected"
                                                : "Chat disconnected"}});
                        updateSources(*it->second);
                    });
            }
        }
        QJsonArray metadata;
        for (const auto &channel : currentSources)
        {
            metadata.append(recording::describeSource(*channel));
        }
        if (metadata != file.sourceSnapshot)
        {
            file.sourceSnapshot = metadata;
            for (const auto &source : metadata)
            {
                enqueue(file, {{"recordType", "event"},
                               {"kind", "sourceChanged"},
                               {"source", source},
                               {"text", "Recording source updated"}});
            }
        }
        bool anyLive = false;
        bool anyConnected = false;
        for (const auto &channel : currentSources)
        {
            const bool live = channel->isLive();
            if (file.live[channel.get()] != live)
            {
                enqueue(
                    file,
                    {{"recordType", "event"},
                     {"kind", live ? "broadcastStarted" : "broadcastEnded"},
                     {"source", recording::describeSource(*channel)},
                     {"text", live ? "Broadcast started" : "Broadcast ended"}});
                file.live[channel.get()] = live;
            }
            anyLive |= live && file.connected[channel.get()];
            anyConnected |= file.connected[channel.get()];
        }
        const char *status = "Waiting";
        if (!anyConnected)
        {
            status = "Reconnecting";
        }
        else if (anyLive)
        {
            status = "Recording";
        }
        if (file.accepting && file.status != status)
        {
            file.status = status;
            changed();
        }
    }

    void stopFile(const QString &id)
    {
        const auto it = files.find(id);
        if (it == files.end() || it->second->stopping)
        {
            return;
        }
        auto &file = *it->second;
        file.accepting = false;
        file.stopping = true;
        file.status = "Saving";
        if (const auto group = groups.find(file.group); group != groups.end())
        {
            bool anyRemaining = false;
            for (const auto &[otherId, other] : files)
            {
                anyRemaining |= other->group == file.group && !other->stopping;
            }
            if (!anyRemaining)
            {
                group->second->stopping = true;
            }
        }
        file.metadata.insert(
            "endedAt",
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        file.metadata.insert(
            "durationSeconds",
            file.timer.isValid() ? file.timer.nsecsElapsed() / 1.0e9 : 0.0);
        file.metadata.insert("acceptedMessages", file.acceptedMessages);
        if (const auto group = groups.find(file.group);
            group != groups.end() && group->second->tab &&
            group->second->tab->getTab())
        {
            file.metadata.insert("tabTitle",
                                 group->second->tab->getTab()->getTitle());
        }
        if (file.prepared)
        {
            const auto metadata = file.metadata;
            QMetaObject::invokeMethod(writer, [writer = writer, id, metadata] {
                writer->finish(id, metadata);
            });
        }
        changed();
    }

    void removeFile(const QString &id)
    {
        const auto it = files.find(id);
        if (it == files.end())
        {
            return;
        }
        const auto group = it->second->group;
        files.erase(it);
        for (const auto &[fileId, file] : files)
        {
            if (file->group == group)
            {
                changed();
                return;
            }
        }
        groups.erase(group);
        changed();
    }
};

ChatRecordingController::ChatRecordingController(const Paths &paths)
    : ChatRecordingController(
          QDir(QStandardPaths::writableLocation(
                   QStandardPaths::AppLocalDataLocation))
              .filePath(
                  "RecordingRecovery/" +
                  recording::stableKey({{"profile", paths.settingsDirectory}})
                      .left(16)),
          QDir(paths.settingsDirectory).filePath("chat-recording.json"),
          QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
              .filePath("Moltorino Recordings"))
{
}

ChatRecordingController::ChatRecordingController(QString recoveryRoot,
                                                 QString optionsPath,
                                                 QString defaultFolder)
    : impl_(std::make_unique<Impl>(this, std::move(recoveryRoot),
                                   std::move(optionsPath),
                                   std::move(defaultFolder)))
{
    connect(impl_->writer, &recording::Writer::prepared, this,
            [this](const QString &token, const QString &error) {
                const auto batch = impl_->batches.find(token);
                if (batch == impl_->batches.end())
                {
                    return;
                }
                const auto ids = batch->second;
                impl_->batches.erase(batch);
                const auto now =
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                QElapsedTimer commonTimer;
                commonTimer.start();
                bool reported = false;
                for (const auto &id : ids)
                {
                    const auto it = impl_->files.find(id);
                    if (it == impl_->files.end())
                    {
                        continue;
                    }
                    auto &file = *it->second;
                    const auto group = impl_->groups.find(file.group);
                    if (!error.isEmpty() || file.stopping ||
                        group == impl_->groups.end() ||
                        group->second->stopping || !file.pane)
                    {
                        if (!error.isEmpty() && !reported)
                        {
                            impl_->notify(
                                file.pane,
                                "Chat recording could not start: " + error);
                            reported = true;
                        }
                        QMetaObject::invokeMethod(impl_->writer,
                                                  [writer = impl_->writer, id] {
                                                      writer->abort(id);
                                                  });
                        impl_->removeFile(id);
                        continue;
                    }
                    file.prepared = true;
                    file.accepting = true;
                    file.timer = commonTimer;
                    file.metadata.insert("startedAt", now);
                    const auto metadata = file.metadata;
                    QMetaObject::invokeMethod(
                        impl_->writer, [writer = impl_->writer, id, metadata] {
                            writer->updateMetadata(id, metadata);
                        });
                    group->second->preparing = false;

                    impl_->enqueue(file, {{"recordType", "event"},
                                          {"kind", "started"},
                                          {"text", "Recording started"}});
                    impl_->updateSources(file);
                    impl_->notify(file.pane, "Recording started.");
                }
                impl_->changed();
            });
    connect(impl_->writer, &recording::Writer::failed, this,
            [this](const QString &id, const QString &error) {
                const auto it = impl_->files.find(id);
                if (it == impl_->files.end())
                {
                    return;
                }
                auto &file = *it->second;
                if (const auto group = impl_->groups.find(file.group);
                    group != impl_->groups.end())
                {
                    group->second->failed = true;
                }
                impl_->notify(
                    file.pane,
                    "Chat recording stopped: " + error +
                        ". Any chat already saved can be recovered.");
                impl_->stopFile(id);
            });
    connect(impl_->writer, &recording::Writer::finished, this,
            [this](const QString &id, const QString &path, qint64 messages,
                   qint64 missing, const QString &error) {
                const auto it = impl_->files.find(id);
                auto *pane = it == impl_->files.end() ? nullptr
                                                      : it->second->pane.data();
                if (error.isEmpty())
                {
                    const auto duration =
                        it == impl_->files.end()
                            ? -1.
                            : it->second->metadata.value("durationSeconds")
                                  .toDouble(-1.);
                    impl_->notify(pane, recording::makeSavedMessage(
                                            path, messages, duration, missing));
                }
                else
                {
                    impl_->notify(
                        pane, "Chat recording could not be saved: " + error +
                                  ". Its local recovery data was kept.");
                }
                impl_->removeFile(id);
            });
    connect(impl_->writer, &recording::Writer::recoverable, this,
            [this](const QStringList &ids) {
                if (impl_->quitting)
                {
                    return;
                }
                QMessageBox prompt(
                    QMessageBox::Question, "Recover chat recordings",
                    QString("%1 unfinished chat recordings were found.")
                        .arg(ids.size()),
                    QMessageBox::NoButton);
                auto *recover =
                    prompt.addButton("Recover", QMessageBox::AcceptRole);
                prompt.addButton("Later", QMessageBox::RejectRole);
                prompt.exec();
                if (prompt.clickedButton() == recover)
                {
                    QMetaObject::invokeMethod(impl_->writer,
                                              [writer = impl_->writer, ids] {
                                                  writer->recover(ids);
                                              });
                }
            });
    auto *timer = impl_->sourceTimer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        for (const auto &[id, file] : impl_->files)
        {
            if (file->accepting)
            {
                impl_->updateSources(*file);
            }
        }
    });
}

ChatRecordingController::~ChatRecordingController()
{
    this->shutdown();
}

QJsonObject ChatRecordingController::options() const
{
    return impl_->options;
}

QString ChatRecordingController::setOptions(QJsonObject options)
{
    const auto folder = options.value("folder").toString().trimmed();
    const auto format = options.value("format").toString();
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder))
    {
        return "Choose a full path for the recording folder.";
    }
    if (format != "twitchDownloader" && format != "basic" && format != "text")
    {
        return "Choose a recording format.";
    }
    options.insert("folder", QDir::cleanPath(folder));
    const auto data = QJsonDocument(options).toJson();
    QSaveFile file(impl_->optionsPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(data) != data.size() || !file.commit())
    {
        return "Recording preferences could not be saved: " +
               file.errorString();
    }
    impl_->options = std::move(options);
    Q_EMIT this->stateChanged();
    return {};
}

bool ChatRecordingController::canStart(SplitContainer *tab) const
{
    if (!tab || impl_->quitting || impl_->group(tab))
    {
        return false;
    }
    for (auto *pane : tab->getSplits())
    {
        if (!sources(pane).empty())
        {
            return true;
        }
    }
    return false;
}

bool ChatRecordingController::isActive(SplitContainer *tab) const
{
    return impl_->group(tab) != nullptr;
}

bool ChatRecordingController::hasRecordings() const
{
    return !impl_->files.empty();
}

QString ChatRecordingController::status(SplitContainer *tab) const
{
    const auto *group = impl_->group(tab);
    if (!group)
    {
        return {};
    }
    if (group->preparing)
    {
        return "Starting";
    }
    bool accepting = false;
    bool recording = false;
    bool waiting = false;
    for (const auto &[id, file] : impl_->files)
    {
        if (file->group == group->id)
        {
            accepting |= file->accepting;
            recording |= file->status == "Recording";
            waiting |= file->status == "Waiting";
        }
    }
    if (!accepting)
    {
        return "Saving";
    }
    if (group->failed)
    {
        return "Recording with errors";
    }
    if (recording)
    {
        return "Recording";
    }
    return waiting ? "Waiting" : "Reconnecting";
}

void ChatRecordingController::toggle(SplitContainer *tab)
{
    if (impl_->group(tab))
    {
        this->stop(tab);
        return;
    }
    if (!this->canStart(tab))
    {
        return;
    }
    auto group = std::make_unique<Impl::Group>();
    group->id = uuid();
    group->tab = tab;
    group->options = impl_->options;
    if (group->options.value("format") != "twitchDownloader")
    {
        group->options.insert("embedImages", false);
    }
    const auto id = group->id;
    impl_->groups.emplace(id, std::move(group));
    QJsonArray files;
    QStringList ids;
    for (auto *pane : tab->getSplits())
    {
        if (!sources(pane).empty())
        {
            const auto metadata =
                impl_->createFile(*impl_->groups.at(id), pane);
            files.append(metadata);
            ids.append(metadata.value("id").toString());
        }
    }
    impl_->batches.emplace(id, ids);
    impl_->groups.at(id)->tabDestroyed =
        connect(tab, &QObject::destroyed, this, [this, id] {
            QStringList files;
            for (const auto &[fileId, file] : impl_->files)
            {
                if (file->group == id)
                {
                    files.append(fileId);
                }
            }
            for (const auto &file : files)
            {
                impl_->stopFile(file);
            }
        });
    QMetaObject::invokeMethod(impl_->writer,
                              [writer = impl_->writer, id, files] {
                                  writer->prepare(id, files);
                              });
    impl_->changed();
}

void ChatRecordingController::stop(SplitContainer *tab)
{
    if (auto *group = impl_->group(tab))
    {
        group->stopping = true;
        for (const auto &[id, file] : impl_->files)
        {
            if (file->group == group->id)
            {
                impl_->stopFile(id);
            }
        }
    }
}

void ChatRecordingController::stopPane(Split *pane)
{
    if (auto *file = impl_->file(pane))
    {
        impl_->stopFile(file->id);
    }
}

void ChatRecordingController::paneAdded(SplitContainer *tab, Split *pane)
{
    if (auto *existing = impl_->file(pane))
    {
        const auto group = impl_->groups.find(existing->group);
        if (group != impl_->groups.end() && group->second->tab != tab)
        {
            impl_->stopFile(existing->id);
        }
        else
        {
            impl_->refreshControlsSoon();
            return;
        }
    }
    auto *group = impl_->group(tab);
    if (!group || group->stopping || sources(pane).empty())
    {
        impl_->refreshControlsSoon();
        return;
    }
    const auto metadata = impl_->createFile(*group, pane);
    const auto id = metadata.value("id").toString();
    impl_->batches.emplace(id, QStringList{id});
    QMetaObject::invokeMethod(impl_->writer,
                              [writer = impl_->writer, id, metadata] {
                                  writer->prepare(id, QJsonArray{metadata});
                              });
    impl_->changed();
}

void ChatRecordingController::capture(Split *pane, const MessagePtr &message)
{
    if (auto *file = impl_->file(pane); file && file->accepting)
    {
        if (const auto *record =
                recording::LiveMessageScope::current(message.get()))
        {
            impl_->enqueue(*file, *record);
        }
    }
}

void ChatRecordingController::captureEvent(const Channel &source,
                                           QJsonObject event)
{
    event.insert("recordType", "event");
    event.insert("source", recording::describeSource(source));
    if (!event.contains("createdAt"))
    {
        event.insert("createdAt", QDateTime::currentDateTimeUtc().toString(
                                      Qt::ISODateWithMs));
    }
    for (const auto &[id, file] : impl_->files)
    {
        if (!file->accepting)
        {
            continue;
        }
        for (const auto &candidate : sources(file->pane))
        {
            if (candidate.get() == &source)
            {
                if (event.value("kind") == "connected" ||
                    event.value("kind") == "disconnected" ||
                    event.value("kind") == "broadcastEnded")
                {
                    file->connected[&source] =
                        event.value("kind") != "disconnected";
                }
                impl_->enqueue(*file, event);
                impl_->updateSources(*file);
                break;
            }
        }
    }
}

void ChatRecordingController::finishBeforeQuit(
    std::function<void()> continuation)
{
    impl_->quitting = true;
    impl_->quitCallbacks.push_back(std::move(continuation));
    for (const auto &[id, file] : impl_->files)
    {
        impl_->stopFile(id);
    }
    impl_->changed();
}

void ChatRecordingController::shutdown()
{
    if (impl_->shutdown)
    {
        return;
    }
    impl_->shutdown = true;
    impl_->quitting = true;
    for (const auto &[id, file] : impl_->files)
    {
        impl_->stopFile(id);
        if (!file->prepared)
        {
            QMetaObject::invokeMethod(impl_->writer,
                                      [writer = impl_->writer, id] {
                                          writer->abort(id);
                                      });
        }
    }

    impl_->batches.clear();
    QMetaObject::invokeMethod(
        impl_->writer,
        [writer = impl_->writer] {
            writer->finishAll();
        },
        Qt::BlockingQueuedConnection);
    impl_->thread.quit();
    impl_->thread.wait();
    impl_->files.clear();
    impl_->groups.clear();
    impl_->changed();
}

void ChatRecordingController::checkRecovery()
{
    if (impl_->quitting)
    {
        return;
    }
    QMetaObject::invokeMethod(impl_->writer, [writer = impl_->writer] {
        writer->scanRecovery();
    });
}

QWidget *makeChatRecordingSettings(QWidget *parent)
{
    auto *widget = new RecordingSettings(parent);
    auto *form = new QFormLayout(widget);
    form->setContentsMargins(0, 0, 0, 0);

    auto *controller = getApp()->getChatRecordings();
    if (!controller)
    {
        return widget;
    }

    const auto options = controller->options();

    auto *format = new QComboBox(widget);
    format->addItem("TwitchDownloader JSON", "twitchDownloader");
    format->addItem("Basic JSON", "basic");
    format->addItem("Plain text", "text");
    format->setCurrentIndex(
        std::max(0, format->findData(options.value("format").toString())));
    form->addRow("Format", format);

    auto *row = new QWidget(widget);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *folder = new QLineEdit(options.value("folder").toString(), row);
    auto *browse = new QPushButton("Browse", row);
    auto *open = new QPushButton("Open folder", row);
    layout->addWidget(folder, 1);
    layout->addWidget(browse);
    layout->addWidget(open);
    form->addRow("Folder", row);

    auto *embed = new QCheckBox("Embed emote and badge images", widget);
    embed->setObjectName("recordingEmbedImages");
    embed->setChecked(options.value("embedImages").toBool());
    form->addRow(embed);

    auto *platformBadges =
        new QCheckBox("Show platform badges in mixed chat", widget);
    platformBadges->setObjectName("recordingShowPlatformBadges");
    platformBadges->setChecked(
        options.value("showPlatformBadges").toBool(true));
    form->addRow(platformBadges);

    auto *showButton =
        new QCheckBox("Show recording button in channel header", widget);
    showButton->setObjectName("recordingShowHeaderButton");
    showButton->setChecked(options.value("showHeaderButton").toBool());
    form->addRow(showButton);

    auto *hotkeys = getApp()->getHotkeys();
    auto *hotkeyRow = new QWidget(widget);
    auto *hotkeyLayout = new QHBoxLayout(hotkeyRow);
    hotkeyLayout->setContentsMargins(0, 0, 0, 0);
    auto *sequence = new QLabel(hotkeyRow);
    sequence->setObjectName("recordingHotkeySequence");
    sequence->setWordWrap(true);
    auto *editHotkey = new QPushButton(hotkeyRow);
    editHotkey->setObjectName("recordingEditHotkey");
    hotkeyLayout->addWidget(sequence, 1);
    hotkeyLayout->addWidget(editHotkey);
    form->addRow("Hotkey", hotkeyRow);

    auto refreshHotkey = [hotkeys, sequence, editHotkey] {
        QStringList sequences;
        for (const auto &hotkey :
             hotkeys->getHotkeys(HotkeyCategory::Split, "toggleChatRecording"))
        {
            sequences.append(
                hotkey->keySequence().toString(QKeySequence::NativeText));
        }
        sequence->setText(sequences.isEmpty() ? "Not set"
                                              : sequences.join(", "));
        editHotkey->setText(sequences.isEmpty() ? "Set hotkey" : "Edit");
    };
    widget->connections.managedConnect(hotkeys->onItemsUpdated, refreshHotkey);
    refreshHotkey();
    QObject::connect(
        editHotkey, &QPushButton::clicked, widget,
        [widget, hotkeys, editHotkey, refreshHotkey] {
            const auto matches = hotkeys->getHotkeys(HotkeyCategory::Split,
                                                     "toggleChatRecording");
            std::shared_ptr<Hotkey> existing;
            if (matches.size() == 1)
            {
                existing = matches.front();
            }
            else if (matches.size() > 1)
            {
                QMenu menu(widget);
                for (const auto &hotkey : matches)
                {
                    auto *action =
                        menu.addAction(hotkey->name() + " (" +
                                       hotkey->keySequence().toString(
                                           QKeySequence::NativeText) +
                                       ")");
                    QObject::connect(action, &QAction::triggered, &menu,
                                     [&existing, hotkey] {
                                         existing = hotkey;
                                     });
                }
                menu.addSeparator();
                menu.addAction("Add hotkey");
                if (!menu.exec(editHotkey->mapToGlobal(
                        QPoint(0, editHotkey->height()))))
                {
                    return;
                }
            }
            auto initial = existing;
            if (!initial)
            {
                QString name = "Toggle tab recording";
                for (int suffix = 2; hotkeys->getHotkeyByName(name); ++suffix)
                {
                    name = QString("Toggle tab recording %1").arg(suffix);
                }
                initial = std::make_shared<Hotkey>(
                    HotkeyCategory::Split, QKeySequence{},
                    "toggleChatRecording", std::vector<QString>{}, name);
            }
            EditHotkeyDialog dialog(initial, widget);
            if (!existing)
            {
                dialog.setWindowTitle("Add hotkey");
            }
            if (dialog.exec() == QDialog::Accepted)
            {
                if (existing)
                {
                    hotkeys->replaceHotkey(existing->name(), dialog.data());
                }
                else
                {
                    hotkeys->addHotkey(dialog.data());
                }
                hotkeys->save();
                refreshHotkey();
            }
        });

    auto *error = new QLabel(widget);
    error->setWordWrap(true);
    error->hide();
    form->addRow(error);

    auto save = [controller, format, folder, embed, platformBadges, showButton,
                 error] {
        const auto message = controller->setOptions(
            {{"format", format->currentData().toString()},
             {"folder", folder->text()},
             {"embedImages", embed->isChecked()},
             {"showPlatformBadges", platformBadges->isChecked()},
             {"showHeaderButton", showButton->isChecked()}});
        error->setText(message);
        error->setVisible(!message.isEmpty());
    };

    QObject::connect(
        format, &QComboBox::currentIndexChanged, widget,
        [embed, platformBadges, format, save] {
            embed->setEnabled(format->currentData() == "twitchDownloader");
            platformBadges->setEnabled(format->currentData() ==
                                       "twitchDownloader");
            save();
        });
    embed->setEnabled(format->currentData() == "twitchDownloader");
    platformBadges->setEnabled(format->currentData() == "twitchDownloader");
    QObject::connect(embed, &QCheckBox::toggled, widget, save);
    QObject::connect(platformBadges, &QCheckBox::toggled, widget, save);
    QObject::connect(showButton, &QCheckBox::toggled, widget, save);
    QObject::connect(folder, &QLineEdit::editingFinished, widget, save);

    QObject::connect(browse, &QPushButton::clicked, widget,
                     [widget, folder, save] {
                         const auto path = QFileDialog::getExistingDirectory(
                             widget, "Recording folder", folder->text());
                         if (!path.isEmpty())
                         {
                             folder->setText(path);
                             save();
                         }
                     });

    QObject::connect(open, &QPushButton::clicked, widget, [folder] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder->text()));
    });

    return widget;
}

}
