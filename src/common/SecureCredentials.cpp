#include "common/SecureCredentials.hpp"

#include "util/PostToThread.hpp"

#include <keychain/keychain.h>
#include <QtConcurrent>

#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

constexpr auto PACKAGE = "com.moltobenne.moltorino7";
constexpr auto SERVICE = "youtube-oauth";

QString errorText(const keychain::Error &error)
{
    return QString::fromStdString(error.message);
}

QString exceptionText(const std::exception &error)
{
    const auto detail = QString::fromUtf8(error.what()).trimmed();
    return detail.isEmpty()
               ? QStringLiteral("The system credential store failed.")
               : QStringLiteral("The system credential store failed: ") +
                     detail.left(512);
}

struct CredentialLane {
    std::mutex mutex;
    std::deque<std::function<void()>> operations;
    bool workerRunning = false;
};

void drainCredentialLane(const std::shared_ptr<CredentialLane> &lane)
{
    while (true)
    {
        std::function<void()> operation;
        {
            std::scoped_lock lock(lane->mutex);
            if (lane->operations.empty())
            {
                lane->workerRunning = false;
                return;
            }
            operation = std::move(lane->operations.front());
            lane->operations.pop_front();
        }

        try
        {
            operation();
        }
        catch (...)
        {
        }
    }
}

std::shared_ptr<CredentialLane> laneFor(const QString &key)
{
    static std::mutex lanesMutex;
    static std::unordered_map<std::string, std::weak_ptr<CredentialLane>> lanes;
    std::scoped_lock lock(lanesMutex);
    std::erase_if(lanes, [](const auto &entry) {
        return entry.second.expired();
    });
    auto &weak = lanes[key.toStdString()];
    auto lane = weak.lock();
    if (!lane)
    {
        lane = std::make_shared<CredentialLane>();
        weak = lane;
    }
    return lane;
}

void runOrdered(const QString &key, std::function<void()> operation)
{
    auto lane = laneFor(key);
    bool startWorker = false;
    {
        std::scoped_lock lock(lane->mutex);
        lane->operations.push_back(std::move(operation));
        if (!lane->workerRunning)
        {
            lane->workerRunning = true;
            startWorker = true;
        }
    }
    if (startWorker)
    {
        std::ignore = QtConcurrent::run([lane = std::move(lane)] {
            drainCredentialLane(lane);
        });
    }
}

}

namespace chatterino {

void SecureCredentials::read(const QString &key, ReadCallback callback)
{
    runOrdered(key, [key = key.toStdString(),
                     callback = std::move(callback)]() mutable {
        ExpectedStr<QString> result = makeUnexpected(
            QStringLiteral("The system credential store failed."));
        try
        {
            keychain::Error error;
            auto secret = keychain::getPassword(PACKAGE, SERVICE, key, error);
            if (error.type == keychain::ErrorType::NotFound)
            {
                result = makeUnexpected(
                    QStringLiteral("Credential was not found."));
            }
            else if (error)
            {
                result = makeUnexpected(errorText(error));
            }
            else
            {
                result = QString::fromStdString(secret);
            }
        }
        catch (const std::exception &error)
        {
            result = makeUnexpected(exceptionText(error));
        }
        catch (...)
        {
        }
        postToGuiThread([callback = std::move(callback),
                         result = std::move(result)]() mutable {
            if (callback)
            {
                callback(std::move(result));
            }
        });
    });
}

void SecureCredentials::write(const QString &key, const QString &secret,
                              WriteCallback callback)
{
    runOrdered(key, [key = key.toStdString(), secret = secret.toStdString(),
                     callback = std::move(callback)]() mutable {
        ExpectedStr<void> result;
        try
        {
            keychain::Error error;
            keychain::setPassword(PACKAGE, SERVICE, key, secret, error);
            if (error)
            {
                result = makeUnexpected(errorText(error));
            }
        }
        catch (const std::exception &error)
        {
            result = makeUnexpected(exceptionText(error));
        }
        catch (...)
        {
            result = makeUnexpected(
                QStringLiteral("The system credential store failed."));
        }
        postToGuiThread([callback = std::move(callback),
                         result = std::move(result)]() mutable {
            if (callback)
            {
                callback(std::move(result));
            }
        });
    });
}

void SecureCredentials::remove(const QString &key, WriteCallback callback)
{
    runOrdered(key, [key = key.toStdString(),
                     callback = std::move(callback)]() mutable {
        ExpectedStr<void> result;
        try
        {
            keychain::Error error;
            keychain::deletePassword(PACKAGE, SERVICE, key, error);
            if (error && error.type != keychain::ErrorType::NotFound)
            {
                result = makeUnexpected(errorText(error));
            }
        }
        catch (const std::exception &error)
        {
            result = makeUnexpected(exceptionText(error));
        }
        catch (...)
        {
            result = makeUnexpected(
                QStringLiteral("The system credential store failed."));
        }
        postToGuiThread([callback = std::move(callback),
                         result = std::move(result)]() mutable {
            if (callback)
            {
                callback(std::move(result));
            }
        });
    });
}

}
