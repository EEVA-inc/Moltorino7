#include "providers/tiktok/TikTokProtocol.hpp"

#include <boost/beast/zlib/inflate_stream.hpp>
#include <boost/crc.hpp>
#include <QByteArrayView>
#include <QtEndian>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace chatterino::tiktok {
using namespace Qt::Literals::StringLiterals;

namespace {

struct InvalidWire : std::runtime_error {
    InvalidWire()
        : std::runtime_error("Invalid TikTok packet")
    {
    }
};

quint64 varint(QByteArrayView data, qsizetype &pos)
{
    quint64 value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7)
    {
        if (pos >= data.size())
        {
            throw InvalidWire();
        }
        auto byte = static_cast<unsigned char>(data[pos++]);
        if (shift == 63 && byte > 1)
        {
            throw InvalidWire();
        }
        value |= quint64(byte & 127) << shift;
        if ((byte & 128) == 0)
        {
            return value;
        }
    }
    throw InvalidWire();
}

struct Field {
    quint32 number;
    unsigned wire;
    quint64 value = 0;
    QByteArrayView bytes;
};

class Fields
{
public:
    explicit Fields(QByteArrayView data)
    {
        qsizetype pos = 0;
        while (pos < data.size())
        {
            const auto tag = varint(data, pos);
            if (tag < 8 || tag >> 3 > 0x1fffffff || this->values.size() >= 8192)
            {
                throw InvalidWire();
            }
            Field field{quint32(tag >> 3), unsigned(tag & 7)};
            if (field.wire == 0)
            {
                field.value = varint(data, pos);
            }
            else if (field.wire == 1 || field.wire == 2 || field.wire == 5)
            {
                const quint64 length = field.wire == 2   ? varint(data, pos)
                                       : field.wire == 1 ? 8
                                                         : 4;
                if (length > quint64(data.size() - pos))
                {
                    throw InvalidWire();
                }
                field.bytes = data.sliced(pos, qsizetype(length));
                pos += qsizetype(length);
            }
            else
            {
                throw InvalidWire();
            }
            this->values.push_back(field);
        }
    }

    QByteArrayView bytes(quint32 number) const
    {
        for (auto it = values.rbegin(); it != values.rend(); ++it)
        {
            if (it->number == number && it->wire == 2)
            {
                return it->bytes;
            }
        }
        return {};
    }
    quint64 number(quint32 number) const
    {
        for (auto it = values.rbegin(); it != values.rend(); ++it)
        {
            if (it->number == number && it->wire == 0)
            {
                return it->value;
            }
        }
        return 0;
    }
    QString text(quint32 number, qsizetype limit = 4096) const
    {
        const auto data = this->bytes(number);
        if (data.size() > limit * 4)
        {
            throw InvalidWire();
        }
        const auto text = QString::fromUtf8(data);
        if (text.size() > limit || text.contains(QChar(0)))
        {
            throw InvalidWire();
        }
        return text;
    }
    std::vector<Field> values;
};

void putVarint(QByteArray &data, quint64 value)
{
    while (value >= 128)
    {
        data.append(char((value & 127) | 128));
        value >>= 7;
    }
    data.append(char(value));
}

QByteArray integer(unsigned field, quint64 value)
{
    QByteArray data;
    putVarint(data, quint64(field) << 3);
    putVarint(data, value);
    return data;
}

QByteArray blob(unsigned field, QByteArrayView value)
{
    QByteArray data;
    putVarint(data, (quint64(field) << 3) | 2);
    putVarint(data, quint64(value.size()));
    data.append(value.data(), value.size());
    return data;
}

QByteArray frame(QByteArrayView kind, QByteArrayView payload, quint64 logID = 0)
{
    return integer(2, logID) + blob(6, "pb") + blob(7, kind) + blob(8, payload);
}

