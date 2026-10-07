#include "controllers/recording/ChatRecordingWriter.hpp"

#include "controllers/recording/ChatRecordingMessage.hpp"

#include <QBuffer>
#include <QCache>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace chatterino::recording {
namespace {
constexpr qsizetype MAX_IMAGE_BYTES = 8 * 1024 * 1024;

QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject object(const QByteArray &bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

QString recordKey(const QJsonObject &source, const QString &id,
                  const QString &kind)
{
    return stableKey({{"source", source.value("key")},
                      {"broadcast", source.value("broadcastId")},
                      {"id", id},
                      {"kind", kind}});
}

QString platformBadgeId(const QString &platform)
{
    if (platform == "twitch" || platform == "youtube" || platform == "kick")
    {
        return "moltorino_platform_" + platform;
    }
    return {};
}

QJsonArray platformBadges(const QSet<QString> &platforms)
{
    QJsonArray badges;
    if (platforms.size() < 2)
    {
        return badges;
    }

    for (const auto &platform :
         {QString("twitch"), QString("youtube"), QString("kick")})
    {
        if (!platforms.contains(platform))
        {
            continue;
        }
        QFile image(":/badges/platform-" + platform + "-36.webp");
        if (!image.open(QIODevice::ReadOnly))
        {
            throw std::runtime_error("A built in recording badge is missing.");
        }
        badges.append(QJsonObject{
            {"name", platformBadgeId(platform)},
            {"versions",
             QJsonObject{
                 {"1",
                  QJsonObject{{"title", platform},
                              {"description", platform},
                              {"bytes", QString::fromLatin1(
                                            image.readAll().toBase64())}}}}}});
    }
    return badges;
}

QString safePart(QString value, int limit)
{
    value.replace(QRegularExpression(R"([\x00-\x1f<>:"/\\|?*])"), "_");
    value = value.trimmed();
    while (value.endsWith('.') || value.endsWith(' '))
    {
        value.chop(1);
    }
    if (value.isEmpty())
    {
        value = "chat";
    }
    if (value.size() > limit)
    {
        value =
            value.left(limit - 9) + "_" + stableKey({{"name", value}}).left(8);
    }

    static const QRegularExpression reserved(
        R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$)",
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(value).hasMatch())
    {
        value.prepend('_');
    }
    return value;
}

QString timestamp(const QJsonValue &value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs)
        .toUTC()
        .toString("yyyy-MM-dd_HH-mm-ss'Z'");
}

void write(QIODevice &file, const QByteArray &bytes)
{
    if (file.write(bytes) != bytes.size())
    {
        throw std::runtime_error(file.errorString().toStdString());
    }
}

void sql(QSqlQuery &query)
{
    if (!query.exec())
    {
        throw std::runtime_error(query.lastError().text().toStdString());
    }
}

void sql(QSqlDatabase &db, const QString &statement)
{
    QSqlQuery query(db);
    if (!query.exec(statement))
    {
        throw std::runtime_error(query.lastError().text().toStdString());
    }
}

bool next(QSqlQuery &query)
{
    const bool row = query.next();

    if (!row && query.lastError().isValid())
    {
        throw std::runtime_error(query.lastError().text().toStdString());
    }
    return row;
}

bool trustedImage(const QUrl &url)
{
    if (url.scheme() != "https" || !url.userInfo().isEmpty() ||
        (url.port(-1) != -1 && url.port() != 443))
    {
        return false;
    }
    const auto host = url.host().toLower();
    if (host == "bluzyrino-badge-registry.blu901-55.workers.dev" &&
        !url.hasFragment() && url.path().startsWith("/badges/"))
    {
        return true;
    }
    for (const auto &domain :
         {"jtvnw.net", "twitchcdn.net", "7tv.app", "7tv.io", "betterttv.net",
          "frankerfacez.com", "ffzap.com", "kick.com", "kickstatic.com",
          "ytimg.com", "ggpht.com", "googleusercontent.com", "homies.tv",
          "chatterino.com", "moltorino.com"})
    {
        if (host == QLatin1String(domain) ||
            host.endsWith("." + QString::fromLatin1(domain)))
        {
            return true;
        }
    }
    return false;
}

QByteArray oneLine(QString value)
{
    value.replace('\\', "\\\\");
    value.replace('\r', "\\r");
    value.replace('\n', "\\n");
    value.replace('\t', "\\t");
    value.replace(QRegularExpression("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f]"),
                  " ");
    return value.toUtf8();
}

QString fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
    {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}
}

QString recordingFileName(const QJsonObject &metadata,
                          const QJsonArray &sources)
{
    QStringList names;
    for (const auto &value : sources)
    {
        const auto source = value.toObject();
        names.append(source.value("platform").toString() + "_" +
                     source.value("login").toString());
    }
    names.removeDuplicates();
    const auto format = metadata.value("format").toString();
    return QString("%1_p%2_%3_%4_to_%5_%6.%7")
        .arg(safePart(metadata.value("tabTitle").toString(), 35))
        .arg(metadata.value("paneNumber").toInt(1))
        .arg(safePart(names.join('+'), 60))
        .arg(timestamp(metadata.value("startedAt")))
        .arg(timestamp(metadata.value("endedAt")))
        .arg(safePart(metadata.value("id").toString(), 12))
        .arg(format == "text" ? "txt" : "json");
}

struct Writer::Impl {
    struct Session {
        QString id;
        QString directory;
        QString connection;
        QJsonObject metadata;
        QSqlDatabase db;
        std::unique_ptr<QLockFile> lock;
        qint64 messages = 0;
        qint64 committedMessages = 0;
        int pending = 0;
        bool stopping = false;
        bool broken = false;
        bool transaction = false;
        bool hasAliases = false;
        std::unique_ptr<QSqlQuery> metadataQuery;
        std::unique_ptr<QSqlQuery> sourceQuery;
        std::unique_ptr<QSqlQuery> recordQuery;
        std::unique_ptr<QSqlQuery> assetQuery;
        std::unique_ptr<QSqlQuery> imageQuery;

