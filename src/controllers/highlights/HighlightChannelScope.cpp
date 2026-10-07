#include "controllers/highlights/HighlightChannelScope.hpp"

#include "messages/Message.hpp"

#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

QString platformKey(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::YouTube:
            return QStringLiteral("youtube");
        case MessagePlatform::TikTok:
            return QStringLiteral("tiktok");
        case MessagePlatform::Kick:
            return QStringLiteral("kick");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral("twitch");
    }
}

QString channelFromKnownUrl(const QString &input)
{
    if (!input.contains(u'.'))
    {
        return input;
    }

    const QUrl url = QUrl::fromUserInput(input);
    if (!url.isValid() || url.host().isEmpty())
    {
        return input;
    }

    auto host = url.host().toLower();
    if (host.startsWith(QStringLiteral("www.")))
    {
        host.remove(0, 4);
    }
    if (host.startsWith(QStringLiteral("m.")))
    {
        host.remove(0, 2);
    }
    if (host != QStringLiteral("twitch.tv") &&
        host != QStringLiteral("kick.com") &&
        host != QStringLiteral("youtube.com") &&
        host != QStringLiteral("tiktok.com") &&
        host != QStringLiteral("youtu.be"))
    {
        return input;
    }

    const auto parts = url.path().split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
    {
        return input;
    }
    if (host == QStringLiteral("youtube.com") &&
        parts.front() == QStringLiteral("channel") && parts.size() >= 2)
    {
        return parts.at(1);
    }
    if (host == QStringLiteral("youtube.com") &&
        parts.front() == QStringLiteral("watch"))
    {
        const auto videoID = QUrlQuery(url).queryItemValue(QStringLiteral("v"));
        return videoID.isEmpty() ? input : videoID;
    }
    if (host == QStringLiteral("youtube.com") &&
        parts.front().startsWith(u'@'))
    {
        return parts.front();
    }
    if (host == QStringLiteral("youtube.com") && parts.size() >= 2 &&
        (parts.front() == QStringLiteral("c") ||
         parts.front() == QStringLiteral("user") ||
         parts.front() == QStringLiteral("live")))
    {
        return parts.at(1);
    }
    if (host == QStringLiteral("tiktok.com"))
    {
        return parts.front();
    }
    if (host == QStringLiteral("twitch.tv") ||
        host == QStringLiteral("kick.com"))
    {
        return parts.front();
    }
    return parts.back();
}

QString cleanHighlightChannelName(QString channel)
{
    channel = channelFromKnownUrl(channel.trimmed()).trimmed();

    for (const auto prefix :
         {QStringLiteral(":youtube:"), QStringLiteral(":kick:"),
          QStringLiteral(":tiktok:"), QStringLiteral(":twitch:")})
    {
        if (channel.startsWith(prefix, Qt::CaseInsensitive))
        {
            channel.remove(0, prefix.size());
            break;
        }
    }
    for (const auto prefix :
         {QStringLiteral("handle:"), QStringLiteral("channel:"),
          QStringLiteral("video:")})
    {
        if (channel.startsWith(prefix, Qt::CaseInsensitive))
        {
            channel.remove(0, prefix.size());
            break;
        }
    }
    while (channel.startsWith(u'#') || channel.startsWith(u'@'))
    {
        channel.remove(0, 1);
    }
    while (channel.endsWith(u'/'))
    {
        channel.chop(1);
    }
    return channel.trimmed();
}

}

QString highlightChannelScopeModeKey(HighlightChannelScopeMode mode)
{
    switch (mode)
    {
        case HighlightChannelScopeMode::OnlySelected:
            return QStringLiteral("only");
        case HighlightChannelScopeMode::ExcludeSelected:
            return QStringLiteral("except");
        case HighlightChannelScopeMode::Everywhere:
        default:
            return QStringLiteral("everywhere");
    }
}

QString highlightChannelScopeModeName(HighlightChannelScopeMode mode)
{
    switch (mode)
    {
        case HighlightChannelScopeMode::OnlySelected:
            return QStringLiteral("Only selected channels");
        case HighlightChannelScopeMode::ExcludeSelected:
            return QStringLiteral("All except selected channels");
        case HighlightChannelScopeMode::Everywhere:
        default:
            return QStringLiteral("All channels");
    }
}