QByteArray inflate(QByteArrayView data)
{
    if (data.size() < 18 || static_cast<unsigned char>(data[0]) != 0x1f ||
        static_cast<unsigned char>(data[1]) != 0x8b || data[2] != 8 ||
        (static_cast<unsigned char>(data[3]) & 0xe0) != 0)
    {
        throw InvalidWire();
    }
    qsizetype pos = 10;
    const auto flags = static_cast<unsigned char>(data[3]);
    const auto end = data.size() - 8;
    if (flags & 4)
    {
        if (pos + 2 > end)
        {
            throw InvalidWire();
        }
        const auto length = qFromLittleEndian<quint16>(data.data() + pos);
        pos += 2 + length;
    }
    for (auto flag : {8, 16})
    {
        if (flags & flag)
        {
            while (pos < end && data[pos] != 0)
            {
                ++pos;
            }
            ++pos;
        }
    }
    if (flags & 2)
    {
        if (pos + 2 > end)
        {
            throw InvalidWire();
        }
        boost::crc_32_type crc;
        crc.process_bytes(data.data(), size_t(pos));
        if (quint16(crc.checksum()) !=
            qFromLittleEndian<quint16>(data.data() + pos))
        {
            throw InvalidWire();
        }
        pos += 2;
    }
    const auto size = qFromLittleEndian<quint32>(data.data() + end + 4);
    if (pos > end || size > MAX_INFLATED_BYTES)
    {
        throw InvalidWire();
    }

    QByteArray result(qsizetype(size) + 1, Qt::Uninitialized);
    boost::beast::zlib::inflate_stream stream;
    boost::beast::zlib::z_params params{};
    params.next_in = data.data() + pos;

    params.avail_in = size_t(data.size() - pos);
    params.next_out = result.data();
    params.avail_out = size_t(result.size());
    boost::system::error_code error;
    stream.write(params, boost::beast::zlib::Flush::finish, error);
    if (error != boost::beast::zlib::error::end_of_stream ||
        params.total_out != size ||
        params.avail_in + (params.data_type & 63) / 8 != 8)
    {
        throw InvalidWire();
    }
    result.resize(size);
    boost::crc_32_type crc;
    crc.process_bytes(result.data(), size_t(result.size()));
    if (crc.checksum() != qFromLittleEndian<quint32>(data.data() + end))
    {
        throw InvalidWire();
    }
    return result;
}

QString imageUrl(QByteArrayView bytes)
{
    const Fields image(bytes);
    for (const auto &field : image.values)
    {
        if (field.number == 1 && field.wire == 2 && field.bytes.size() <= 4096)
        {
            const auto value = QString::fromUtf8(field.bytes);
            if (isTikTokImageUrl(QUrl(value)))
            {
                return value;
            }
        }
    }
    return {};
}

QString id(quint64 value)
{
    return value ? QString::number(value) : QString{};
}

TikTokAuthor author(QByteArrayView bytes, QByteArrayView identityBytes)
{
    const Fields user(bytes);
    const Fields identity(identityBytes);
    const Fields attributes(user.bytes(32));
    const Fields subscription(user.bytes(63));
    TikTokAuthor result;
    result.id = id(user.number(1));
    if (result.id.isEmpty())
    {
        result.id = user.text(1028, 32);
    }
    result.handle =
        normalizeTikTokHandle(user.text(38, 64)).value_or(QString{});
    result.displayName = user.text(3, 256).trimmed();
    result.bio = user.text(5, 1024);
    for (const auto field : {11, 10, 9})
    {
        result.avatarUrl = imageUrl(user.bytes(field));
        if (!result.avatarUrl.isEmpty())
        {
            break;
        }
    }
    result.verified = user.number(12) != 0;
    result.moderator =
        identity.number(5) || attributes.number(2) || attributes.number(3);
    result.broadcaster = identity.number(6) != 0;
    result.subscriber = identity.number(2) || subscription.number(7);
    for (const auto &field : user.values)
    {
        if (field.number != 64 || field.wire != 2 || result.badges.size() >= 8)
        {
            continue;
        }
        const Fields badge(field.bytes);

        if (badge.number(11) == 0 &&
            std::ranges::any_of(badge.values, [](const auto &value) {
                return value.number == 11 && value.wire == 0;
            }))
        {
            continue;
        }
        QString label;
        switch (badge.number(3))
        {
            case 1:
                label = u"Moderator"_s;
                break;
            case 4:
            case 7:
                label = u"Subscriber"_s;
                break;
            case 8:
                label = u"Gifter level"_s;
                break;
            case 10:
                label = u"Fan club"_s;
                break;
            case 11:
                label = u"LIVE Pro"_s;
                break;
            case 12:
                label = u"Host"_s;
                break;
            default:
                label = u"TikTok badge"_s;
                break;
        }
        const bool combined = !badge.bytes(23).isEmpty();
        const Fields detail(badge.bytes(combined ? 23 : 20));
        const auto url = imageUrl(detail.bytes(2));
        if (!url.isEmpty())
        {
            TikTokBadge record{
                .imageUrl = url,
                .label = label,
                .scene = int(std::min<quint64>(badge.number(3), 255)),
                .combined = combined};
            if (combined)
            {
                record.text = detail.text(4, 64).simplified().left(32);
                const Fields textBadge(detail.bytes(3));

                if (record.text.isEmpty() && textBadge.bytes(4).isEmpty())
                {
                    record.text = textBadge.text(3, 64).simplified().left(32);
                }
                record.textColor = QColor(Fields(detail.bytes(6)).text(3, 32));
                const Fields background(detail.bytes(11));
                const Fields darkBackground(detail.bytes(12));
                record.backgroundColor = QColor(background.text(2, 32));
                record.darkBackgroundColor = QColor(darkBackground.text(2, 32));
                record.backgroundUrl = imageUrl(background.bytes(1));
                record.darkBackgroundUrl = imageUrl(darkBackground.bytes(1));
                if (!record.text.isEmpty())
                {
                    record.label += u' ' + record.text;
                }
            }
            result.badges.push_back(std::move(record));
        }
    }
    return result;
}