        QCache<QString, QJsonObject> knownSources{32};
        QCache<QString, bool> knownAssets{128};
        QCache<QString, bool> imageAvailability{128};

        ~Session()
        {
            closeDatabase();
        }

        void closeDatabase()
        {
            metadataQuery.reset();
            sourceQuery.reset();
            recordQuery.reset();
            assetQuery.reset();
            imageQuery.reset();
            knownSources.clear();
            knownAssets.clear();
            imageAvailability.clear();
            db.close();
            db = {};
            if (!connection.isEmpty())
            {
                QSqlDatabase::removeDatabase(connection);
                connection.clear();
            }
        }

        QSqlQuery &statement(std::unique_ptr<QSqlQuery> &cached,
                             const char *text)
        {
            if (!cached)
            {
                cached = std::make_unique<QSqlQuery>(db);
                if (!cached->prepare(QString::fromLatin1(text)))
                {
                    throw std::runtime_error(
                        cached->lastError().text().toStdString());
                }
            }
            return *cached;
        }

        void openDatabase()
        {
            connection = "recording_" +
                         QUuid::createUuid().toString(QUuid::WithoutBraces);
            db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(QDir(directory).filePath("journal.sqlite"));
            if (!db.open())
            {
                throw std::runtime_error(db.lastError().text().toStdString());
            }
            sql(db, "PRAGMA journal_mode=WAL");
            sql(db, "PRAGMA synchronous=FULL");
            sql(db, "PRAGMA cache_size=-256");
            sql(db, "PRAGMA busy_timeout=1000");
        }

        void commit()
        {
            if (transaction)
            {
                saveMetadata();
                if (!db.commit())
                {
                    throw std::runtime_error(
                        db.lastError().text().toStdString());
                }
            }
            transaction = false;
            pending = 0;
            committedMessages = messages;
        }

        void begin()
        {
            if (!transaction && !db.transaction())
            {
                throw std::runtime_error(db.lastError().text().toStdString());
            }
            transaction = true;
        }

        void saveMetadata()
        {
            auto &query = statement(metadataQuery,
                                    "INSERT OR REPLACE INTO metadata(key,data) "
                                    "VALUES('recording',?)");
            query.bindValue(0, json(metadata));
            sql(query);
            query.finish();
        }

        QJsonArray sources()
        {
            QJsonArray result;
            QSqlQuery query(db);
            query.prepare("SELECT data FROM sources ORDER BY key");
            sql(query);
            while (next(query))
            {
                result.append(object(query.value(0).toByteArray()));
            }
            return result;
        }

        void addSource(const QJsonObject &source)
        {
            if (source.isEmpty())
            {
                return;
            }
            const auto key = source.value("key").toString() + ":" +
                             source.value("broadcastId").toString();
            if (const auto *known = knownSources.object(key);
                known && *known == source)
            {
                return;
            }
            auto &query = statement(
                sourceQuery,
                "INSERT OR REPLACE INTO sources(key,data) VALUES(?,?)");
            query.bindValue(0, key);
            query.bindValue(1, json(source));
            sql(query);
            query.finish();
            knownSources.insert(key, new QJsonObject(source));
        }

        bool addAsset(const QJsonObject &asset)
        {
            const auto id = asset.value("id").toString();
            if (knownAssets.object(id))
            {
                return false;
            }
            auto &query = statement(
                assetQuery,
                "INSERT OR IGNORE INTO assets(id,data,status) VALUES(?,?,?)");
            query.bindValue(0, id);
            query.bindValue(1, json(asset));
            query.bindValue(2, metadata.value("embedImages").toBool() ? 0 : -1);
            sql(query);
            const bool inserted = query.numRowsAffected() > 0;
            query.finish();
            knownAssets.insert(id, new bool(true));
            return inserted;
        }

