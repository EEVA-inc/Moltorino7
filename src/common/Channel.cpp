// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Channel.hpp"

#include "Application.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageSimilarity.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Settings.hpp"
#include "util/ChannelHelpers.hpp"

namespace {

constexpr uint8_t MAX_RECURSION = 64;

struct RecursionGuard {
    constexpr RecursionGuard(uint8_t *count) noexcept
        : count(count)
    {
        assert(*count < MAX_RECURSION);
        *this->count += 1;
    }
    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard(RecursionGuard &&) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;
    RecursionGuard &operator=(RecursionGuard &&) = delete;
    constexpr ~RecursionGuard()
    {
        *this->count -= 1;
    }

    uint8_t *count;
};

}

namespace chatterino {

//
// Channel
//
Channel::Channel(const QString &name, Type type)
    : Channel(name, type,
              sanitizeScrollbackLimit(
                  getSettings()->scrollbackSplitLimit.getValue()))
{
}

Channel::~Channel()
{
    auto *app = tryGetApp();
    if (app && !isAppAboutToQuit() && this->anythingLogged_)
    {
        app->getChatLogger()->closeChannel(this->name_, this->platform_);
    }
}

Channel::Type Channel::getType() const
{
    return this->type_;
}

const QString &Channel::getName() const
{
    return this->name_;
}

const QString &Channel::getDisplayName() const
{
    return this->getName();
}

const QString &Channel::getLocalizedName() const
{
    return this->getName();
}

bool Channel::isTwitchChannel() const
{
    return this->type_ >= Type::Twitch && this->type_ < Type::TwitchEnd;
}

bool Channel::isKickChannel() const
{
    return this->type_ == Type::Kick;
}

bool Channel::isTwitchOrKickChannel() const
{
    return this->isTwitchChannel() || this->isKickChannel();
}

bool Channel::isEmpty() const
{
    return this->name_.isEmpty();
}

bool Channel::hasMessages() const
{
    return !this->messages_.empty();
}

size_t Channel::countMessages() const
{
    return this->messages_.size();
}

std::vector<MessagePtr> Channel::getMessageSnapshot() const
{
    return this->messages_.getSnapshot();
}

std::vector<MessagePtr> Channel::getMessageSnapshot(size_t nItems) const
{
    return this->messages_.lastN(nItems);
}

std::vector<MessagePtrMut> Channel::getMessageSnapshotMut(size_t nItems) const
{
    return this->messages_.lastNBy<MessagePtrMut>(nItems, [](const auto &msg) {
        return std::const_pointer_cast<Message>(msg);
    });
}

MessagePtr Channel::getLastMessage() const
{
    auto last = this->messages_.last();
    if (last)
    {
        return *std::move(last);
    }
    return nullptr;
}

void Channel::addMessage(MessagePtr message, MessageContext context,
                         std::optional<MessageFlags> overridingFlags)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    message->freeze();

    MessagePtr deleted;

    if (context == MessageContext::Original && this->getType() != Type::None)
    {
        // Only log original messages
        auto isDoNotLogSet =
            (overridingFlags && overridingFlags->has(MessageFlag::DoNotLog)) ||
            message->flags.has(MessageFlag::DoNotLog);

        if (!isDoNotLogSet)
        {
            // Only log messages where the `DoNotLog` flag is not set
            getApp()->getChatLogger()->addMessage(this->name_, message,
                                                  this->platform_,
                                                  this->getCurrentStreamID());
            this->anythingLogged_ = true;
        }
    }

    if (this->messages_.pushBack(message, deleted))
    {
        this->messageRemovedFromStart(deleted);
    }

    this->messageAppended.invoke(message, overridingFlags);
}

void Channel::addSystemMessage(const QString &contents)
{
    MessageBuilder builder(systemMessage, contents);
    if (this->isTikTokChannel())
    {
        builder->platform = MessagePlatform::TikTok;
        builder->channelName = this->getName();
        auto channel = std::make_unique<ChannelNameElement>(
            u'#' + this->getDisplayName());
        channel->setLink({Link::JumpToChannel,
                          QStringLiteral(":tiktok:") + this->getName()});
        builder->elements.insert(builder->elements.begin(), std::move(channel));
    }
    this->addMessage(builder.release(), MessageContext::Original);
}

