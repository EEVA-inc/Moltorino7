#include "controllers/filters/lang/expressions/IdentifierExpression.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/filters/lang/Filter.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"

#include <QString>
#include <QStringBuilder>

#include <algorithm>
#include <concepts>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

using namespace chatterino;
using namespace filters;
using namespace Qt::StringLiterals;

template <typename T>
QVariant makeVariantFor(T &&value)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_integral_v<Value> && !std::is_same_v<Value, bool>)
    {
        return static_cast<int>(value);
    }
    return QVariant::fromValue(std::forward<T>(value));
}

struct Accessor {
    Accessor(std::invocable<RunContext> auto &&fn)
        : fn([fn = std::forward<decltype(fn)>(fn)](RunContext ctx) {
            return makeVariantFor(fn(ctx));
        })
    {
    }

    Accessor()
        : fn([](RunContext) {
            return false;
        })
    {
    }

    std::function<QVariant(RunContext)> fn;
};

struct IdentifierExpression final : public Expression {
    IdentifierExpression(QString name, std::optional<Type> type,
                         Accessor accessor)
        : name(std::move(name))
        , type(type)
        , accessor(std::move(accessor))
    {
    }

    PossibleType synthesizeType(const TypingContext &) const override
    {
        if (this->type)
        {
            return TypeClass{*this->type};
        }
        return IllTyped{.expr = this,
                        .message = u"Invalid access: " % this->name};
    }

    QString debug(const TypingContext &) const override
    {
        return u"Val(" % this->name % ')';
    }

    QString filterString() const override
    {
        return this->name;
    }

    QVariant execute(RunContext context) const override
    {
        if (context.values)
        {
            return context.values->value(this->name);
        }
        return context.message ? this->accessor.fn(context) : QVariant{};
    }

private:
    QString name;
    std::optional<Type> type;
    Accessor accessor;
};

template <auto Ptr>
auto memberAccessor(RunContext ctx)
{
    return ctx.message->*Ptr;
}

template <MessageFlag Flag>
bool flagAccessor(RunContext ctx)
{
    return ctx.message->flags.has(Flag);
}

