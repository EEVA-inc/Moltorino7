#pragma once

#include <cstdint>

namespace chatterino {

enum class HelixAnnouncementColor : std::uint8_t {
    Blue,
    Green,
    Orange,
    Purple,

    Primary,
};

enum class HelixClipError : std::uint8_t {
    Unknown,
    ClipsUnavailable,
    ClipsDisabled,
    ClipsRestricted,
    ClipsRestrictedCategory,
    UserNotAuthenticated,
};

enum class HelixStreamMarkerError : std::uint8_t {
    Unknown,
    UserNotAuthorized,
    UserNotAuthenticated,
};

enum class HelixAutoModMessageError : std::uint8_t {
    Unknown,
    MessageAlreadyProcessed,
    UserNotAuthenticated,
    UserNotAuthorized,
    MessageNotFound,
};

enum class HelixUpdateUserChatColorError : std::uint8_t {
    Unknown,
    UserMissingScope,
    InvalidColor,

    Forwarded,
};

enum class HelixDeleteChatMessagesError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthenticated,
    UserNotAuthorized,
    MessageUnavailable,

    Forwarded,
};

enum class HelixSendChatAnnouncementError : std::uint8_t {
    Unknown,
    UserMissingScope,

    Forwarded,
};

enum class HelixAddChannelModeratorError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    TargetAlreadyModded,
    TargetIsVIP,

    Forwarded,
};

enum class HelixRemoveChannelModeratorError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    TargetNotModded,
    Ratelimited,

    Forwarded,
};

enum class HelixAddChannelVIPError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,

    Forwarded,
};

enum class HelixRemoveChannelVIPError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,

    Forwarded,
};

enum class HelixUnbanUserError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    ConflictingOperation,
    TargetNotBanned,

    Forwarded,
};

enum class HelixStartRaidError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    CantRaidYourself,
    Ratelimited,

    Forwarded,
};

enum class HelixCancelRaidError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    NoRaidPending,
    Ratelimited,

    Forwarded,
};

enum class HelixUpdateChatSettingsError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    Forbidden,
    OutOfRange,

    Forwarded,
};

enum class HelixUpdateChannelError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,

    Forwarded,
};

enum class HelixBanUserError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    ConflictingOperation,
    TargetBanned,
    CannotBanUser,

    Forwarded,
};

enum class HelixWarnUserError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    ConflictingOperation,
    CannotWarnUser,

    Forwarded,
};

enum class HelixWhisperError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    Ratelimited,
    NoVerifiedPhone,
    RecipientBlockedUser,
    WhisperSelf,

    Forwarded,
};

enum class HelixGetChattersError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,

    Forwarded,
};

enum class HelixGetModeratorsError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,

    Forwarded,
};

enum class HelixListVIPsError : std::uint8_t {
    Unknown,
    UserMissingScope,
    UserNotAuthorized,
    UserNotBroadcaster,
    Ratelimited,

    Forwarded,
};

enum class HelixSendShoutoutError : std::uint8_t {
    Unknown,
    UserIsBroadcaster,
    BroadcasterNotLive,
    UserNotAuthorized,
    UserMissingScope,

    Ratelimited,
};

enum class HelixUpdateShieldModeError : std::uint8_t {
    Unknown,
    UserMissingScope,
    MissingPermission,

    Forwarded,
};

enum class HelixStartCommercialError : std::uint8_t {
    Unknown,
    TokenMustMatchBroadcaster,
    UserMissingScope,
    BroadcasterNotStreaming,
    MissingLengthParameter,
    Ratelimited,

    Forwarded,
};

enum class HelixGetGlobalBadgesError : std::uint8_t {
    Unknown,

    Forwarded,
};

enum class HelixSendMessageError : std::uint8_t {
    Unknown,

    MissingText,
    BadRequest,
    Forbidden,
    MessageTooLarge,
    UserMissingScope,

    Forwarded,
};

enum class HelixCreateEventSubSubscriptionError : std::uint8_t {
    BadRequest,
    Unauthorized,
    Forbidden,
    Conflict,
    Ratelimited,
    NoSession,

    Forwarded,
};

enum class HelixPinMessageError : std::uint8_t {
    Unknown,

    InvalidParameter,
    MissingScope,
    Forbidden,
    NotFound,
    Conflict,
    RateLimited,

    Forwarded,
};

}
