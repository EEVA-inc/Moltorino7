#include "controllers/plugins/api/DateTime.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/SolTypes.hpp"  // IWYU pragma: keep

#    include <QDateTime>
#    include <sol/sol.hpp>

namespace chatterino::lua::api::datetime {

void createUserTypes(sol::table &c2)
{
    c2.new_usertype<QDateTime>(
        "DateTime", sol::no_constructor, sol::meta_method::to_string,
        [](const QDateTime &self) {
            return self.toString(Qt::ISODateWithMs);
        },
        "from_iso_string",
        [](const QString &value) {
            return QDateTime::fromString(value, Qt::ISODateWithMs);
        },
        "to_iso_string",
        [](const QDateTime &self) {
            return self.toString(Qt::ISODateWithMs);
        },
        "to_iso_string_without_ms",
        [](const QDateTime &self) {
            return self.toString(Qt::ISODate);
        },
        "current_local",
        [] {
            return QDateTime::currentDateTime();
        },
        "current_utc",
        [] {
            return QDateTime::currentDateTimeUtc();
        },
        "from_unix_milliseconds",
        [](qint64 timestamp) {
            return QDateTime::fromMSecsSinceEpoch(timestamp);
        },
        "from_unix_seconds",
        [](qint64 timestamp) {
            return QDateTime::fromSecsSinceEpoch(timestamp);
        },
        "to_unix_milliseconds",
        [](const QDateTime &self) {
            return self.toMSecsSinceEpoch();
        },
        "to_unix_seconds",
        [](const QDateTime &self) {
            return self.toSecsSinceEpoch();
        },
        "is_local",
        [](const QDateTime &self) {
            return self.timeSpec() == Qt::LocalTime;
        },
        "is_utc",
        [](const QDateTime &self) {
            return self.timeSpec() == Qt::UTC;
        },
        "to_local",
        [](const QDateTime &self) {
            return self.toLocalTime();
        },
        "to_utc",
        [](const QDateTime &self) {
            return self.toUTC();
        });
}

}

#endif
