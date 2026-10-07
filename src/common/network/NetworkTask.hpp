// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class QNetworkReply;

namespace chatterino {

class NetworkData;

}

namespace chatterino::network::detail {

class NetworkTask : public QObject
{
    Q_OBJECT

public:
    NetworkTask(std::shared_ptr<NetworkData> &&data);
    ~NetworkTask() override;

    NetworkTask(const NetworkTask &) = delete;
    NetworkTask(NetworkTask &&) = delete;
    NetworkTask &operator=(const NetworkTask &) = delete;
    NetworkTask &operator=(NetworkTask &&) = delete;

public Q_SLOTS:
    void run();

private:
    QNetworkReply *createReply();

    void logReply();
    void writeToCache(const QByteArray &bytes) const;

    std::shared_ptr<NetworkData> data_;
    QNetworkReply *reply_{};
    QTimer *timer_{};
    std::optional<std::stop_callback<std::function<void()>>> cancellation_;

private Q_SLOTS:
    void timeout();
    void responseTooLarge();
    void finished();
};

}