void Channel::addOrReplaceTimeout(MessagePtr message, const QDateTime &now)
{
    addOrReplaceChannelTimeout(
        this->getMessageSnapshot(), std::move(message), now,
        [this](auto /*idx*/, auto msg, auto replacement) {
            this->replaceMessage(msg, replacement);
        },
        [this](auto msg) {
            this->addMessage(msg, MessageContext::Original);
        },
        true);
}

void Channel::addOrReplaceClearChat(MessagePtr message, const QDateTime &now)
{
    addOrReplaceChannelClear(
        this->getMessageSnapshot(20), std::move(message), now,
        [this](auto /*idx*/, auto msg, auto replacement) {
            this->replaceMessage(msg, replacement);
        },
        [this](auto msg) {
            this->addMessage(msg, MessageContext::Original);
        });
}

void Channel::disableAllMessages()
{
    for (const auto &message : this->getMessageSnapshot())
    {
        if (message->flags.hasAny({MessageFlag::System, MessageFlag::Timeout,
                                   MessageFlag::Whisper}))
        {
            continue;
        }

        message->flags.set(MessageFlag::Disabled);
    }
}

void Channel::addMessagesAtStart(const std::vector<MessagePtr> &_messages)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    for (const auto &msg : _messages)
    {
        msg->freeze();
    }

    std::vector<MessagePtr> addedMessages =
        this->messages_.pushFront(_messages);

    if (addedMessages.size() != 0)
    {
        this->messagesAddedAtStart.invoke(addedMessages);
    }
}

void Channel::fillInMissingMessages(const std::vector<MessagePtr> &messages)
{
    if (messages.empty())
    {
        return;
    }

    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    for (const auto &msg : messages)
    {
        msg->freeze();
    }

    while (true)
    {
        auto snapshot = this->getMessageSnapshot();
        if (snapshot.empty())
        {
            // There are no messages in this channel yet so we can just insert them
            // at the front in order
            this->messages_.pushFront(messages);
            this->filledInMessages.invoke(messages);
            return;
        }

        std::unordered_set<QString> existingMessageIds;
        existingMessageIds.reserve(snapshot.size());
        // First, collect the ids of every message already present in the channel
        for (const auto &msg : snapshot)
        {
            if (!msg->flags.has(MessageFlag::System) && !msg->id.isEmpty())
            {
                existingMessageIds.insert(msg->id);
            }
        }

        std::vector<MessagePtr> missing;
        missing.reserve(messages.size());
        for (const auto &msg : messages)
        {
            if (existingMessageIds.count(msg->id) == 0)
            {
                missing.push_back(msg);
            }
        }
        if (missing.empty())
        {
            return;
        }

        std::vector<MessagePtr> merged;
        merged.reserve(snapshot.size() + missing.size());
        auto nextMissing = missing.begin();
        for (const auto &current : snapshot)
        {
            if (!current->flags.has(MessageFlag::System))
            {
                while (nextMissing != missing.end() &&
                       (*nextMissing)->serverReceivedTime <
                           current->serverReceivedTime)
                {
                    merged.push_back(*nextMissing++);
                }
            }
            merged.push_back(current);
        }
        merged.insert(merged.end(), nextMissing, missing.end());

        const auto retained =
            std::min(merged.size(), this->messages_.limit());
        const auto retainedBegin = merged.end() - retained;
        if (retained == snapshot.size() &&
            std::equal(retainedBegin, merged.end(), snapshot.begin()))
        {
            return;
        }

        if (this->messages_.replaceContentsIfUnchanged(snapshot,
                                                       std::move(merged)))
        {
            // We only invoke a signal once at the end of filling all messages to
            // prevent doing any unnecessary repaints.
            this->filledInMessages.invoke(messages);
            return;
        }
    }
}

void Channel::replaceMessage(const MessagePtr &message,
                             const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    replacement->freeze();
    int index = this->messages_.replaceItem(message, replacement);

    if (index >= 0)
    {
        this->messageReplaced.invoke((size_t)index, message, replacement);
    }
}

