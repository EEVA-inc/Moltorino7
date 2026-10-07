#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chatterino {

inline constexpr int YOUTUBE_MESSAGE_LIMIT = 200;

enum class YouTubeMessageKind : std::uint8_t {
    Text,
    SuperChat,
    NewMembership,
    MembershipMilestone,
    MembershipGift,
    Tombstone,
    UserBanned,
    AuthorMessagesDeleted,
    ChatEnded,
    Ignored,
};

enum class YouTubeBanKind : std::uint8_t {
    None,
    Temporary,
    Permanent,
};

struct YouTubeAuthor {
    QString channelId;

    QString handle;
    QString displayName;
    QString avatarUrl;
    QString membershipBadgeUrl;
    QString membershipBadgeTooltip;

    bool isOwner = false;
    bool isModerator = false;
    bool isVerified = false;
    bool isMember = false;

    bool roleMetadataKnown = false;

    bool operator==(const YouTubeAuthor &) const = default;
};

[[nodiscard]] QString visibleYouTubeName(QString name);

[[nodiscard]] QString youtubeChannelUrl(const QString &channelID);

[[nodiscard]] bool isTrustedYouTubeImageUrl(const QString &url);

void applyYouTubeAuthorName(YouTubeAuthor &author, QString name);

struct YouTubeBanDetails {
    QString targetChannelId;
    QString targetDisplayName;
    YouTubeBanKind kind = YouTubeBanKind::None;
    std::chrono::seconds duration{};
};

struct YouTubeMessageRun {
    enum class Kind : std::uint8_t {
        Text,
        Emoji,
    };

    Kind kind = Kind::Text;

    QString text;
    QString emojiID;
    QStringList emojiShortcuts;
    QString emojiImageUrl;
    bool customEmoji = false;
};

[[nodiscard]] bool shouldJoinYouTubeMessageRuns(
    const YouTubeMessageRun &left, const YouTubeMessageRun &right) noexcept;

struct YouTubeMessage {
    QString id;
    QString liveChatId;
    YouTubeMessageKind kind = YouTubeMessageKind::Ignored;
    QString text;

    QString eventText;
    QString amountDisplayString;
    QString membershipLevelName;
    int memberMonths = 0;
    int giftCount = 0;
    std::vector<YouTubeMessageRun> runs;
    QDateTime publishedAt;
    YouTubeAuthor author;
    std::optional<YouTubeBanDetails> ban;

    QString targetMessageID;
    QString targetAuthorChannelID;

    QString targetAuthorDisplayName;
    bool replacesExisting = false;

    bool localEcho = false;

    bool historical = false;

    [[nodiscard]] bool isIgnored() const noexcept
    {
        return this->kind == YouTubeMessageKind::Ignored;
    }

    [[nodiscard]] bool isUserChatMessage() const noexcept;
};

class YouTubeMessageDeduper
{
public:
    static constexpr std::size_t MAX_ENTRIES = 4'096;

    [[nodiscard]] bool accept(const YouTubeMessage &message);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct QStringViewHash {
        std::size_t operator()(QStringView value) const
        {
            return qHash(value);
        }
    };

    std::unordered_set<QStringView, QStringViewHash> seen_;
    std::deque<QString> order_;
};

class YouTubeReplayBuffer
{
public:
    explicit YouTubeReplayBuffer(std::size_t limit);

    YouTubeReplayBuffer(const YouTubeReplayBuffer &) = delete;
    YouTubeReplayBuffer(YouTubeReplayBuffer &&) = delete;
    YouTubeReplayBuffer &operator=(const YouTubeReplayBuffer &) = delete;
    YouTubeReplayBuffer &operator=(YouTubeReplayBuffer &&) = delete;

    void append(YouTubeMessage message);
    [[nodiscard]] std::vector<YouTubeMessage> snapshot() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t limit() const noexcept;

private:
    struct StoredMessage {
        YouTubeMessage message;
        std::shared_ptr<const YouTubeAuthor> author;
        std::uint64_t sequence = 0;
    };

    struct IDRecord {
        StoredMessage *first = nullptr;
        std::size_t count = 0;
    };

    struct QStringViewHash {
        std::size_t operator()(QStringView value) const
        {
            return qHash(value);
        }
    };

    [[nodiscard]] StoredMessage store(YouTubeMessage message);
    [[nodiscard]] std::shared_ptr<const YouTubeAuthor> internAuthor(
        YouTubeAuthor author);
    void addID(StoredMessage &message);
    void removeID(StoredMessage &message);
    void evictOldest();
    void pruneAuthors();

    const std::size_t limit_;
    std::deque<StoredMessage> messages_;

    std::unordered_map<QStringView, IDRecord, QStringViewHash> byID_;
    std::unordered_map<QString, std::weak_ptr<const YouTubeAuthor>> authors_;
    std::uint64_t nextSequence_ = 0;
    std::size_t evictionsSinceAuthorPrune_ = 0;
};

struct YouTubePendingSend {
    QString localMessageID;
    QString visibleMessageID;
    QString acknowledgedMessageID;
    QString liveChatID;
    QString text;
    QDateTime sentAt;
    YouTubeAuthor author;
    std::chrono::steady_clock::time_point trackedAt{};
};

class YouTubePendingSendTracker
{
public:
    static constexpr std::size_t MAX_PENDING_SENDS = 128;
    static constexpr auto MATCH_WINDOW = std::chrono::seconds(30);

    void track(YouTubePendingSend send);

    [[nodiscard]] std::optional<QString> acknowledge(
        QStringView localMessageID, QString acknowledgedMessageID);

    [[nodiscard]] std::optional<YouTubePendingSend> takeMatching(
        const YouTubeMessage &message,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    [[nodiscard]] std::optional<YouTubePendingSend> takeByLocalID(
        QStringView localMessageID);

    [[nodiscard]] std::optional<YouTubePendingSend> takeByMessageID(
        QStringView messageID);

    [[nodiscard]] std::vector<QString> unacknowledgedLocalMessageIDs() const;
    void clear();
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::deque<YouTubePendingSend> pending_;
};

[[nodiscard]] YouTubeMessageKind classifyYouTubeMessageType(
    QStringView sourceType) noexcept;

[[nodiscard]] YouTubeMessage parseYouTubeMessage(const QJsonObject &object);

}
