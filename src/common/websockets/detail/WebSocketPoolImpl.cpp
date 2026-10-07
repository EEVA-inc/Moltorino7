// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/websockets/detail/WebSocketPoolImpl.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "common/websockets/detail/WebSocketConnection.hpp"
#include "util/RenameThread.hpp"

#include <boost/certify/https_verification.hpp>
#include <QFileInfo>
#include <QStringBuilder>
#include <QStringList>

namespace {
#ifdef Q_OS_LINUX
void loadLinuxSystemCaCertificates(boost::asio::ssl::context &ssl)
{
    const QStringList files{
        qEnvironmentVariable("SSL_CERT_FILE"),
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
    for (const auto &path : files)
    {
        if (!QFileInfo(path).isFile())
        {
            continue;
        }
        boost::system::error_code ec;
        ssl.load_verify_file(path.toStdString(), ec);
        if (ec)
        {
            qCDebug(chatterinoWebsocket) << "Failed to load CA file" << path;
        }
    }
    const QStringList directories{
        qEnvironmentVariable("SSL_CERT_DIR"),
        "/etc/ssl/certs",
        "/etc/pki/tls/certs",
        "/etc/pki/ca-trust/extracted/pem",
    };
    for (const auto &path : directories)
    {
        if (!QFileInfo(path).isDir())
        {
            continue;
        }
        boost::system::error_code ec;
        ssl.add_verify_path(path.toStdString(), ec);
        if (ec)
        {
            qCDebug(chatterinoWebsocket) << "Failed to load CA directory" << path;
        }
    }
}
#endif
}

namespace chatterino::ws::detail {

WebSocketPoolImpl::WebSocketPoolImpl(const QString &shortName)
    : ioc(1)
    , ssl(boost::asio::ssl::context::tls_client)
    , work(this->ioc.get_executor())
{
    boost::system::error_code ec;
    this->ssl.set_options(
        boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 |
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::single_dh_use,
        ec);
    if (ec)
    {
        qCWarning(chatterinoWebsocket) << "Failed to set SSL context options"
                                       << QString::fromStdString(ec.message());
    }

#ifdef CHATTERINO_WITH_TESTS
    if (!getApp()->isTest())
#endif
    {
        this->ssl.set_verify_mode(
            boost::asio::ssl::verify_peer |
            boost::asio::ssl::verify_fail_if_no_peer_cert);
        boost::system::error_code verifyPathsError;
        this->ssl.set_default_verify_paths(verifyPathsError);
        if (verifyPathsError)
        {
            qCWarning(chatterinoWebsocket) << "Failed to load default CA paths";
        }
#ifdef Q_OS_LINUX
        loadLinuxSystemCaCertificates(this->ssl);
#endif

        boost::certify::enable_native_https_server_verification(this->ssl);
    }

    this->ioThread = std::make_unique<std::thread>([this] {
        this->ioc.run();
        this->shutdownFlag.set();
    });

    auto threadName = [&]() -> QString {
        if (shortName.isEmpty())
        {
            return "WebSocketPool";
        }
        return "WS-" % shortName;
    }();
    renameThread(*this->ioThread, threadName);
}

WebSocketPoolImpl::~WebSocketPoolImpl()
{
    assert(this->closing);

    if (!this->tryShutdown(std::chrono::seconds{10}))
    {
        this->ioc.stop();
        this->ioThread->join();
    }
}

bool WebSocketPoolImpl::tryShutdown(std::chrono::milliseconds timeout)
{
    this->closing = true;
    if (!this->ioThread || !this->ioThread->joinable())
    {
        return true;
    }

    this->work.reset();
    {
        std::lock_guard g(this->connectionMutex);
        for (const auto &conn : this->connections)
        {
            conn->close();
        }
    }

    if (!this->shutdownFlag.waitFor(timeout))
    {
        qCWarning(chatterinoWebsocket)
            << "Failed to gracefully close all connections in time";
        return false;
    }

    if (this->ioThread->joinable())
    {
        this->ioThread->join();
    }
    return true;
}

void WebSocketPoolImpl::removeConnection(WebSocketConnection *conn)
{
    std::lock_guard g(this->connectionMutex);
    std::erase_if(this->connections, [conn](const auto &v) {
        return v.get() == conn;
    });
}

}