TikTokEmote emote(QByteArrayView bytes, int index = -1)
{
    const Fields model(bytes);
    return {model.text(1, 128), imageUrl(model.bytes(2)), index};
}

std::vector<QString> ids(const Fields &fields, quint32 number)
{
    std::vector<QString> result;
    for (const auto &field : fields.values)
    {
        if (field.number != number)
        {
            continue;
        }
        if (result.size() >= 4096)
        {
            throw InvalidWire();
        }
        if (field.wire == 0 && field.value)
        {
            result.push_back(id(field.value));
        }
        else if (field.wire == 2)
        {
            qsizetype pos = 0;
            while (pos < field.bytes.size())
            {
                if (result.size() >= 4096)
                {
                    throw InvalidWire();
                }
                const auto value = varint(field.bytes, pos);
                if (value)
                {
                    result.push_back(id(value));
                }
            }
        }
    }
    return result;
}

std::optional<TikTokEvent> event(const Fields &wrapper, bool initial)
{
    const auto type = wrapper.text(1, 128);
    const bool chat = type == u"WebcastChatMessage";
    const bool emoteOnly = type == u"WebcastEmoteChatMessage";
    const bool gift = type == u"WebcastGiftMessage";
    const bool subscription = type == u"WebcastSubNotifyMessage";
    if (!chat && !emoteOnly && !gift && !subscription &&
        type != u"WebcastRoomUserSeqMessage" &&
        type != u"WebcastControlMessage" && type != u"WebcastImDeleteMessage" &&
        type != u"WebcastRoomVerifyMessage" &&
        type != u"WebcastAccessControlMessage")
    {
        return std::nullopt;
    }
    const Fields payload(wrapper.bytes(2));
    const Fields common(payload.bytes(1));
    TikTokEvent result;
    result.id = id(common.number(2) ? common.number(2) : wrapper.number(3));
    result.roomID = id(common.number(3));
    const auto seconds = common.number(4);
    if (seconds > 0 && seconds < 4102444800ULL)
    {
        result.time = QDateTime::fromSecsSinceEpoch(qint64(seconds), Qt::UTC);
    }
    result.historical = initial || wrapper.number(6) != 0;
    if (chat || emoteOnly || gift || subscription)
    {
        quint32 authorField = 2;
        quint32 identityField = 0;
        if (gift)
        {
            authorField = 7;
            identityField = 32;
        }
        else if (emoteOnly)
        {
            identityField = 5;
        }
        else if (chat)
        {
            identityField = 18;
        }
        result.author =
            author(payload.bytes(authorField), payload.bytes(identityField));
        if (result.author.id.isEmpty())
        {
            return std::nullopt;
        }
        result.text = chat ? payload.text(3, 4096) : QString{};
        for (const auto &field : payload.values)
        {
            if (field.wire != 2 || result.emotes.size() >= 64)
            {
                continue;
            }
            if (chat && field.number == 13)
            {
                const Fields indexed(field.bytes);
                const auto index = indexed.number(1);

                result.emotes.push_back(emote(
                    indexed.bytes(2), index <= quint64(result.text.size() + 64)
                                          ? int(index)
                                          : -2));
            }
            else if (emoteOnly && field.number == 3)
            {
                result.emotes.push_back(emote(field.bytes));
            }
        }
        if (gift)
        {
            const Fields details(payload.bytes(15));

            if (details.number(11) == 1 && payload.number(9) == 0)
            {
                return std::nullopt;
            }
            result.kind = TikTokEvent::Kind::Gift;
            result.value = std::clamp<quint64>(payload.number(5), 1, 1000000);
            auto &info = result.gift.emplace();
            info.id =
                id(payload.number(2) ? payload.number(2) : details.number(5));
            info.name = details.text(16, 256).simplified();
            info.imageUrl = imageUrl(details.bytes(1));
            if (info.imageUrl.isEmpty())
            {
                info.imageUrl = imageUrl(details.bytes(21));
            }
            info.groupID = id(payload.number(11));
            info.recipient = author(payload.bytes(8), {});
        }
        else if (subscription)
        {
            const auto status = payload.number(8);
            if (status != 1 && status != 2)
            {
                return std::nullopt;
            }
            result.kind = TikTokEvent::Kind::Subscription;
            result.author.subscriber = true;
            result.value = std::min<quint64>(payload.number(4), 1200);
        }
    }
    else if (type == u"WebcastRoomUserSeqMessage")
    {
        result.kind = TikTokEvent::Kind::Viewers;
        result.value = payload.number(3);
    }
    else if (type == u"WebcastControlMessage")
    {
        result.kind = TikTokEvent::Kind::Control;
        result.value = payload.number(2);
    }
    else if (type == u"WebcastRoomVerifyMessage")
    {
        if (!payload.number(5))
        {
            return std::nullopt;
        }
        result.kind = TikTokEvent::Kind::Control;
        result.value = 3;
    }
    else if (type == u"WebcastAccessControlMessage")
    {
        if (payload.bytes(2).isEmpty())
        {
            return std::nullopt;
        }
        result.kind = TikTokEvent::Kind::AccessDenied;
    }
    else
    {
        result.kind = TikTokEvent::Kind::Delete;
        result.deletedMessages = ids(payload, 2);
        result.deletedUsers = ids(payload, 3);
    }
    return result;
}

}