        bool storeRecord(QJsonObject record)
        {
            const auto source = record.value("source").toObject();
            const auto id = record.value("id").toString();
            const auto kind = record.value("recordType").toString();
            const auto key =
                id.isEmpty()
                    ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                    : recordKey(source, id, kind);
            const bool youtube = source.value("platform") == "youtube";
            const auto details = record.value("metadata").toObject();
            if (youtube && details.value("replacesExisting").toBool())
            {
                QSqlQuery query(db);
                auto target = details.value("targetMessageId").toString();
                if (target.isEmpty())
                {
                    target = id;
                }
                QStringList targets;
                if (!target.isEmpty())
                {
                    targets = {recordKey(source, target, "message"),
                               recordKey(source, target, "event")};
                }
                if (!id.isEmpty() && target != id)
                {
                    targets.append(key);
                }
                for (const auto &targetKey : targets)
                {
                    query.prepare(
                        "SELECT seq,kind,data,event_key FROM records WHERE "
                        "event_key=? UNION ALL SELECT r.seq,r.kind,r.data,"
                        "r.event_key FROM record_aliases a JOIN records r ON "
                        "r.seq=a.seq WHERE a.key=? LIMIT 1");
                    query.addBindValue(targetKey);
                    query.addBindValue(targetKey);
                    sql(query);
                    if (!next(query))
                    {
                        continue;
                    }
                    const auto seq = query.value(0).toLongLong();
                    const auto oldKind = query.value(1).toString();
                    const auto previous = object(query.value(2).toByteArray());
                    const auto originalKey = query.value(3).toString();
                    query.finish();
                    record.insert("correctedAt", record.value("receivedAt"));
                    record.insert("correctionOffsetSeconds",
                                  record.value("offsetSeconds"));
                    for (const auto &field : {"receivedAt", "offsetSeconds"})
                    {
                        if (previous.contains(field))
                        {
                            record.insert(field, previous.value(field));
                        }
                    }
                    if (record.value("createdAt").toString().isEmpty())
                    {
                        record.insert("createdAt", previous.value("createdAt"));
                    }
                    record.insert("eventKey", originalKey);
                    query.prepare(
                        "UPDATE records SET kind=?,data=? WHERE seq=?");
                    query.addBindValue(kind);
                    query.addBindValue(json(record));
                    query.addBindValue(seq);
                    sql(query);
                    if (key != originalKey)
                    {
                        query.prepare("INSERT OR IGNORE INTO record_aliases"
                                      "(key,seq) VALUES(?,?)");
                        query.addBindValue(key);
                        query.addBindValue(seq);
                        sql(query);
                        hasAliases = true;
                    }
                    messages += static_cast<int>(kind == "message") -
                                static_cast<int>(oldKind == "message");
                    return true;
                }
            }
            if (youtube && hasAliases)
            {
                QSqlQuery query(db);
                query.prepare("SELECT 1 FROM record_aliases WHERE key=?");
                query.addBindValue(key);
                sql(query);
                if (next(query))
                {
                    return false;
                }
                query.finish();
            }
            record.insert("eventKey", key);
            auto &query =
                statement(recordQuery,
                          "INSERT OR IGNORE INTO records(event_key,kind,data) "
                          "VALUES(?,?,?)");
            query.bindValue(0, key);
            query.bindValue(1, kind);
            const auto bytes = json(record);
            query.bindValue(2, bytes);
            sql(query);
            const bool inserted = query.numRowsAffected() > 0;
            query.finish();

            if (bytes.size() > 64 * 1024)
            {
                recordQuery.reset();
            }
            if (!inserted)
            {
                return false;
            }
            messages += kind == "message";
            return true;
        }
    };

    struct Download {
        QString session;
        QString asset;
        QByteArray bytes;
    };

    Writer *owner;
    QString root;
    std::map<QString, std::unique_ptr<Session>> sessions;
    std::map<QNetworkReply *, Download> downloads;
    QNetworkAccessManager *network = nullptr;
    QTimer *flushTimer = nullptr;
    QThreadPool exports;

    explicit Impl(Writer *owner, QString root)
        : owner(owner)
        , root(std::move(root))
        , exports(owner)
    {
        exports.setMaxThreadCount(1);
        exports.setObjectName("Chat recording export");
    }

    ~Impl()
    {
        exports.waitForDone();
    }

    void initialize()
    {
        if (flushTimer)
        {
            return;
        }
        flushTimer = new QTimer(owner);
        flushTimer->setInterval(500);
        flushTimer->setSingleShot(true);
        QObject::connect(flushTimer, &QTimer::timeout, owner, [this] {
            for (auto &[id, session] : sessions)
            {
                if (!session->broken && session->pending)
                {
                    try
                    {
                        session->commit();
                    }
                    catch (const std::exception &error)
                    {
                        fail(*session, QString::fromUtf8(error.what()));
                    }
                }
            }
        });
    }

    void scheduleFlush()
    {
        initialize();
        if (!flushTimer->isActive())
        {
            flushTimer->start();
        }
    }

    std::unique_ptr<Session> open(const QString &id, bool fresh)
    {
        if (id.isEmpty() || id.contains('/') || id.contains('\\') ||
            id.contains(".."))
        {
            throw std::runtime_error("Invalid recording ID.");
        }
        auto session = std::make_unique<Session>();
        session->id = id;
        session->directory = QDir(root).filePath(id);
        if (fresh && !QDir().mkpath(session->directory))
        {
            throw std::runtime_error(
                "The local recording recovery folder could not be created.");
        }
        session->lock = std::make_unique<QLockFile>(
            QDir(session->directory).filePath("recording.lock"));
        session->lock->setStaleLockTime(0);
        if (!session->lock->tryLock())
        {
            throw std::runtime_error(
                "This recording is open in another Moltorino process.");
        }
        session->openDatabase();
        if (fresh)
        {
            sql(session->db, "CREATE TABLE metadata(key TEXT PRIMARY KEY,data "
                             "BLOB NOT NULL)");
            sql(session->db,
                "CREATE TABLE records(seq INTEGER PRIMARY KEY,event_key TEXT "
                "UNIQUE,kind TEXT NOT NULL,data BLOB NOT NULL)");
            sql(session->db, "CREATE TABLE record_aliases(key TEXT PRIMARY KEY,"
                             "seq INTEGER NOT NULL)");
            sql(session->db, "CREATE TABLE sources(key TEXT PRIMARY KEY,data "
                             "BLOB NOT NULL)");
            sql(session->db,
                "CREATE TABLE assets(id TEXT PRIMARY KEY,data BLOB NOT "
                "NULL,bytes BLOB,status INTEGER NOT NULL DEFAULT 0)");
            sql(session->db, "CREATE INDEX asset_status ON assets(status)");
        }
        else
        {
            QSqlQuery query(session->db);
            query.prepare("SELECT data FROM metadata WHERE key='recording'");
            sql(query);
            const auto metadata =
                next(query) ? object(query.value(0).toByteArray()) : QJsonObject{};
            if (metadata.isEmpty())
            {
                throw std::runtime_error(
                    "The recording recovery metadata is unreadable.");
            }
            session->metadata = metadata;
            query.prepare("SELECT COUNT(*) FROM records WHERE kind='message'");
            sql(query);
            next(query);
            session->messages = query.value(0).toLongLong();
            session->committedMessages = session->messages;

            sql(session->db, "UPDATE assets SET status=0 WHERE status=1");
        }
        return session;
    }

