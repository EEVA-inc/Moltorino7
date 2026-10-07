#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QMap>
#include <QString>

#include <memory>

namespace chatterino::tiktok::ticketguard {
using namespace Qt::StringLiterals;
using Headers = QMap<QByteArray, QByteArray>;

enum class EndpointRole {
    Consumer,
    Provider,
    ProviderAndConsumer,
};

class TicketGuard
{
public:
    static constexpr qsizetype MaxExportBytes = 65536;
    static constexpr qsizetype MaxPemBytes = 8192;
    static constexpr qsizetype MaxTicketBytes = 4096;
    static constexpr qsizetype MaxTsSignBytes = 8192;
    static constexpr qsizetype MaxServerHeaderBytes = 32768;
    static constexpr qsizetype MaxUrlBytes = 8192;

    static TicketGuard importStorage(const QByteArray &exportedJson,
                                     const QString &scene = u"tt_fetch"_s,
                                     const QString &storageNamespace = {});

    Headers headersForRequest(const QByteArray &absoluteUrl,
                              qint64 timestampSeconds,
                              EndpointRole role = EndpointRole::Consumer) const;

    bool applyServerData(const QByteArray &base64Header, EndpointRole role);

    QByteArray publicPointBase64() const;

private:
    struct KeyMaterial;
    std::shared_ptr<KeyMaterial> keys_;
    QString certificate_;
    QString csr_;
    QString ticket_;
    QJsonValue tsSign_ = QJsonValue(QJsonValue::Undefined);
    bool hasStorageNamespace_ = false;
};
}
