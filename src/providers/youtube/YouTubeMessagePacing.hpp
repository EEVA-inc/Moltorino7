#pragma once

#include "providers/youtube/YouTubeTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>

namespace chatterino::youtube {

inline constexpr auto MIN_CONTINUATION_POLL_DELAY =
    std::chrono::milliseconds{250};
inline constexpr auto RECENT_ACTIVITY_POLL_DELAY =
    std::chrono::milliseconds{500};
inline constexpr auto IDLE_CONTINUATION_POLL_DELAY =
    std::chrono::milliseconds{2'000};
inline constexpr auto RECENT_ACTIVITY_POLL_WINDOW =
    std::chrono::milliseconds{1'500};

inline constexpr auto DEFAULT_MESSAGE_DELIVERY_WINDOW =
    std::chrono::milliseconds{500};
inline constexpr auto MIN_MESSAGE_DELIVERY_WINDOW =
    std::chrono::milliseconds{250};
inline constexpr auto MAX_MESSAGE_DELIVERY_WINDOW =
    std::chrono::milliseconds{1'000};
inline constexpr auto MESSAGE_DELIVERY_BREATHING_GAP =
    std::chrono::milliseconds{75};
inline constexpr auto MESSAGE_DELIVERY_CADENCE_RESET =
    std::chrono::milliseconds{2'500};
inline constexpr auto MESSAGE_DELIVERY_WINDOW_HYSTERESIS =
    std::chrono::milliseconds{25};
inline constexpr auto MAX_MESSAGE_DELIVERY_WINDOW_ADJUSTMENT =
    std::chrono::milliseconds{75};
inline constexpr auto MIN_MESSAGE_DELIVERY_INTERVAL =
    std::chrono::milliseconds{16};
inline constexpr auto MAX_MESSAGE_DELIVERY_INTERVAL =
    std::chrono::milliseconds{225};
inline constexpr std::size_t MAX_MESSAGE_DELIVERY_BATCH = 64;

struct MessageDeliveryStep {
    std::size_t batchSize = 0;
    std::chrono::milliseconds delay{0};
};

[[nodiscard]] inline qsizetype messageDeliveryTextCost(
    const YouTubeMessage &message, qsizetype maximum)
{
    if (maximum <= 0)
    {
        return 0;
    }

    qsizetype total = 0;
    const auto add = [&total, maximum](const QString &part) {
        total += std::min(part.size(), maximum - total);
    };
    add(message.text);
    add(message.eventText);
    add(message.amountDisplayString);
    return total;
}

[[nodiscard]] inline std::chrono::milliseconds continuationPollDelay(
    bool receivedMessages, bool recentlyActive,
    std::optional<std::chrono::milliseconds> serverTimeout)
{
    const auto maximum =
        (receivedMessages || recentlyActive)
            ? (receivedMessages ? MIN_CONTINUATION_POLL_DELAY
                                : RECENT_ACTIVITY_POLL_DELAY)
            : IDLE_CONTINUATION_POLL_DELAY;
    return std::clamp(serverTimeout.value_or(maximum),
                      MIN_CONTINUATION_POLL_DELAY, maximum);
}

[[nodiscard]] inline std::chrono::milliseconds nextMessageDeliveryWindow(
    std::chrono::milliseconds previousWindow,
    std::chrono::milliseconds observedBatchInterval)
{
    if (observedBatchInterval <= std::chrono::milliseconds{0} ||
        observedBatchInterval >= MESSAGE_DELIVERY_CADENCE_RESET)
    {
        return DEFAULT_MESSAGE_DELIVERY_WINDOW;
    }

    const auto boundedInterval =
        std::clamp(observedBatchInterval, MIN_MESSAGE_DELIVERY_WINDOW,
                   MAX_MESSAGE_DELIVERY_WINDOW +
                       MESSAGE_DELIVERY_BREATHING_GAP);
    const auto observedWindow = std::clamp(
        boundedInterval - MESSAGE_DELIVERY_BREATHING_GAP,
        MIN_MESSAGE_DELIVERY_WINDOW, MAX_MESSAGE_DELIVERY_WINDOW);

    const auto boundedPrevious =
        std::clamp(previousWindow, MIN_MESSAGE_DELIVERY_WINDOW,
                   MAX_MESSAGE_DELIVERY_WINDOW);
    const auto difference = observedWindow - boundedPrevious;
    if (difference >= -MESSAGE_DELIVERY_WINDOW_HYSTERESIS &&
        difference <= MESSAGE_DELIVERY_WINDOW_HYSTERESIS)
    {
        return boundedPrevious;
    }

    const auto adjustment = std::clamp(
        difference / 2, -MAX_MESSAGE_DELIVERY_WINDOW_ADJUSTMENT,
        MAX_MESSAGE_DELIVERY_WINDOW_ADJUSTMENT);
    return boundedPrevious + adjustment;
}

[[nodiscard]] inline MessageDeliveryStep messageDeliveryStep(
    std::size_t queuedMessages, std::chrono::milliseconds remainingWindow)
{
    if (queuedMessages == 0)
    {
        return {};
    }
    if (remainingWindow <= std::chrono::milliseconds{0})
    {
        return {
            .batchSize =
                std::min(queuedMessages, MAX_MESSAGE_DELIVERY_BATCH),
            .delay = std::chrono::milliseconds{0},
        };
    }

    const auto availableTicks = std::max<std::size_t>(
        1, static_cast<std::size_t>(remainingWindow /
                                    MIN_MESSAGE_DELIVERY_INTERVAL));
    const auto requiredBatch =
        (queuedMessages + availableTicks - 1) / availableTicks;
    const auto batchSize = std::clamp<std::size_t>(
        requiredBatch, 1, MAX_MESSAGE_DELIVERY_BATCH);
    const auto requiredTicks =
        (queuedMessages + batchSize - 1) / batchSize;
    const auto idealDelay = std::chrono::milliseconds{
        remainingWindow.count() /
        static_cast<std::chrono::milliseconds::rep>(requiredTicks)};

    return {
        .batchSize = batchSize,
        .delay = std::clamp(idealDelay, MIN_MESSAGE_DELIVERY_INTERVAL,
                            MAX_MESSAGE_DELIVERY_INTERVAL),
    };
}

}