void Channel::replaceMessage(size_t index, const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    replacement->freeze();

    MessagePtr prev;
    if (this->messages_.replaceItem(index, replacement, &prev))
    {
        this->messageReplaced.invoke(index, prev, replacement);
    }
}

bool Channel::replaceMessage(size_t hint, const MessagePtr &message,
                             const MessagePtr &replacement)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return false;
    }

    replacement->freeze();

    auto index = this->messages_.replaceItem(hint, message, replacement);
    if (index >= 0)
    {
        this->messageReplaced.invoke(static_cast<size_t>(index), message,
                                    replacement);
        return true;
    }
    return false;
}

void Channel::disableMessage(const QString &messageID)
{
    auto msg = this->findMessageByID(messageID);
    if (msg != nullptr)
    {
        msg->flags.set(MessageFlag::Disabled);
    }
}

void Channel::mergeFrom(const std::span<std::span<const MessagePtr>> sources)
{
    assert(this->messages_.empty());
    this->messages_.pushFrontWhile([&] {
        MessagePtr max;
        QDateTime dt;
        size_t curI = 0;
        for (size_t i = 0; i < sources.size(); i++)
        {
            auto src = sources[i];
            if (!src.empty())
            {
                QDateTime cur = src.back()->serverReceivedTime;
                if (!dt.isValid() || cur > dt)
                {
                    max = src.back();
                    dt = cur;
                    curI = i;
                }
            }
        }
        if (max)
        {
            sources[curI] = sources[curI].subspan(0, sources[curI].size() - 1);
        }
        return max;
    });
}

void Channel::clearMessages()
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    this->messages_.clear();
    this->messagesCleared.invoke();
}

MessagePtr Channel::findMessageByID(QStringView messageID)
{
    if (messageID.isEmpty())
    {
        return nullptr;
    }

    MessagePtr res;

    if (auto msg = this->messages_.rfind([messageID](const MessagePtr &msg) {
            return msg->id == messageID;
        });
        msg)
    {
        res = *msg;
    }

    return res;
}

void Channel::applySimilarityFilters(const MessagePtr &message) const
{
    setSimilarityFlags(message, this->messages_.getSnapshot());
}

MessageSinkTraits Channel::sinkTraits() const
{
    return {
        MessageSinkTrait::AddMentionsToGlobalChannel,
        MessageSinkTrait::RequiresKnownChannelPointReward,
    };
}

bool Channel::canSendMessage() const
{
    return false;
}

bool Channel::isWritable() const
{
    using Type = Channel::Type;
    auto type = this->getType();
    return type != Type::TwitchMentions && type != Type::TwitchLive &&
           type != Type::TwitchAutomod;
}

void Channel::sendMessage(const QString &message)
{
}

bool Channel::isMod() const
{
    return false;
}

bool Channel::isBroadcaster() const
{
    return false;
}

bool Channel::hasModRights() const
{
    return this->isMod() || this->isBroadcaster();
}

bool Channel::hasHighRateLimit() const
{
    return this->isMod() || this->isBroadcaster();
}

bool Channel::isLive() const
{
    return false;
}

bool Channel::isRerun() const
{
    return false;
}

bool Channel::shouldIgnoreHighlights() const
{
    return this->type_ == Type::TwitchAutomod ||
           this->type_ == Type::TwitchMentions ||
           this->type_ == Type::TwitchWhispers;
}

bool Channel::canReconnect() const
{
    return false;
}

void Channel::reconnect()
{
}

QString Channel::getCurrentStreamID() const
{
    return {};
}

std::shared_ptr<Channel> Channel::getEmpty()
{
    static std::shared_ptr<Channel> channel(new Channel("", Type::None));
    return channel;
}

void Channel::onConnected()
{
}

void Channel::messageRemovedFromStart(const MessagePtr &msg)
{
}