std::optional<Batch> decodeFrame(const QByteArray &data)
{
    if (data.isEmpty() || data.size() > MAX_FRAME_BYTES)
    {
        return std::nullopt;
    }
    try
    {
        const Fields outer(data);
        const auto kind = outer.text(7, 64);
        Batch batch;
        if (kind != u"msg" && kind != u"im_enter_room_resp")
        {
            return batch;
        }
        auto payload = outer.bytes(8);
        QByteArray expanded;
        if (payload.size() >= 2 &&
            static_cast<unsigned char>(payload[0]) == 0x1f &&
            static_cast<unsigned char>(payload[1]) == 0x8b)
        {
            expanded = inflate(payload);
            payload = expanded;
        }
        const Fields response(payload);
        const auto ext = response.bytes(5);
        if (response.number(9))
        {
            if (ext.size() > 16384)
            {
                throw InvalidWire();
            }
            batch.acknowledgement = frame("ack", ext, outer.number(2));
        }
        const bool initial =
            response.number(11) || kind == u"im_enter_room_resp";
        size_t messages = 0;
        for (const auto &field : response.values)
        {
            if (field.number != 1 || field.wire != 2)
            {
                continue;
            }
            if (++messages > 1024)
            {
                throw InvalidWire();
            }
            const Fields wrapper(field.bytes);
            batch.roomTraffic = true;
            if (auto parsed = event(wrapper, initial))
            {
                batch.events.push_back(std::move(*parsed));
            }
        }
        return batch;
    }
    catch (const InvalidWire &)
    {
        return std::nullopt;
    }
}

QByteArray heartbeat(quint64 roomID)
{
    return frame("hb", integer(1, roomID));
}

QByteArray enterRoom(quint64 roomID)
{
    return frame("im_enter_room", integer(1, roomID) + integer(4, 12) +
                                      blob(5, "audience") + blob(9, "0"));
}

QByteArray userAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
}

QUrl socketUrl(QStringView roomID)
{
    QUrl url(
        u"wss://webcast-ws.tiktok.com/webcast/im/ws_proxy/ws_reuse_supplement/"_s);
    QUrlQuery query;
    const std::pair<QString, QString> values[] = {
        {u"version_code"_s, u"180800"_s},
        {u"device_platform"_s, u"web"_s},
        {u"cookie_enabled"_s, u"true"_s},
        {u"screen_width"_s, u"1920"_s},
        {u"screen_height"_s, u"1080"_s},
        {u"browser_language"_s, u"en-US"_s},
        {u"browser_platform"_s, u"Win32"_s},
        {u"browser_name"_s, u"Mozilla"_s},
        {u"browser_version"_s, u"5.0 (Windows)"_s},
        {u"browser_online"_s, u"true"_s},
        {u"tz_name"_s, u"Europe/Berlin"_s},
        {u"app_name"_s, u"tiktok_web"_s},
        {u"sup_ws_ds_opt"_s, u"1"_s},
        {u"update_version_code"_s, u"2.0.0"_s},
        {u"compress"_s, u"gzip"_s},
        {u"webcast_language"_s, u"en"_s},
        {u"ws_direct"_s, u"1"_s},
        {u"aid"_s, u"1988"_s},
        {u"live_id"_s, u"12"_s},
        {u"app_language"_s, u"en"_s},
        {u"client_enter"_s, u"1"_s},
        {u"room_id"_s, roomID.toString()},
        {u"identity"_s, u"audience"_s},
        {u"history_comment_count"_s, u"6"_s},
        {u"last_rtt"_s, u"150.000"_s},
        {u"heartbeat_duration"_s, u"10000"_s},
        {u"resp_content_type"_s, u"protobuf"_s},
        {u"did_rule"_s, u"3"_s},
    };
    for (const auto &[key, value] : values)
    {
        query.addQueryItem(key, value);
    }
    url.setQuery(query);
    return url;
}

}
