#include "controllers/ignores/HiddenUserController.hpp"

#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"

#include <QSet>

#include <algorithm>
#include <array>
#include <atomic>

namespace {

using namespace chatterino;

constexpr std::size_t PLATFORM_COUNT = 4;

std::size_t platformIndex(HiddenUserPlatform platform)
{
    return static_cast<std::size_t>(platform);
}

QString normalizeName(QString value)
{
    value = value.trimmed();
    while (value.startsWith(u'@'))
    {
        value.remove(0, 1);
    }
    return value.toCaseFolded();
}

QString normalizeID(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QStringLiteral("id:"), Qt::CaseInsensitive))
    {
        value.remove(0, 3);
    }
    if (value.startsWith(QStringLiteral("kick:"), Qt::CaseInsensitive))
    {
        value.remove(0, 5);
    }
    return value;
}

bool isMentionCharacter(QChar character)
{
    return character.isLetterOrNumber() || character == u'_' ||
           character == u'-' || character == u'.';
}

bool isDisallowedMentionPrefix(QChar character)
{
    return isMentionCharacter(character) || character == u'/' ||
           character == u'\\' || character == u'=' || character == u'+' ||
           character == u'&' || character == u'?';
}

}

namespace chatterino {

struct HiddenUserController::Cache {
    struct Bucket {
        QSet<QString> ids;
        QSet<QString> names;
        QSet<QString> mentionNames;
        QSet<QString> nameOnlyIdentities;
    };

    std::array<Bucket, PLATFORM_COUNT> platforms;
    bool empty = true;
};

HiddenUserPlatform hiddenUserPlatform(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return HiddenUserPlatform::Kick;
        case MessagePlatform::YouTube:
            return HiddenUserPlatform::YouTube;
        case MessagePlatform::TikTok:
            return HiddenUserPlatform::TikTok;
        case MessagePlatform::AnyOrTwitch:
            return HiddenUserPlatform::Twitch;
    }
    return HiddenUserPlatform::Twitch;
}

HiddenUserController::HiddenUserController(SignalVector<HiddenUser> &users)
    : users_(users)
{
    this->rebuild();
    const auto refresh = [this](const auto &) {
        this->rebuild();
        this->changed.invoke();
    };
    this->signalHolder_.managedConnect(users.itemInserted, refresh);
    this->signalHolder_.managedConnect(users.itemRemoved, refresh);
}

void HiddenUserController::rebuild()
{
    auto cache = std::make_shared<Cache>();
    for (const auto &user : this->users_.raw())
    {
        auto &bucket = cache->platforms.at(platformIndex(user.platform()));
        const auto id = normalizeID(user.userID());
        const auto login = normalizeName(user.login());
        const auto displayName = normalizeName(user.displayName());
        if (!id.isEmpty())
        {
            bucket.ids.insert(id);
            cache->empty = false;
        }
        if (!login.isEmpty())
        {
            bucket.names.insert(login);
            bucket.mentionNames.insert(login);
            if (id.isEmpty())
            {
                bucket.nameOnlyIdentities.insert(login);
            }
            cache->empty = false;
        }
        if (!displayName.isEmpty())
        {
            bucket.names.insert(displayName);
            if (id.isEmpty())
            {
                bucket.nameOnlyIdentities.insert(displayName);
            }
            cache->empty = false;
        }
    }
    const bool hasUsers = !cache->empty;
    std::shared_ptr<const Cache> immutableCache = std::move(cache);
    this->cache_.set(std::move(immutableCache));
    this->hasUsers_.store(hasUsers, std::memory_order_release);
}

bool HiddenUserController::isHidden(MessagePlatform platform,
                                    const QString &userID,
                                    const QString &login,
                                    const QString &displayName) const
{
    if (!this->hasUsers_.load(std::memory_order_acquire))
    {
        return false;
    }
    const auto cache = this->cache_.get();
    if (!cache || cache->empty)
    {
        return false;
    }
    return this->isHidden(*cache, platform, userID, login, displayName);
}

bool HiddenUserController::setHidden(HiddenUserPlatform platform,
                                     const QString &userID,
                                     const QString &login,
                                     const QString &displayName, bool hidden)
{
    const bool hasIdentity = !userID.trimmed().isEmpty() ||
                             !login.trimmed().isEmpty() ||
                             !displayName.trimmed().isEmpty();
    if (!hasIdentity)
    {
        return false;
    }

    const auto matches = [&](const HiddenUser &user) {
        return user.matchesIdentity(platform, userID, login, displayName);
    };
    if (hidden)
    {
        if (std::ranges::any_of(this->users_.raw(), matches))
        {
            return false;
        }
        this->users_.append(HiddenUser{platform, userID, login, displayName});
        return true;
    }

    bool changed = false;
    for (auto index = static_cast<int>(this->users_.raw().size()) - 1;
         index >= 0; --index)
    {
        if (matches(this->users_.raw().at(static_cast<std::size_t>(index))))
        {
            this->users_.removeAt(index);
            changed = true;
        }
    }
    return changed;
}

