#include "common/SecureCredentials.hpp"

#include <QTimer>

namespace chatterino {

namespace {

QString unavailableMessage()
{
    return QStringLiteral(
        "Secure credentials are not available in this build.");
}

}

void SecureCredentials::read(const QString &, ReadCallback callback)
{
    QTimer::singleShot(0, [callback = std::move(callback)] {
        if (callback)
        {
            callback(makeUnexpected(unavailableMessage()));
        }
    });
}

void SecureCredentials::write(const QString &, const QString &,
                              WriteCallback callback)
{
    QTimer::singleShot(0, [callback = std::move(callback)] {
        if (callback)
        {
            callback(makeUnexpected(unavailableMessage()));
        }
    });
}

void SecureCredentials::remove(const QString &, WriteCallback callback)
{
    QTimer::singleShot(0, [callback = std::move(callback)] {
        if (callback)
        {
            callback(makeUnexpected(unavailableMessage()));
        }
    });
}

}
