#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <memory>

namespace chatterino::recording {

class Writer final : public QObject
{
    Q_OBJECT
public:
    explicit Writer(QString recoveryRoot);
    ~Writer() override;
    void prepare(const QString &group, const QJsonArray &files);
    void append(const QString &id, const QByteArray &record);
    void updateMetadata(const QString &id, const QJsonObject &metadata);
    void finish(const QString &id, const QJsonObject &metadata,
                bool immediately = false);
    void abort(const QString &id);
    void finishAll();
    void scanRecovery();
    void recover(const QStringList &ids);

Q_SIGNALS:
    void prepared(QString group, QString error);
    void failed(QString id, QString error);
    void finished(QString id, QString path, qint64 messages,
                  qint64 missingImages, QString error);
    void recoverable(QStringList ids);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

QString recordingFileName(const QJsonObject &metadata,
                          const QJsonArray &sources);

}