    void fail(Session &session, const QString &error)
    {
        if (session.broken)
        {
            return;
        }
        session.broken = true;
        if (session.transaction)
        {
            session.db.rollback();
            session.transaction = false;
        }
        Q_EMIT owner->failed(session.id, error);
    }

    void imageDone(const QString &id, const QString &assetId, QByteArray bytes)
    {
        const auto it = sessions.find(id);
        if (it == sessions.end() || it->second->broken)
        {
            return;
        }
        auto &session = *it->second;
        try
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto size = reader.size();
            const auto format = reader.format().toLower();
            if (bytes.size() > MAX_IMAGE_BYTES || size.width() <= 0 ||
                size.height() <= 0 || size.width() > 4096 ||
                size.height() > 4096 ||
                (format != "png" && format != "gif" && format != "webp" &&
                 format != "jpeg" && format != "jpg"))
            {
                bytes.clear();
            }
            session.begin();
            QSqlQuery query(session.db);
            query.prepare("UPDATE assets SET bytes=?,status=? WHERE id=?");
            query.addBindValue(bytes);
            query.addBindValue(bytes.isEmpty() ? 3 : 2);
            query.addBindValue(assetId);
            sql(query);
            ++session.pending;
            scheduleFlush();
        }
        catch (const std::exception &error)
        {
            fail(session, QString::fromUtf8(error.what()));
        }
    }

    void pumpImages()
    {
        initialize();
        for (auto &[id, session] : sessions)
        {
            if (session->broken ||
                !session->metadata.value("embedImages").toBool())
            {
                continue;
            }
            while (!session->broken && downloads.size() < 3)
            {
                QSqlQuery query(session->db);
                query.prepare(
                    "SELECT id,data FROM assets WHERE status=0 LIMIT 1");
                if (!query.exec() || !query.next())
                {
                    if (query.lastError().isValid())
                    {
                        fail(*session, query.lastError().text());
                    }
                    break;
                }
                const auto assetId = query.value(0).toString();
                const auto data = object(query.value(1).toByteArray());
                query.finish();
                query.prepare("UPDATE assets SET status=1 WHERE id=?");
                query.addBindValue(assetId);
                if (!query.exec())
                {
                    fail(*session, query.lastError().text());
                    break;
                }
                const auto urlString = data.value("url").toString();
                if (urlString.startsWith(":/"))
                {
                    QFile resource(urlString);
                    QByteArray bytes;
                    if (resource.open(QIODevice::ReadOnly) &&
                        resource.size() <= MAX_IMAGE_BYTES)
                    {
                        bytes = resource.readAll();

                        if (urlString.endsWith(".svg", Qt::CaseInsensitive))
                        {
                            const auto image = QImage::fromData(bytes).scaled(
                                72, 72, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
                            bytes.clear();
                            QBuffer encoded(&bytes);
                            encoded.open(QIODevice::WriteOnly);
                            image.save(&encoded, "PNG");
                        }
                    }
                    imageDone(id, assetId, bytes);
                    continue;
                }
                const QUrl url(urlString);
                if (!trustedImage(url))
                {
                    imageDone(id, assetId, {});
                    continue;
                }
                QNetworkRequest request(url);
                request.setTransferTimeout(10000);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                     QNetworkRequest::ManualRedirectPolicy);
                if (!network)
                {
                    network = new QNetworkAccessManager(owner);
                }
                auto *reply = network->get(request);
                reply->setReadBufferSize(64 * 1024);
                downloads.emplace(reply, Download{id, assetId, {}});
                QObject::connect(
                    reply, &QNetworkReply::readyRead, owner, [this, reply] {
                        auto it = downloads.find(reply);
                        if (it == downloads.end())
                        {
                            return;
                        }
                        auto &bytes = it->second.bytes;
                        bytes.append(
                            reply->read(MAX_IMAGE_BYTES + 1 - bytes.size()));
                        if (bytes.size() > MAX_IMAGE_BYTES)
                        {
                            reply->abort();
                        }
                    });
                QObject::connect(
                    reply, &QNetworkReply::finished, owner, [this, reply] {
                        auto it = downloads.find(reply);
                        if (it == downloads.end())
                        {
                            return;
                        }
                        auto download = std::move(it->second);
                        downloads.erase(it);
                        download.bytes.append(reply->read(
                            MAX_IMAGE_BYTES + 1 - download.bytes.size()));
                        const auto status =
                            reply
                                ->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute)
                                .toInt();
                        if (reply->error() != QNetworkReply::NoError ||
                            status != 200)
                        {
                            download.bytes.clear();
                        }
                        reply->deleteLater();
                        imageDone(download.session, download.asset,
                                  std::move(download.bytes));
                        pumpImages();
                    });
            }
        }
    }

    void cancelImages(const QString &id)
    {
        for (auto it = downloads.begin(); it != downloads.end();)
        {
            if (it->second.session == id)
            {
                auto *reply = it->first;
                it = downloads.erase(it);
                reply->disconnect(owner);
                reply->abort();
                reply->deleteLater();
            }
            else
            {
                ++it;
            }
        }
    }

    static bool hasImage(Session &session, const QString &id)
    {
        if (const auto *known = session.imageAvailability.object(id))
        {
            return *known;
        }
        auto &query = session.statement(session.imageQuery,
                                        "SELECT status FROM assets WHERE id=?");
        query.bindValue(0, id);
        sql(query);
        const bool available = next(query) && query.value(0).toInt() == 2;
        query.finish();
        session.imageAvailability.insert(id, new bool(available));
        return available;
    }

    static QJsonObject comment(Session &session, const QJsonObject &record,
                               bool showPlatformBadges)
    {
        const auto source = record.value("source").toObject();
        const auto author = record.value("author").toObject();
        const auto details = record.value("metadata").toObject();
        const auto body = record.value("text").toString();
        QJsonArray fragments;
        QJsonArray badges;
        auto displayName = author.value("displayName").toString();
        if (displayName.isEmpty())
        {
            displayName = author.value("login").toString();
        }
        const auto amount = details.value("amount").toString();
        if (!amount.isEmpty())
        {
            fragments.append(
                QJsonObject{{"text", "Super Chat " + amount + ": "},
                            {"emoticon", QJsonValue::Null}});
        }
        int cursor = 0;
        for (const auto &value : record.value("spans").toArray())
        {
            const auto span = value.toObject();
            const int start = span.value("start").toInt();
            const int length = span.value("length").toInt();
            const auto assetId = span.value("assetId").toString();
            if (start < cursor || length <= 0 || start > body.size() ||
                length > body.size() - start)
            {
                continue;
            }
            if (start > cursor)
            {
                fragments.append(
                    QJsonObject{{"text", body.mid(cursor, start - cursor)},
                                {"emoticon", QJsonValue::Null}});
            }
            QJsonObject assetData;
            for (const auto &candidate : record.value("assets").toArray())
            {
                if (candidate.toObject().value("id") == assetId)
                {
                    assetData = candidate.toObject();
                    break;
                }
            }
            auto fragmentText = body.mid(start, length);
            QJsonValue emoticon(QJsonValue::Null);
            if (hasImage(session, assetId))
            {
                if (assetData.value("zeroWidth").toBool())
                {
                    fragmentText = assetId;
                }
                else
                {
                    emoticon = QJsonObject{{"emoticon_id", assetId}};
                }
            }
            else if (!session.metadata.value("embedImages").toBool() &&
                     source.value("platform") == "twitch" &&
                     assetData.value("url").toString().contains(
                         "static-cdn.jtvnw.net/emoticons/"))
            {
                emoticon =
                    QJsonObject{{"emoticon_id", assetData.value("providerId")}};
            }
            fragments.append(
                QJsonObject{{"text", fragmentText}, {"emoticon", emoticon}});
            cursor = start + length;
        }
        if (cursor < body.size() || fragments.isEmpty())
        {
            fragments.append(QJsonObject{{"text", body.mid(cursor)},
                                         {"emoticon", QJsonValue::Null}});
        }
        for (const auto &badge : record.value("badges").toArray())
        {
            if (hasImage(session, badge.toString()))
            {
                badges.append(QJsonObject{{"_id", badge}, {"version", "1"}});
            }
        }
        if (!session.metadata.value("embedImages").toBool() &&
            source.value("platform") == "twitch")
        {
            badges = record.value("nativeBadges").toArray();
        }
        if (showPlatformBadges)
        {
            const auto platformId =
                platformBadgeId(source.value("platform").toString());
            if (!platformId.isEmpty())
            {
                badges.prepend(
                    QJsonObject{{"_id", platformId}, {"version", "1"}});
            }
        }
        QJsonObject extension = record;

        extension.remove("assets");
        const QColor usernameColor(author.value("color").toString());
        auto createdAt = record.value("createdAt").toString();
        if (!QDateTime::fromString(createdAt, Qt::ISODateWithMs).isValid())
        {
            createdAt = record.value("receivedAt").toString();
        }
        return {
            {"_id", record.value("eventKey")},
            {"created_at", createdAt},
            {"channel_id", source.value("id")},
            {"content_type", "video"},
            {"content_id", QJsonValue::Null},
            {"content_offset_seconds", record.value("offsetSeconds")},
            {"commenter", QJsonObject{{"_id", author.value("id")},
                                      {"name", author.value("login")},
                                      {"display_name", displayName}}},
            {"message",
             QJsonObject{{"body", body},
                         {"fragments", fragments},
                         {"user_badges", badges},

                         {"user_color",
                          usernameColor.isValid()
                              ? QJsonValue(usernameColor.name(QColor::HexRgb))
                              : QJsonValue(QJsonValue::Null)},
                         {"bits_spent", details.value("bits").toInt()},
                         {"emoticons", QJsonArray{}}}},
            {"moltorino", extension}};
    }

    static void writeRecordArray(Session &session, QIODevice &out,
                                 const QString &kind, bool twitchDownloader,
                                 bool showPlatformBadges)
    {
        QSqlQuery query(session.db);
        query.setForwardOnly(true);
        query.prepare("SELECT data FROM records WHERE kind=? ORDER BY seq");
        query.addBindValue(kind);
        sql(query);
        bool first = true;
        write(out, "[");
        while (next(query))
        {
            if (!first)
            {
                write(out, ",");
            }
            first = false;
            const auto record = object(query.value(0).toByteArray());
            write(out, json(twitchDownloader
                                ? comment(session, record, showPlatformBadges)
                                : record));
        }
        write(out, "]");
    }

    static void embedded(Session &session, QIODevice &out, const QString &type,
                         bool zeroWidth, const QJsonArray &extra = {})
    {
        QSqlQuery query(session.db);
        query.setForwardOnly(true);
        query.prepare(
            "SELECT data,bytes FROM assets WHERE status=2 ORDER BY id");
        sql(query);
        bool first = true;
        write(out, "[");
        while (next(query))
        {
            const auto data = object(query.value(0).toByteArray());
            if (data.value("type") != type ||
                (type == "emote" &&
                 data.value("zeroWidth").toBool() != zeroWidth))
            {
                continue;
            }
            const auto bytes = query.value(1).toByteArray();
            QJsonObject entry;
            if (type == "badge")
            {
                entry = {{"name", data.value("id")},
                         {"versions",
                          QJsonObject{
                              {"1", QJsonObject{
                                        {"title", data.value("name")},
                                        {"description", data.value("name")},
                                        {"bytes", QString::fromLatin1(
                                                      bytes.toBase64())}}}}}};
            }
            else
            {
                QBuffer buffer;
                buffer.setData(bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                const auto size = reader.size();
                entry = {
                    {"id", data.value("id")},
                    {"name", zeroWidth ? data.value("id") : data.value("name")},
                    {"data", QString::fromLatin1(bytes.toBase64())},
                    {"width", size.width()},
                    {"height", size.height()},
                    {"imageScale",
                     data.contains("logicalHeight")
                         ? std::max(
                               1, static_cast<int>(std::lround(
                                      size.height() /
                                      std::max(1.0, data.value("logicalHeight")
                                                        .toDouble(28)))))
                         : data.value("imageScale").toInt(1)},
                    {"isZeroWidth", zeroWidth}};
            }
            if (!first)
            {
                write(out, ",");
            }
            first = false;
            write(out, json(entry));
        }
        for (const auto &entry : extra)
        {
            if (!first)
            {
                write(out, ",");
            }
            first = false;
            write(out, json(entry.toObject()));
        }
        write(out, "]");
    }

    static void assetDefinitions(Session &session, QIODevice &out)
    {
        QSqlQuery query(session.db);
        query.setForwardOnly(true);
        query.prepare("SELECT data FROM assets ORDER BY id");
        sql(query);
        bool first = true;
        write(out, "[");
        while (next(query))
        {
            if (!first)
            {
                write(out, ",");
            }
            first = false;
            write(out, query.value(0).toByteArray());
        }
        write(out, "]");
    }

    void publish(const QString &id)
    {
        const auto it = sessions.find(id);
        if (it == sessions.end())
        {
            return;
        }
        cancelImages(id);
        auto &session = *it->second;
        if (!session.broken)
        {
            try
            {
                session.saveMetadata();
                session.commit();
            }
            catch (const std::exception &error)
            {
                fail(session, QString::fromUtf8(error.what()));
            }
        }

        session.closeDatabase();
        std::shared_ptr<Session> exporting = std::move(it->second);
        sessions.erase(it);
        if (sessions.empty() && flushTimer)
        {
            flushTimer->stop();
        }
        exports.start([exporting = std::move(exporting), owner = owner] {
            publishSession(*exporting, owner);
        });
        pumpImages();
    }

    static void publishSession(Session &session, Writer *owner)
    {
#ifdef Q_OS_WIN

        const bool background =
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto restorePriority = qScopeGuard([background] {
            if (background)
            {
                SetThreadPriority(GetCurrentThread(),
                                  THREAD_MODE_BACKGROUND_END);
            }
        });
#endif
        const auto id = session.id;
        QString path;
        QString error;
        qint64 missing = 0;
        try
        {
            if (session.broken)
            {
                throw std::runtime_error(
                    QString("Recording stopped after a storage error. "
                            "%1 messages were saved for recovery; up to "
                            "%2 more messages may be missing.")
                        .arg(session.committedMessages)
                        .arg(std::max<qint64>(
                            0, session.metadata.value("acceptedMessages")
                                       .toInteger(session.messages) -
                                   session.committedMessages))
                        .toStdString());
            }
            session.openDatabase();
            const auto published =
                session.metadata.value("publishedPath").toString();
            const auto publishedDigest =
                session.metadata.value("publishedSha256").toString();
            if (!published.isEmpty() && !publishedDigest.isEmpty() &&
                fileDigest(published) == publishedDigest)
            {
                const auto messages = session.messages;
                const auto directory = session.directory;
                session.closeDatabase();
                session.lock.reset();
                QDir(directory).removeRecursively();
                Q_EMIT owner->finished(id, published, messages, 0, {});
                return;
            }
            const auto sources = session.sources();
            const QDir destination(session.metadata.value("folder").toString());
            path = destination.filePath(
                recordingFileName(session.metadata, sources));
            const auto staging =
                destination.filePath("." + id + ".recording.tmp");
            QSaveFile file(staging);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly))
            {
                throw std::runtime_error(file.errorString().toStdString());
            }
            QSqlQuery count(session.db);
            count.prepare(
                "SELECT COUNT(*) FROM assets WHERE status<>2 AND status<>-1");
            sql(count);
            next(count);
            missing = count.value(0).toLongLong();
            count.finish();
            session.metadata.insert("missingImages", missing);
            session.metadata.insert("sources", sources);
            const auto format = session.metadata.value("format").toString();
            if (format == "text")
            {
                write(file, "# Moltorino chat recording\n# " +
                                json(session.metadata) + "\n");
                QSqlQuery query(session.db);
                query.setForwardOnly(true);
                query.prepare("SELECT data FROM records ORDER BY seq");
                sql(query);
                while (next(query))
                {
                    const auto record = object(query.value(0).toByteArray());
                    const auto source = record.value("source").toObject();
                    const auto author = record.value("author").toObject();
                    write(file,
                          "[" + oneLine(record.value("receivedAt").toString()) +
                              "] [" +
                              oneLine(source.value("platform").toString() +
                                      "/" + source.value("login").toString()) +
                              "] " +
                              oneLine(author.value("displayName").toString()) +
                              (record.value("recordType") == "event"
                                   ? " [event]: "
                                   : ": ") +
                              oneLine(record.value("text").toString()) + "\n");
                }
            }
            else if (format == "basic")
            {
                write(file, "{\"schemaVersion\":1,\"recording\":" +
                                json(session.metadata) + ",\"messages\":");
                writeRecordArray(session, file, "message", false, false);
                write(file, ",\"events\":");
                writeRecordArray(session, file, "event", false, false);
                write(file, "}");
            }
            else
            {
                const double duration = std::max(
                    1.0,
                    std::ceil(
                        session.metadata.value("durationSeconds").toDouble()));
                QSet<QString> platforms;
                std::map<QString, QJsonObject> twitchSources;
                QSet<QString> unresolvedTwitchLogins;
                for (const auto &value : sources)
                {
                    const auto origin = value.toObject();
                    const auto platform = origin.value("platform").toString();
                    platforms.insert(platform);
                    if (platform == "twitch")
                    {
                        bool validId = false;
                        const int channelId =
                            origin.value("id").toString().toInt(&validId);
                        if (validId && channelId > 0)
                        {
                            twitchSources.emplace(QString::number(channelId),
                                                  origin);
                        }
                        else
                        {
                            unresolvedTwitchLogins.insert(
                                origin.value("login").toString().toLower());
                        }
                    }
                }

                for (const auto &[channelId, origin] : twitchSources)
                {
                    unresolvedTwitchLogins.remove(
                        origin.value("login").toString().toLower());
                }

                const bool singleTwitch = twitchSources.size() == 1 &&
                                          unresolvedTwitchLogins.isEmpty();
                const bool showPlatformBadges =
                    platforms.size() > 1 &&
                    session.metadata.value("showPlatformBadges").toBool(true);
                const auto source = singleTwitch ? twitchSources.begin()->second
                                                 : QJsonObject{};
                bool idOk = false;
                const int twitchId = source.value("id").toString().toInt(&idOk);
                QJsonObject header{
                    {"FileInfo",
                     QJsonObject{
                         {"Version",
                          QJsonObject{
                              {"Major", 1}, {"Minor", 4}, {"Patch", 0}}},
                         {"CreatedAt", session.metadata.value("startedAt")},
                         {"UpdatedAt", session.metadata.value("endedAt")}}},
                    {"streamer",
                     QJsonObject{
                         {"id", idOk ? twitchId : 0},
                         {"name", singleTwitch
                                      ? source.value("displayName")
                                      : session.metadata.value("tabTitle")},
                         {"login", singleTwitch
                                       ? source.value("login")
                                       : QJsonValue("moltorino_recording")}}},
                    {"video",
                     QJsonObject{
                         {"id", QJsonValue::Null},
                         {"title", session.metadata.value("tabTitle")},
                         {"created_at", session.metadata.value("startedAt")},
                         {"start", 0},
                         {"end", duration},
                         {"length", duration},
                         {"chapters", QJsonArray{}}}}};
                auto bytes = json(header);
                bytes.chop(1);
                write(file, bytes + ",\"comments\":");
                writeRecordArray(session, file, "message", true,
                                 showPlatformBadges);
                write(file, ",\"embeddedData\":{\"firstParty\":");
                embedded(session, file, "emote", false);
                write(file, ",\"thirdParty\":");
                embedded(session, file, "emote", true);
                write(file, ",\"twitchBadges\":");
                embedded(session, file, "badge", false,
                         showPlatformBadges ? platformBadges(platforms)
                                            : QJsonArray{});
                write(file, ",\"twitchBits\":[]},\"moltorino\":");
                bytes = json(session.metadata);
                bytes.chop(1);
                write(file, bytes + ",\"assets\":");
                assetDefinitions(session, file);
                write(file, ",\"events\":");
                writeRecordArray(session, file, "event", false, false);
                write(file, "}}");
            }
            if (!file.commit())
            {
                throw std::runtime_error(file.errorString().toStdString());
            }
            const auto digest = fileDigest(staging);
            if (digest.isEmpty())
            {
                throw std::runtime_error("Could not verify the completed "
                                         "recording before saving it.");
            }
            session.metadata.insert("publishedPath", path);
            session.metadata.insert("publishedSha256", digest);
            session.saveMetadata();
            session.commit();

            if (!QFile::rename(staging, path))
            {
                throw std::runtime_error(
                    "Could not save the completed recording to its final "
                    "location. Recovery data was kept.");
            }
        }
        catch (const std::exception &exception)
        {
            error = QString::fromUtf8(exception.what());
            path.clear();
        }
        const auto messages = session.messages;
        const auto directory = session.directory;
        session.closeDatabase();
        session.lock.reset();
        if (error.isEmpty())
        {
            QDir(directory).removeRecursively();
        }
        Q_EMIT owner->finished(id, path, messages, missing, error);
    }
};

