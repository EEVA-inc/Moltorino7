#pragma once

#include "common/ChatterinoSetting.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDate>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringView>
#include <QTime>
#include <QTimer>

namespace chatterino {

QString dailyPositiveQuote(QRandomGenerator &random,
                           QStringView previousQuote = {});
QString dailyPositiveLeadIn(const QTime &time, QRandomGenerator &random,
                            QStringView previousLeadIn = {});
bool dailyPositiveMessageIsDue(const QDate &lastShownDate, const QDate &today);

class MoltorinoDailyMessage final : public QObject
{
public:
    explicit MoltorinoDailyMessage(QObject *parent = nullptr);

    void start();

private:
    void showIfDue();
    void scheduleNextDay();
    void scheduleRetry();

    QStringSetting lastQuote_{"/moltorino/dailyMessage/lastQuote", {}};
    QStringSetting lastLeadIn_{"/moltorino/dailyMessage/lastLeadIn", {}};
    QTimer timer_;
    bool started_ = false;
    pajlada::Signals::SignalHolder signalHolder_;
};

}
