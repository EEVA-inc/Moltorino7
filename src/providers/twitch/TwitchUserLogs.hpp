#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class QUrl;

namespace chatterino::twitch_user_logs {

struct Period {
    int year{};
    int month{};
    std::optional<int> day;

    QString key() const;
    QString label() const;
    QString compactLabel() const;
    bool operator==(const Period &) const = default;
};

struct Message {
    QString text;
    QString siteAnchor;
    QDateTime timestamp;
    qsizetype sourceOffset = -1;
};

struct Page {
    QVector<Message> messages;
    qsizetype availableMessageCount{};
    qsizetype sourceRecordCount{};
    bool hasMoreResults{};
};

using PeriodsCallback = std::function<void(QVector<Period>)>;
using PageCallback = std::function<void(Page)>;
using AnchorCallback = std::function<void(QString)>;
using ErrorCallback = std::function<void(QString)>;
using ContinueCallback = std::function<bool()>;

void loadAvailablePeriods(const QString &channel, const QString &user,
                          PeriodsCallback onSuccess, ErrorCallback onError);
void loadPeriod(const QString &channel, const QString &user,
                const Period &period, PageCallback onSuccess,
                ErrorCallback onError, ContinueCallback shouldContinue = {});
void search(const QString &channel, const QString &user, const QString &query,
            qsizetype offset, PageCallback onSuccess, ErrorCallback onError,
            ContinueCallback shouldContinue = {});
void resolvePeriodMessageAnchor(const QString &channel, const QString &user,
                                const Period &period, const Message &expected,
                                AnchorCallback onSuccess,
                                ErrorCallback onError);
void resolveSearchMessageAnchor(const QString &channel, const QString &user,
                                const QString &query, const Message &expected,
                                AnchorCallback onSuccess,
                                ErrorCallback onError);

namespace detail {

constexpr qsizetype LOG_PAGE_SIZE = 20000;
constexpr qsizetype SEARCH_PAGE_SIZE = 2000;

QUrl availablePeriodsUrl(const QString &channel, const QString &user);
QUrl periodUrl(const QString &channel, const QString &user,
               const Period &period);
QUrl searchUrl(const QString &channel, const QString &user,
               const QString &query, qsizetype offset = 0);
QUrl periodMessageUrl(const QString &channel, const QString &user,
                      const Period &period, qsizetype sourceOffset);
QUrl searchMessageUrl(const QString &channel, const QString &user,
                      const QString &query, qsizetype sourceOffset);
QUrl messageUrl(const QString &channel, const Message &message);

std::optional<QVector<Period>> parseAvailablePeriods(const QByteArray &data,
                                                     QString *error = nullptr);
std::optional<Page> parseTextPage(const QByteArray &data,
                                  QString *error = nullptr);
std::optional<Page> parseSearchPage(const QByteArray &data, qsizetype offset,
                                    QString *error = nullptr);
std::optional<Message> parseJsonMessage(const QByteArray &data,
                                        QString *error = nullptr);

}
}
