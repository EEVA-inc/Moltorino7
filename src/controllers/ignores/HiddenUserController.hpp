#pragma once

#include "common/Atomic.hpp"
#include "common/SignalVector.hpp"
#include "controllers/ignores/HiddenUser.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace chatterino {

struct Message;
enum class MessagePlatform : std::uint8_t;

class HiddenUserController
{
public:
    explicit HiddenUserController(SignalVector<HiddenUser> &users);

    bool isHidden(MessagePlatform platform, const QString &userID,
                  const QString &login = {},
                  const QString &displayName = {}) const;
    bool shouldSuppressHighlights(MessagePlatform platform,
                                  const QString &userID,
                                  const QString &login,
                                  const QString &text) const;
    bool shouldHideMessage(const Message &message) const;
    bool setHidden(HiddenUserPlatform platform, const QString &userID,
                   const QString &login, const QString &displayName,
                   bool hidden);

    pajlada::Signals::NoArgSignal changed;

private:
    struct Cache;
    void rebuild();
    bool isHidden(const Cache &cache, MessagePlatform platform,
                  const QString &userID, const QString &login,
                  const QString &displayName = {}) const;
    bool hasHiddenMention(const Cache &cache, MessagePlatform platform,
                          const QString &text) const;

    SignalVector<HiddenUser> &users_;
    Atomic<std::shared_ptr<const Cache>> cache_;
    std::atomic_bool hasUsers_{false};
    pajlada::Signals::SignalHolder signalHolder_;
};

HiddenUserPlatform hiddenUserPlatform(MessagePlatform platform);

}
