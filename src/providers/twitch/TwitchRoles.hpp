#pragma once

#include <QDate>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

class QByteArray;
class QUrl;

namespace chatterino::twitch_roles {

enum class Perspective { User, Channel };
enum class Role { Moderator, Vip, Founder, Artist };

constexpr std::array<Role, 4> ROLES{
    Role::Moderator,
    Role::Vip,
    Role::Founder,
    Role::Artist,
};

struct Summary {
    bool found{};
    QString userID;
    QString login;
    QString displayName;
    std::array<qint64, ROLES.size()> counts{};

    qint64 count(Role role) const;
};

struct Entry {
    QString userID;
    QString login;
    QString displayName;
    QDate grantedDate;
    bool partner{};
    bool affiliate{};
};

struct Page {
    QVector<Entry> entries;
    qint64 total{};
    QString nextCursor;
};

using SummaryCallback = std::function<void(Summary)>;
using PageCallback = std::function<void(Page)>;
using ErrorCallback = std::function<void(QString)>;

QString rolePath(Role role);

void loadSummary(Perspective perspective, const QString &userID,
                 SummaryCallback onSuccess, ErrorCallback onError);
void loadPage(Perspective perspective, Role role, const QString &userID,
              const QString &cursor, PageCallback onSuccess,
              ErrorCallback onError);

namespace detail {

constexpr int PAGE_SIZE = 100;

QUrl summaryUrl(Perspective perspective, const QString &userID);
QUrl pageUrl(Perspective perspective, Role role, const QString &userID,
             const QString &cursor = {});
QUrl websiteUrl(Perspective perspective, const QString &login);
bool repeatsCursor(const QString &requestedCursor,
                   const QString &nextCursor);
void sortNewestFirst(QVector<Entry> &entries);

std::optional<Summary> parseSummary(const QByteArray &data,
                                    QString *error = nullptr);
std::optional<Page> parsePage(const QByteArray &data,
                              QString *error = nullptr);

}
}
