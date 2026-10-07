#pragma once

#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace chatterino {

enum class MessagePlatform : std::uint8_t;

enum class HighlightChannelScopeMode : std::uint8_t {
    Everywhere,
    OnlySelected,
    ExcludeSelected,
};

enum class HighlightChannelTargetPlatform : std::uint8_t {
    Any,
    Twitch,
    YouTube,
    Kick,
    TikTok,
};

struct HighlightChannelTarget {
    HighlightChannelTargetPlatform platform{
        HighlightChannelTargetPlatform::Any};

    QString channel;

    bool operator==(const HighlightChannelTarget &other) const = default;
};

QString highlightChannelScopeModeKey(HighlightChannelScopeMode mode);
QString highlightChannelScopeModeName(HighlightChannelScopeMode mode);
HighlightChannelScopeMode highlightChannelScopeModeFromKey(
    const QString &key);

QString highlightChannelTargetPlatformKey(
    HighlightChannelTargetPlatform platform);
QString highlightChannelTargetPlatformName(
    HighlightChannelTargetPlatform platform);
HighlightChannelTargetPlatform highlightChannelTargetPlatformFromKey(
    const QString &key);

QString normalizeHighlightChannelName(QString channel);

std::optional<HighlightChannelTarget> makeHighlightChannelTarget(
    HighlightChannelTargetPlatform platform, QString channel);
QString encodeHighlightChannelTarget(const HighlightChannelTarget &target);
std::optional<HighlightChannelTarget> decodeHighlightChannelTarget(
    const QString &encoded);

QString highlightChannelScopeSummary(HighlightChannelScopeMode mode,
                                     std::size_t targetCount);
QString highlightChannelScopeDescription(
    HighlightChannelScopeMode mode,
    const std::vector<HighlightChannelTarget> &targets);

class HighlightChannelScope
{
public:
    HighlightChannelScope(
        HighlightChannelScopeMode mode =
            HighlightChannelScopeMode::Everywhere,
        std::vector<HighlightChannelTarget> targets = {});

    bool operator==(const HighlightChannelScope &other) const;

    HighlightChannelScopeMode mode() const;
    const std::vector<HighlightChannelTarget> &targets() const;

    bool appliesTo(MessagePlatform platform, const QString &channelName) const;
    bool appliesToNormalized(MessagePlatform platform,
                             const QString &normalizedChannelName) const;

private:
    static QString targetKey(HighlightChannelTargetPlatform platform,
                             const QString &normalizedChannelName);

    HighlightChannelScopeMode mode_{HighlightChannelScopeMode::Everywhere};
    std::vector<HighlightChannelTarget> targets_;
    QSet<QString> targetKeys_;
};

}