using AccessorMap = std::map<QString, std::pair<Type, Accessor>>;
const AccessorMap &accessorMap()
{
    static const AccessorMap map{

        {
            u"author.badges"_s,
            {
                Type::StringList,
                [](RunContext ctx) {
                    QStringList badges;
                    badges.reserve(static_cast<qsizetype>(
                        ctx.message->twitchBadges.size()));
                    for (const auto &e : ctx.message->twitchBadges)
                    {
                        badges.emplace_back(e.key_);
                    }
                    return badges;
                },
            },
        },
        {
            u"author.external_badges"_s,
            {Type::StringList, memberAccessor<&Message::externalBadges>},
        },
        {
            u"author.color"_s,
            {Type::Color, memberAccessor<&Message::usernameColor>},
        },
        {
            u"author.name"_s,
            {Type::String, memberAccessor<&Message::displayName>},
        },
        {
            u"author.user_id"_s,
            {Type::String, memberAccessor<&Message::userID>},
        },
        {
            u"author.no_color"_s,
            {
                Type::Bool,
                [](RunContext ctx) {
                    return !ctx.message->usernameColor.isValid();
                },
            },
        },
        {
            u"author.subbed"_s,
            {
                Type::Bool,
                [](RunContext ctx) {
                    return std::ranges::any_of(
                        ctx.message->twitchBadges, [](const auto &it) {
                            return it.key_ == u"subscriber" ||
                                   it.key_ == u"founder";
                        });
                },
            },
        },
        {
            u"author.sub_length"_s,
            {
                Type::Int,
                [](RunContext ctx) {
                    const auto findBadgeInfo = [&ctx](QStringView name) {
                        return std::ranges::find_if(
                            ctx.message->twitchBadgeInfos,
                            [name](const auto &info) {
                                return info.first == name;
                            });
                    };

                    auto it = findBadgeInfo(u"subscriber");
                    if (it == ctx.message->twitchBadgeInfos.end())
                    {
                        it = findBadgeInfo(u"founder");
                    }
                    if (it == ctx.message->twitchBadgeInfos.end())
                    {
                        return 0;
                    }
                    return it->second.toInt();
                },
            },
        },

        {
            u"bits.amount"_s,
            {Type::Int, memberAccessor<&Message::bits>},
        },

        {
            u"channel.live"_s,
            {
                Type::Bool,
                [](RunContext ctx) {
                    auto *tc = dynamic_cast<TwitchChannel *>(ctx.channel);
                    if (tc)
                    {
                        return tc->isLive();
                    }
                    return false;
                },
            },
        },
        {
            u"channel.name"_s,
            {Type::String, memberAccessor<&Message::channelName>},
        },
        {
            u"channel.watching"_s,
            {
                Type::Bool,
                [](RunContext ctx) {
                    auto chan =
                        getApp()->getTwitch()->getWatchingChannel().get();
                    return !chan->getName().isEmpty() &&
                           chan->getName().compare(ctx.message->channelName,
                                                   Qt::CaseInsensitive) == 0;
                },
            },
        },

        {
            u"flags.action"_s,
            {Type::Bool, flagAccessor<MessageFlag::Action>},
        },
        {
            u"flags.highlighted"_s,
            {Type::Bool, flagAccessor<MessageFlag::Highlighted>},
        },
        {
            u"flags.points_redeemed"_s,
            {Type::Bool, flagAccessor<MessageFlag::RedeemedHighlight>},
        },
        {
            u"flags.sub_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::Subscription>},
        },
        {
            u"flags.system_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::System>},
        },
        {
            u"flags.reward_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::RedeemedChannelPointReward>},
        },
        {
            u"flags.first_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::FirstMessage>},
        },
        {
            u"flags.elevated_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::ElevatedMessage>},
        },
        {
            u"flags.hype_chat"_s,
            {Type::Bool, flagAccessor<MessageFlag::ElevatedMessage>},
        },
        {
            u"flags.cheer_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::CheerMessage>},
        },
        {
            u"flags.whisper"_s,
            {Type::Bool, flagAccessor<MessageFlag::Whisper>},
        },
        {
            u"flags.reply"_s,
            {Type::Bool, flagAccessor<MessageFlag::ReplyMessage>},
        },
        {
            u"flags.automod"_s,
            {Type::Bool, flagAccessor<MessageFlag::AutoMod>},
        },
        {
            u"flags.moderation_action"_s,
            {Type::Bool, flagAccessor<MessageFlag::ModerationAction>},
        },
        {
            u"flags.restricted"_s,
            {Type::Bool, flagAccessor<MessageFlag::RestrictedMessage>},
        },
        {
            u"flags.monitored"_s,
            {Type::Bool, flagAccessor<MessageFlag::MonitoredMessage>},
        },
        {
            u"flags.shared"_s,
            {Type::Bool, flagAccessor<MessageFlag::SharedMessage>},
        },
        {
            u"flags.similar"_s,
            {Type::Bool, flagAccessor<MessageFlag::Similar>},
        },
        {
            u"flags.repeated_message"_s,
            {Type::Bool, flagAccessor<MessageFlag::RepeatedMessage>},
        },
        {
            u"flags.repeated_messages"_s,
            {Type::Bool, flagAccessor<MessageFlag::RepeatedMessage>},
        },
        {
            u"flags.watch_streak"_s,
            {Type::Bool, flagAccessor<MessageFlag::WatchStreak>},
        },
        {
            u"flags.webchat_detected"_s,
            {
                Type::Bool,
                [](RunContext ctx) {
                    return ctx.message->clientDetection ==
                           Message::ClientDetectionStatus::Web;
                },
            },
        },
        {
            u"flags.announcement"_s,
            {Type::Bool, flagAccessor<MessageFlag::Announcement>},
        },
        {
            u"flags.emote_only"_s,
            {Type::Bool, memberAccessor<&Message::emoteOnly>},
        },

        {
            u"message.content"_s,
            {Type::String, memberAccessor<&Message::messageText>},
        },
        {
            u"message.length"_s,
            {
                Type::Int,
                [](RunContext ctx) {
                    return ctx.message->messageText.length();
                },
            },
        },

        {
            u"moltorino.client_detection"_s,
            {
                Type::String,
                [](RunContext ctx) {
                    return Message::clientDetectionStatusToString(
                        ctx.message->clientDetection);
                },
            },
        },

        {
            u"reward.cost"_s,
            {
                Type::Int,
                [](RunContext ctx) {
                    const auto &r = ctx.message->reward;
                    if (r)
                    {
                        return r->cost;
                    }
                    return -1;
                },
            },
        },
        {
            u"reward.id"_s,
            {
                Type::String,
                [](RunContext ctx) {
                    const auto &r = ctx.message->reward;
                    if (r)
                    {
                        return r->id;
                    }
                    return QString{};
                },
            },
        },
        {
            u"reward.title"_s,
            {
                Type::String,
                [](RunContext ctx) {
                    const auto &r = ctx.message->reward;
                    if (r)
                    {
                        return r->title;
                    }
                    return QString{};
                },
            },
        },
    };
    return map;
}

}

namespace chatterino::filters {

const QMap<QString, Type> MESSAGE_TYPING_CONTEXT = [] {
    QMap<QString, Type> types;
    for (const auto &[name, accessor] : accessorMap())
    {
        types.insert(name, accessor.first);
    }
    return types;
}();

ContextMap buildContextMap(const MessagePtr &message, Channel *channel)
{
    ContextMap values;
    if (!message)
    {
        return values;
    }
    RunContext context(*message, channel);
    for (const auto &[name, accessor] : accessorMap())
    {
        values.insert(name, accessor.second.fn(context));
    }
    return values;
}

std::unique_ptr<Expression> createIdentifierExpression(const QString &name)
{
    const auto &map = accessorMap();
    auto it = map.find(name);
    if (it == map.end())
    {
        return std::make_unique<IdentifierExpression>(name, std::nullopt,
                                                      Accessor());
    }
    return std::make_unique<IdentifierExpression>(it->first, it->second.first,
                                                  it->second.second);
}

}