Writer::Writer(QString recoveryRoot)
    : impl_(std::make_unique<Impl>(this, std::move(recoveryRoot)))
{
}

Writer::~Writer() = default;

void Writer::prepare(const QString &group, const QJsonArray &files)
{
    impl_->initialize();
    QStringList prepared;
    try
    {
        for (const auto &value : files)
        {
            const auto metadata = value.toObject();
            const auto id = metadata.value("id").toString();
            const QDir folder(metadata.value("folder").toString());
            if (!QDir().mkpath(folder.absolutePath()))
            {
                throw std::runtime_error(
                    "The recording output folder could not be created.");
            }
            QSaveFile probe(folder.filePath("." + id + ".write-check"));
            probe.setDirectWriteFallback(false);
            if (!probe.open(QIODevice::WriteOnly))
            {
                throw std::runtime_error(probe.errorString().toStdString());
            }
            probe.cancelWriting();
            auto session = impl_->open(id, true);
            session->metadata = metadata;
            session->saveMetadata();
            for (const auto &source : metadata.value("sources").toArray())
            {
                session->addSource(source.toObject());
            }
            impl_->sessions.emplace(id, std::move(session));
            prepared.append(id);
        }
        Q_EMIT this->prepared(group, {});
    }
    catch (const std::exception &error)
    {
        for (const auto &id : prepared)
        {
            this->abort(id);
        }
        Q_EMIT this->prepared(group, QString::fromUtf8(error.what()));
    }
}