void Channel::upsertPersonalSeventvEmotes(
    const QString &userLogin, const std::shared_ptr<const EmoteMap> &emoteMap)
{
    // This is attempting a (kind-of) surgical replacement of the users' last
    // sent message. The the last message is essentially re-parsed and newly
    // added emotes are inserted where appropriate.

    assertInGuiThread();
    auto snapshot = this->getMessageSnapshot(5);
    if (snapshot.empty())
    {
        return;
    }

    /// Finds the last message of the user (searches the last five messages).
    /// If no message is found, `std::nullopt` is returned.
    const auto findMessage = [&]() -> std::optional<MessagePtr> {
        auto size = static_cast<qsizetype>(snapshot.size());
        auto end = std::max<qsizetype>(0, size - 5);

        // explicitly using signed integers here to represent '-1'
        for (qsizetype i = size - 1; i >= end; i--)
        {
            const auto &message = snapshot[i];
            if (message->loginName == userLogin)
            {
                return message;
            }
        }

        return std::nullopt;
    };

    const auto message = findMessage();
    if (!message)
    {
        return;
    }

    using MessageElementVec = std::vector<std::unique_ptr<MessageElement>>;

    /// Tries to find words in the @a textElement that are emotes in the @a emoteMap
    /// (i.e. newly added emotes) and converts these to an emote element
    /// or, if they're zero-width, to a layered emote element.
    const auto upsertWords = [&](MessageElementVec &elements,
                                 TextElement *textElement) {
        QStringList words;
        bool anyChange = false;

        /// Appends a text element with the pending @a words
        /// and clears the vector.
        ///
        /// @pre @a words must not be empty
        const auto flush = [&]() {
            elements.emplace_back(std::make_unique<TextElement>(
                std::move(words), textElement->getFlags(), textElement->color(),
                textElement->fontStyle()));
            words.clear();
        };

        /// Attempts to insert the emote as a zero-width emote.
        /// If there are pending words to be inserted (i.e. @a words is not empty
        /// and thus there's no previous emote to merge the @a emote with),
        /// or there are no elements in the message yet, the insertion fails.
        ///
        /// @returns `true` iff the insertion succeeded.
        const auto tryInsertZeroWidth = [&](const EmotePtr &emote) -> bool {
            if (!words.empty() || elements.empty())
            {
                // either the last element will be a TextElement _or_
                // there are no elements.
                return false;
            }
            // [THIS IS LARGELY THE SAME AS IN TwitchMessageBuilder::tryAppendEmote]
            // Attempt to merge current zero-width emote into any previous emotes
            auto *asEmote = dynamic_cast<EmoteElement *>(elements.back().get());
            if (asEmote)
            {
                // Make sure to access asEmote before taking ownership when releasing
                auto baseEmote = asEmote->getEmote();
                // Need to remove EmoteElement and replace with LayeredEmoteElement
                auto baseEmoteElement = std::move(elements.back());
                elements.pop_back();

                std::vector<LayeredEmoteElement::Emote> layers{
                    {.ptr = baseEmote, .flags = baseEmoteElement->getFlags()},
                    {.ptr = emote, .flags = MessageElementFlag::Emote},
                };
                elements.emplace_back(std::make_unique<LayeredEmoteElement>(
                    std::move(layers),
                    baseEmoteElement->getFlags() | MessageElementFlag::Emote,
                    textElement->color()));
                return true;
            }

            auto *asLayered =
                dynamic_cast<LayeredEmoteElement *>(elements.back().get());
            if (asLayered)
            {
                asLayered->addEmoteLayer(
                    {.ptr = emote, .flags = MessageElementFlag::Emote});
                asLayered->addFlags(MessageElementFlag::Emote);
                return true;
            }
            return false;
        };

        // Find all words that match a personal emote and replace them with emotes
        const auto prevWords = textElement->words();
        for (const auto &word : prevWords)
        {
            auto emoteIt = emoteMap->find(EmoteName{word});
            if (emoteIt == emoteMap->end())
            {
                words.emplace_back(word);
                continue;
            }
            anyChange = true;

            if (emoteIt->second->zeroWidth)
            {
                if (tryInsertZeroWidth(emoteIt->second))
                {
                    continue;
                }
            }

            flush();

            elements.emplace_back(std::make_unique<EmoteElement>(
                emoteIt->second, MessageElementFlag::Emote));
        }

        if (anyChange)
        {
            flush();
        }
        else
        {
            elements.emplace_back(textElement->clone());
        }
    };

    auto cloned = message.value()->clone();
    // We create a new vector of elements,
    // if we encounter a `TextElement` that contains any emote,
    // we insert an `EmoteElement` (or `LayeredEmoteElement`) at the position.
    MessageElementVec elements;
    elements.reserve(cloned->elements.size());

    std::for_each(
        std::make_move_iterator(cloned->elements.begin()),
        std::make_move_iterator(cloned->elements.end()), [&](auto &&element) {
            MessageElement *elementPtr = element.get();
            auto *textElement = dynamic_cast<TextElement *>(elementPtr);
            auto *linkElement = dynamic_cast<LinkElement *>(elementPtr);
            auto *mentionElement = dynamic_cast<MentionElement *>(elementPtr);

            // Check if this contains the message text
            if (textElement && !linkElement && !mentionElement &&
                textElement->getFlags().has(MessageElementFlag::Text))
            {
                upsertWords(elements, textElement);
            }
            else
            {
                elements.emplace_back(std::forward<decltype(element)>(element));
            }
        });

    cloned->elements = std::move(elements);

    this->replaceMessage(message.value(), cloned);
}

