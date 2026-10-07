#pragma once

#include <QColor>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <optional>
#include <vector>

namespace chatterino {

constexpr qsizetype TIKTOK_MESSAGE_LIMIT = 150;

std::optional<QString> normalizeTikTokHandle(QStringView source);
QUrl tikTokProfileUrl(QStringView handle);
bool isTikTokImageUrl(const QUrl &url);

struct TikTokBadge {
    QString imageUrl;
    QString label;
    QString text;
    QColor textColor;
    QColor backgroundColor;
    QColor darkBackgroundColor;
    QString backgroundUrl;
    QString darkBackgroundUrl;
    int scene = 0;
    bool combined = false;
};

struct TikTokAuthor {
    QString id;
    QString handle;
    QString displayName;
    QString avatarUrl;
    QString bio;
    bool verified = false;
    bool moderator = false;
    bool broadcaster = false;
    bool subscriber = false;
    std::vector<TikTokBadge> badges;
};

struct TikTokEmote {
    QString id;
    QString imageUrl;

    int index = -1;
};

struct TikTokGift {
    QString id;
    QString name;
    QString imageUrl;
    QString groupID;
    TikTokAuthor recipient;
};

struct TikTokEvent {
    enum class Kind {
        Chat,
        Gift,
        Subscription,
        Viewers,
        Control,
        Delete,
        AccessDenied
    };
    Kind kind = Kind::Chat;
    QString id;
    QString roomID;
    QDateTime time;
    TikTokAuthor author;
    QString text;
    std::vector<TikTokEmote> emotes;
    std::optional<TikTokGift> gift;
    bool historical = false;
    quint64 value = 0;
    std::vector<QString> deletedMessages;
    std::vector<QString> deletedUsers;
};

struct TikTokRoom {
    QString id;
    QString title;
    QString avatarUrl;
    qint64 viewers = -1;
    bool live = false;
    bool paused = false;
};

}