void Writer::updateMetadata(const QString &id, const QJsonObject &metadata)
{
    const auto it = impl_->sessions.find(id);
    if (it == impl_->sessions.end() || it->second->broken)
    {
        return;
    }
    auto &session = *it->second;
    try
    {
        for (auto item = metadata.begin(); item != metadata.end(); ++item)
        {
            session.metadata.insert(item.key(), item.value());
        }
        session.saveMetadata();
        session.commit();
    }
    catch (const std::exception &error)
    {
        impl_->fail(session, QString::fromUtf8(error.what()));
    }
}

void Writer::append(const QString &id, const QByteArray &bytes)
{
    const auto it = impl_->sessions.find(id);
    if (it == impl_->sessions.end() || it->second->broken ||
        it->second->stopping)
    {
        return;
    }
    auto &session = *it->second;
    try
    {
        auto record = object(bytes);
        if (record.isEmpty())
        {
            throw std::runtime_error(
                "An invalid recording record was rejected.");
        }
        session.begin();
        const auto source = record.value("source").toObject();
        if (!session.storeRecord(record))
        {
            return;
        }
        if (record.contains("receivedAt"))
        {
            session.metadata.insert("lastReceivedAt",
                                    record.value("receivedAt"));
        }
        if (record.contains("offsetSeconds"))
        {
            session.metadata.insert("lastOffsetSeconds",
                                    record.value("offsetSeconds"));
        }
        session.addSource(source);
        bool newAssets = false;
        for (const auto &value : record.value("assets").toArray())
        {
            newAssets |= session.addAsset(value.toObject());
        }
        if (++session.pending >= 100)
        {
            session.commit();
        }
        else
        {
            impl_->scheduleFlush();
        }
        if (newAssets && session.metadata.value("embedImages").toBool())
        {
            impl_->pumpImages();
        }
    }
    catch (const std::exception &error)
    {
        impl_->fail(session, QString::fromUtf8(error.what()));
    }
}

