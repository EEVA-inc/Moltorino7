#include "providers/moltorino/MoltorinoAuthPagination.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>

namespace chatterino::MoltorinoAuth::detail {
namespace {

constexpr auto MODERATED_CHANNEL_CURSOR_PREFIX = "chanperm:";

bool isNumericId(const QString &id)
{
    return !id.isEmpty() && std::all_of(id.cbegin(), id.cend(), [](QChar c) {
        return c.unicode() >= u'0' && c.unicode() <= u'9';
    });
}

std::optional<QByteArray> decodeBase64(const QString &encoded)
{
    const auto bytes = encoded.trimmed().toLatin1();
    if (bytes.isEmpty() || bytes.size() > 32768)
    {
        return std::nullopt;
    }

    const auto strict = QByteArray::AbortOnBase64DecodingErrors;
    auto decoded =
        QByteArray::fromBase64(bytes, QByteArray::Base64Encoding | strict);
    if (!decoded.isEmpty())
    {
        return decoded;
    }

    decoded =
        QByteArray::fromBase64(bytes, QByteArray::Base64UrlEncoding | strict);
    if (decoded.isEmpty())
    {
        return std::nullopt;
    }
    return decoded;
}

std::optional<QJsonObject> decodeObject(const QString &encoded)
{
    const auto decoded = decodeBase64(encoded);
    if (!decoded)
    {
        return std::nullopt;
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(*decoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    return document.object();
}

bool replaceBoundary(QJsonValue &value, const QString &currentSuffix,
                     const QString &nextSuffix, int &replacements)
{
    if (value.isString())
    {
        auto text = value.toString();
        if (!text.contains(currentSuffix))
        {
            return true;
        }

        if (!text.endsWith(currentSuffix) ||
            text.count(currentSuffix, Qt::CaseSensitive) != 1)
        {
            return false;
        }

        text.chop(currentSuffix.size());
        text.append(nextSuffix);
        value = text;
        ++replacements;
        return replacements <= 2;
    }

    if (value.isArray())
    {
        auto array = value.toArray();
        for (qsizetype index = 0; index < array.size(); ++index)
        {
            auto item = array.at(index);
            if (!replaceBoundary(item, currentSuffix, nextSuffix, replacements))
            {
                return false;
            }
            array[index] = item;
        }
        value = array;
        return true;
    }

    if (value.isObject())
    {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            QJsonValue item = it.value();
            if (!replaceBoundary(item, currentSuffix, nextSuffix, replacements))
            {
                return false;
            }
            it.value() = item;
        }
        value = object;
    }
    return true;
}

QString encodeObject(const QJsonObject &object)
{
    return QString::fromLatin1(QJsonDocument(object)
                                   .toJson(QJsonDocument::Compact)
                                   .toBase64(QByteArray::Base64Encoding |
                                             QByteArray::OmitTrailingEquals));
}

}

std::optional<QString> advanceModeratedChannelsCursor(
    const QString &cursor, const QString &currentChannelId,
    const QString &nextChannelId)
{
    const auto currentId = currentChannelId.trimmed();
    const auto nextId = nextChannelId.trimmed();
    if (!isNumericId(currentId) || !isNumericId(nextId) || currentId == nextId)
    {
        return std::nullopt;
    }

    auto outer = decodeObject(cursor);
    if (!outer || !outer->value("a").isObject())
    {
        return std::nullopt;
    }

    auto anchor = outer->value("a").toObject();
    const auto wrappedCursor = anchor.value("Cursor");
    if (!wrappedCursor.isString())
    {
        return std::nullopt;
    }

    const auto wrapped = wrappedCursor.toString();
    const auto prefix = QString::fromLatin1(MODERATED_CHANNEL_CURSOR_PREFIX);
    if (!wrapped.startsWith(prefix))
    {
        return std::nullopt;
    }

    auto inner = decodeObject(wrapped.mid(prefix.size()));
    if (!inner)
    {
        return std::nullopt;
    }

    QJsonValue innerValue(*inner);
    const auto currentSuffix = QStringLiteral("__") + currentId;
    const auto nextSuffix = QStringLiteral("__") + nextId;
    int replacements = 0;
    if (!replaceBoundary(innerValue, currentSuffix, nextSuffix, replacements) ||
        replacements != 2 || !innerValue.isObject())
    {
        return std::nullopt;
    }

    anchor.insert("Cursor", prefix + encodeObject(innerValue.toObject()));
    outer->insert("a", anchor);
    const auto advanced = encodeObject(*outer);
    if (advanced.isEmpty() || advanced == cursor.trimmed())
    {
        return std::nullopt;
    }
    return advanced;
}

}