HighlightChannelScopeMode highlightChannelScopeModeFromKey(const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("only") ||
        normalized == QStringLiteral("include"))
    {
        return HighlightChannelScopeMode::OnlySelected;
    }
    if (normalized == QStringLiteral("except") ||
        normalized == QStringLiteral("exclude"))
    {
        return HighlightChannelScopeMode::ExcludeSelected;
    }
    return HighlightChannelScopeMode::Everywhere;
}

QString highlightChannelTargetPlatformKey(
    HighlightChannelTargetPlatform platform)
{
    switch (platform)
    {
        case HighlightChannelTargetPlatform::Twitch:
            return QStringLiteral("twitch");
        case HighlightChannelTargetPlatform::YouTube:
            return QStringLiteral("youtube");
        case HighlightChannelTargetPlatform::Kick:
            return QStringLiteral("kick");
        case HighlightChannelTargetPlatform::TikTok:
            return QStringLiteral("tiktok");
        case HighlightChannelTargetPlatform::Any:
        default:
            return QStringLiteral("any");
    }
}

QString highlightChannelTargetPlatformName(
    HighlightChannelTargetPlatform platform)
{
    switch (platform)
    {
        case HighlightChannelTargetPlatform::Twitch:
            return QStringLiteral("Twitch");
        case HighlightChannelTargetPlatform::YouTube:
            return QStringLiteral("YouTube");
        case HighlightChannelTargetPlatform::Kick:
            return QStringLiteral("Kick");
        case HighlightChannelTargetPlatform::TikTok:
            return QStringLiteral("TikTok");
        case HighlightChannelTargetPlatform::Any:
        default:
            return QStringLiteral("Any platform");
    }
}

HighlightChannelTargetPlatform highlightChannelTargetPlatformFromKey(
    const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("twitch"))
    {
        return HighlightChannelTargetPlatform::Twitch;
    }
    if (normalized == QStringLiteral("youtube"))
    {
        return HighlightChannelTargetPlatform::YouTube;
    }
    if (normalized == QStringLiteral("kick"))
    {
        return HighlightChannelTargetPlatform::Kick;
    }
    if (normalized == QStringLiteral("tiktok"))
    {
        return HighlightChannelTargetPlatform::TikTok;
    }
    return HighlightChannelTargetPlatform::Any;
}

QString normalizeHighlightChannelName(QString channel)
{
    return cleanHighlightChannelName(std::move(channel)).toCaseFolded();
}

std::optional<HighlightChannelTarget> makeHighlightChannelTarget(
    HighlightChannelTargetPlatform platform, QString channel)
{
    channel = cleanHighlightChannelName(std::move(channel));
    if (channel.isEmpty())
    {
        return std::nullopt;
    }
    return HighlightChannelTarget{platform, std::move(channel)};
}

QString encodeHighlightChannelTarget(const HighlightChannelTarget &target)
{
    return highlightChannelTargetPlatformKey(target.platform) + u':' +
           target.channel;
}

std::optional<HighlightChannelTarget> decodeHighlightChannelTarget(
    const QString &encoded)
{
    auto platform = HighlightChannelTargetPlatform::Any;
    auto channel = encoded.trimmed();
    const auto separator = channel.indexOf(u':');
    if (separator > 0)
    {
        const auto prefix = channel.left(separator).toLower();
        if (prefix == QStringLiteral("any") ||
            prefix == QStringLiteral("all") ||
            prefix == QStringLiteral("twitch") ||
            prefix == QStringLiteral("youtube") ||
            prefix == QStringLiteral("kick") ||
            prefix == QStringLiteral("tiktok"))
        {
            platform = highlightChannelTargetPlatformFromKey(prefix);
            channel.remove(0, separator + 1);
        }
    }
    else if (channel.startsWith(QStringLiteral(":youtube:"),
                                Qt::CaseInsensitive))
    {
        platform = HighlightChannelTargetPlatform::YouTube;
    }
    else if (channel.startsWith(QStringLiteral(":kick:"),
                                Qt::CaseInsensitive))
    {
        platform = HighlightChannelTargetPlatform::Kick;
    }
    else if (channel.startsWith(QStringLiteral(":tiktok:"),
                                Qt::CaseInsensitive))
    {
        platform = HighlightChannelTargetPlatform::TikTok;
    }
    else if (channel.startsWith(QStringLiteral(":twitch:"),
                                Qt::CaseInsensitive))
    {
        platform = HighlightChannelTargetPlatform::Twitch;
    }
    return makeHighlightChannelTarget(platform, std::move(channel));
}