void Writer::finish(const QString &id, const QJsonObject &metadata,
                    bool immediately)
{
    const auto it = impl_->sessions.find(id);
    if (it == impl_->sessions.end())
    {
        return;
    }
    auto &session = *it->second;
    for (auto item = metadata.begin(); item != metadata.end(); ++item)
    {
        session.metadata.insert(item.key(), item.value());
    }
    const bool wasStopping = session.stopping;
    session.stopping = true;
    bool imagesPending = false;
    if (!session.broken)
    {
        try
        {
            session.saveMetadata();
            session.commit();
            if (session.metadata.value("embedImages").toBool())
            {
                QSqlQuery query(session.db);
                query.prepare(
                    "SELECT 1 FROM assets WHERE status IN (0,1) LIMIT 1");
                sql(query);
                imagesPending = next(query);
            }
        }
        catch (const std::exception &error)
        {
            impl_->fail(session, QString::fromUtf8(error.what()));
        }
    }
    if (immediately || session.broken || !imagesPending)
    {
        impl_->publish(id);
    }
    else if (!wasStopping)
    {
        QTimer::singleShot(3000, this, [this, id] {
            impl_->publish(id);
        });
    }
}

void Writer::abort(const QString &id)
{
    const auto it = impl_->sessions.find(id);
    if (it != impl_->sessions.end() && it->second->messages == 0)
    {
        impl_->cancelImages(id);
        const auto directory = it->second->directory;
        impl_->sessions.erase(it);
        if (impl_->sessions.empty() && impl_->flushTimer)
        {
            impl_->flushTimer->stop();
        }
        QDir(directory).removeRecursively();
    }
}

