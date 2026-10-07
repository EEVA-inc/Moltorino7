#pragma once

#include "messages/Message.hpp"

#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>

class QWidget;

namespace chatterino {
class Paths;
class Channel;
class Split;
class SplitContainer;

class ChatRecordingController final : public QObject
{
    Q_OBJECT
public:
    explicit ChatRecordingController(const Paths &paths);
    ChatRecordingController(QString recoveryRoot, QString optionsPath,
                            QString defaultFolder);
    ~ChatRecordingController() override;

    QJsonObject options() const;
    QString setOptions(QJsonObject options);
    void toggle(SplitContainer *tab);
    void stop(SplitContainer *tab);
    void stopPane(Split *pane);
    void paneAdded(SplitContainer *tab, Split *pane);
    void capture(Split *pane, const MessagePtr &message);
    void captureEvent(const Channel &source, QJsonObject event);
    bool isActive(SplitContainer *tab) const;
    bool hasRecordings() const;
    bool canStart(SplitContainer *tab) const;
    QString status(SplitContainer *tab) const;
    void finishBeforeQuit(std::function<void()> continuation);
    void shutdown();
    void checkRecovery();

Q_SIGNALS:
    void stateChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

QWidget *makeChatRecordingSettings(QWidget *parent = nullptr);
SplitContainer *recordingTabFor(Split *pane);

}