QString highlightChannelScopeSummary(HighlightChannelScopeMode mode,
                                     std::size_t targetCount)
{
    if (mode == HighlightChannelScopeMode::Everywhere)
    {
        return QStringLiteral("Everywhere");
    }
    const auto count = QString::number(targetCount);
    const auto noun = targetCount == 1 ? QStringLiteral("channel")
                                       : QStringLiteral("channels");
    return mode == HighlightChannelScopeMode::OnlySelected
               ? QStringLiteral("%1 %2").arg(count, noun)
               : QStringLiteral("All except %1").arg(count);
}

QString highlightChannelScopeDescription(
    HighlightChannelScopeMode mode,
    const std::vector<HighlightChannelTarget> &targets)
{
    QString heading;
    if (mode == HighlightChannelScopeMode::Everywhere)
    {
        heading = QStringLiteral("Applies in all channels");
    }
    else
    {
        const auto count = QString::number(targets.size());
        const auto noun = targets.size() == 1 ? QStringLiteral("channel")
                                               : QStringLiteral("channels");
        heading = mode == HighlightChannelScopeMode::OnlySelected
                      ? QStringLiteral("Applies only in %1 selected %2")
                            .arg(count, noun)
                      : QStringLiteral(
                            "Applies in all channels except %1 selected %2")
                            .arg(count, noun);
    }
    QStringList lines{std::move(heading)};
    constexpr std::size_t MAX_VISIBLE_TARGETS = 20;
    for (std::size_t index = 0;
         index < std::min(targets.size(), MAX_VISIBLE_TARGETS); ++index)
    {
        const auto &target = targets.at(index);
        lines.append(QStringLiteral("%1: %2")
                         .arg(highlightChannelTargetPlatformName(
                                  target.platform),
                              target.channel));
    }
    if (targets.size() > MAX_VISIBLE_TARGETS)
    {
        lines.append(QStringLiteral("and %1 more")
                         .arg(targets.size() - MAX_VISIBLE_TARGETS));
    }
    return lines.join(u'\n');
}

HighlightChannelScope::HighlightChannelScope(
    HighlightChannelScopeMode mode,
    std::vector<HighlightChannelTarget> targets)
    : mode_(mode)
{
    for (auto &target : targets)
    {
        auto normalized =
            makeHighlightChannelTarget(target.platform, std::move(target.channel));
        if (!normalized)
        {
            continue;
        }
        const auto key =
            targetKey(normalized->platform,
                      normalizeHighlightChannelName(normalized->channel));
        if (this->targetKeys_.contains(key))
        {
            continue;
        }
        this->targetKeys_.insert(key);
        this->targets_.emplace_back(std::move(*normalized));
    }
}

bool HighlightChannelScope::operator==(
    const HighlightChannelScope &other) const
{
    return this->mode_ == other.mode_ && this->targets_ == other.targets_;
}

HighlightChannelScopeMode HighlightChannelScope::mode() const
{
    return this->mode_;
}

const std::vector<HighlightChannelTarget> &HighlightChannelScope::targets()
    const
{
    return this->targets_;
}

bool HighlightChannelScope::appliesTo(MessagePlatform platform,
                                      const QString &channelName) const
{
    return this->appliesToNormalized(
        platform, normalizeHighlightChannelName(channelName));
}

bool HighlightChannelScope::appliesToNormalized(
    MessagePlatform platform, const QString &normalizedChannelName) const
{
    if (this->mode_ == HighlightChannelScopeMode::Everywhere)
    {
        return true;
    }

    const bool selected =
        !normalizedChannelName.isEmpty() &&
        (this->targetKeys_.contains(
             targetKey(HighlightChannelTargetPlatform::Any,
                       normalizedChannelName)) ||
         this->targetKeys_.contains(platformKey(platform) + u':' +
                                    normalizedChannelName));
    return this->mode_ == HighlightChannelScopeMode::OnlySelected ? selected
                                                                  : !selected;
}

QString HighlightChannelScope::targetKey(
    HighlightChannelTargetPlatform platform,
    const QString &normalizedChannelName)
{
    return highlightChannelTargetPlatformKey(platform) + u':' +
           normalizedChannelName;
}

}