void Writer::finishAll()
{
    QStringList ids;
    for (const auto &[id, session] : impl_->sessions)
    {
        ids.append(id);
    }
    for (const auto &id : ids)
    {
        this->finish(id, {}, true);
    }

    impl_->exports.waitForDone();
}

void Writer::scanRecovery()
{
    QStringList ids;
    for (const auto &id :
         QDir(impl_->root).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        const QDir directory(QDir(impl_->root).filePath(id));
        if (!QFile::exists(directory.filePath("journal.sqlite")))
        {
            continue;
        }
        QLockFile lock(directory.filePath("recording.lock"));
        lock.setStaleLockTime(0);
        if (lock.tryLock())
        {
            ids.append(id);
        }
    }
    if (!ids.isEmpty())
    {
        Q_EMIT recoverable(ids);
    }
}

void Writer::recover(const QStringList &ids)
{
    for (const auto &id : ids)
    {
        try
        {
            auto session = impl_->open(id, false);
            if (session->metadata.value("endedAt").toString().isEmpty())
            {
                QSqlQuery query(session->db);
                query.prepare(
                    "SELECT data FROM records ORDER BY seq DESC LIMIT 1");
                sql(query);
                const auto last = next(query)
                                      ? object(query.value(0).toByteArray())
                                      : QJsonObject{};
                session->metadata.insert(
                    "endedAt",
                    session->metadata.value("lastReceivedAt")
                        .toString(
                            last.value("receivedAt")
                                .toString(session->metadata.value("startedAt")
                                              .toString())));
                session->metadata.insert(
                    "durationSeconds",
                    session->metadata.value("lastOffsetSeconds")
                        .toDouble(last.value("offsetSeconds").toDouble()));
                session->metadata.insert("endTimeEstimated", true);
            }
            session->metadata.insert("recovered", true);
            impl_->sessions.emplace(id, std::move(session));
            this->finish(id, {}, true);
        }
        catch (const std::exception &error)
        {
            Q_EMIT finished(id, {}, 0, 0, QString::fromUtf8(error.what()));
        }
    }
}

}