MessagePlatform Channel::messagePlatform() const
{
    return this->messagePlatform_;
}

Channel::Channel(const QString &name, Type type, size_t messagesLimit)
    : lastDate_(QDate::currentDate())
    , name_(name)
    , prefixedName_(QStringLiteral("#") + name)
    , messages_(messagesLimit)
    , type_(type)
{
    if (this->isTwitchChannel())
    {
        this->platform_ = "twitch";
    }
    else if (this->isYouTubeChannel())
    {
        this->platform_ = "youtube";
    }
    else if (this->isTikTokChannel())
    {
        this->platform_ = "tiktok";
    }

    if (this->isKickChannel())
    {
        this->messagePlatform_ = MessagePlatform::Kick;
    }
    else if (this->isYouTubeChannel())
    {
        this->messagePlatform_ = MessagePlatform::YouTube;
    }
    else if (this->isTikTokChannel())
    {
        this->messagePlatform_ = MessagePlatform::TikTok;
    }
    else
    {
        this->messagePlatform_ = MessagePlatform::AnyOrTwitch;
    }
}

TabCompletionModel *Channel::getCompletionModel()
{
    if (!this->completionModel)
    {
        this->completionModel =
            std::make_unique<TabCompletionModel>(*this, nullptr);
    }
    return this->completionModel.get();
}

const QString &Channel::getPrefixedName() const
{
    return this->prefixedName_;
}

bool Channel::isYouTubeChannel() const
{
    return this->type_ == Type::YouTube;
}

bool Channel::isTikTokChannel() const
{
    return this->type_ == Type::TikTok;
}

void Channel::prependMessage(MessagePtr message)
{
    RecursionGuard g{&this->recursionCount_};
    if (!this->canRecurse())
    {
        return;
    }

    message->freeze();

    MessagePtr deleted;
    this->messages_.pushFront(message, deleted);
    this->messagePrepended.invoke(message, deleted);
}

bool Channel::canRecurse() const noexcept
{
    return this->recursionCount_ < MAX_RECURSION;
}

//
// Indirect channel
//
IndirectChannel::Data::Data(ChannelPtr _channel, Channel::Type _type)
    : channel(std::move(_channel))
    , type(_type)
{
}

IndirectChannel::IndirectChannel(ChannelPtr channel, Channel::Type type)
    : data_(std::make_unique<Data>(std::move(channel), type))
{
}

ChannelPtr IndirectChannel::get() const
{
    return this->data_->channel;
}

void IndirectChannel::reset(ChannelPtr channel)
{
    assert(this->data_->type != Channel::Type::Direct);

    this->data_->channel = std::move(channel);
    this->data_->changed.invoke();
}

pajlada::Signals::NoArgSignal &IndirectChannel::getChannelChanged()
{
    return this->data_->changed;
}

Channel::Type IndirectChannel::getType() const
{
    if (this->data_->type == Channel::Type::Direct)
    {
        return this->get()->getType();
    }

    return this->data_->type;
}

}  // namespace chatterino