bool HiddenUserController::isHidden(const Cache &cache,
                                    MessagePlatform platform,
                                    const QString &userID,
                                    const QString &login,
                                    const QString &displayName) const
{
    const auto &bucket =
        cache.platforms.at(platformIndex(hiddenUserPlatform(platform)));
    const auto id = normalizeID(userID);
    if (!id.isEmpty())
    {
        if (bucket.ids.contains(id))
        {
            return true;
        }
        const auto normalizedLogin = normalizeName(login);
        const auto normalizedDisplayName = normalizeName(displayName);
        return (!normalizedLogin.isEmpty() &&
                bucket.nameOnlyIdentities.contains(normalizedLogin)) ||
               (!normalizedDisplayName.isEmpty() &&
                bucket.nameOnlyIdentities.contains(normalizedDisplayName));
    }
    const auto normalizedLogin = normalizeName(login);
    if (!normalizedLogin.isEmpty() && bucket.names.contains(normalizedLogin))
    {
        return true;
    }
    const auto normalizedDisplayName = normalizeName(displayName);
    return !normalizedDisplayName.isEmpty() &&
           bucket.names.contains(normalizedDisplayName);
}

bool HiddenUserController::hasHiddenMention(const Cache &cache,
                                            MessagePlatform platform,
                                            const QString &text) const
{
    if (text.isEmpty())
    {
        return false;
    }
    const auto &names = cache.platforms
                            .at(platformIndex(hiddenUserPlatform(platform)))
                            .mentionNames;
    if (names.isEmpty())
    {
        return false;
    }

    qsizetype offset = 0;
    while ((offset = text.indexOf(u'@', offset)) >= 0)
    {
        if (offset > 0 && isDisallowedMentionPrefix(text.at(offset - 1)))
        {
            ++offset;
            continue;
        }
        auto end = offset + 1;
        while (end < text.size() && isMentionCharacter(text.at(end)))
        {
            ++end;
        }
        auto candidate = normalizeName(
            text.mid(offset + 1, end - offset - 1));
        if (names.contains(candidate))
        {
            return true;
        }

        while (candidate.endsWith(u'.') || candidate.endsWith(u'-'))
        {
            candidate.chop(1);
            if (names.contains(candidate))
            {
                return true;
            }
        }
        offset = std::max(end, offset + 1);
    }

    auto leadingStart = qsizetype{0};
    while (leadingStart < text.size() && text.at(leadingStart).isSpace())
    {
        ++leadingStart;
    }
    auto leadingEnd = leadingStart;
    while (leadingEnd < text.size() &&
           isMentionCharacter(text.at(leadingEnd)))
    {
        ++leadingEnd;
    }
    if (leadingEnd > leadingStart && leadingEnd < text.size() &&
        text.at(leadingEnd) == u',' &&
        names.contains(normalizeName(
            text.mid(leadingStart, leadingEnd - leadingStart))))
    {
        return true;
    }

    return false;
}

bool HiddenUserController::shouldSuppressHighlights(
    MessagePlatform platform, const QString &userID, const QString &login,
    const QString &text) const
{
    if (!this->hasUsers_.load(std::memory_order_acquire))
    {
        return false;
    }
    const auto cache = this->cache_.get();
    if (!cache || cache->empty)
    {
        return false;
    }
    return this->isHidden(*cache, platform, userID, login) ||
           this->hasHiddenMention(*cache, platform, text);
}

bool HiddenUserController::shouldHideMessage(const Message &message) const
{
    if (!this->hasUsers_.load(std::memory_order_acquire))
    {
        return false;
    }
    const auto cache = this->cache_.get();
    if (!cache || cache->empty)
    {
        return false;
    }

    if (this->isHidden(*cache, message.platform, message.userID,
                       message.loginName, message.displayName) ||
        this->isHidden(*cache, message.platform, {}, message.timeoutUser))
    {
        return true;
    }

    if (message.replyParent &&
        this->isHidden(*cache, message.replyParent->platform,
                       message.replyParent->userID,
                       message.replyParent->loginName,
                       message.replyParent->displayName))
    {
        return true;
    }

    if (this->hasHiddenMention(*cache, message.platform, message.messageText))
    {
        return true;
    }

    const auto isHiddenUserLink = [&](const Link &link) {
        if (link.type != Link::UserInfo && link.type != Link::UserWhisper)
        {
            return false;
        }
        const bool linksByID =
            link.value.startsWith(QStringLiteral("id:"),
                                  Qt::CaseInsensitive) ||
            link.value.startsWith(QStringLiteral("kick:"),
                                  Qt::CaseInsensitive);
        return linksByID
                   ? this->isHidden(*cache, message.platform, link.value, {})
                   : this->isHidden(*cache, message.platform, {}, link.value);
    };

    for (const auto &element : message.elements)
    {
        if (!element ||
            !element->getFlags().has(MessageElementFlag::Mention))
        {
            continue;
        }
        const auto link = element->getLink();
        if (link.type == Link::UserInfo && isHiddenUserLink(link))
        {
            return true;
        }
    }

    const bool inspectReplyTarget =
        message.flags.has(MessageFlag::ReplyMessage);
    const bool inspectRelatedUsers = message.flags.hasAny(
        {MessageFlag::System, MessageFlag::Subscription,
         MessageFlag::Timeout, MessageFlag::ModerationAction});
    if (!inspectReplyTarget && !inspectRelatedUsers)
    {
        return false;
    }

    for (const auto &element : message.elements)
    {
        if (!element)
        {
            continue;
        }
        if (!inspectRelatedUsers &&
            !element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }
        if (isHiddenUserLink(element->getLink()))
        {
            return true;
        }
    }

    return false;
}

}
