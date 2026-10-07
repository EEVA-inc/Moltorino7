configure_file(
    providers/youtube/YouTubeCredentials.hpp.in
    "${CMAKE_BINARY_DIR}/autogen/providers/youtube/YouTubeCredentials.hpp"
)

set(LIBRARY_PROJECT "${PROJECT_NAME}-lib")
set(VERSION_PROJECT "${LIBRARY_PROJECT}-version")
set(EXECUTABLE_PROJECT "${PROJECT_NAME}")
# used for QT_DISABLE_DEPRECATED_BEFORE/QT_WARN_DEPRECATED_UP_TO
set(CHATTERINO_MIN_QT_VERSION 0x060403)

# registers the native messageing host
option(CHATTERINO_DEBUG_NATIVE_MESSAGES "Debug native messages" OFF)
option(CHATTERINO_STATIC_QT_BUILD "Static link Qt" OFF)

set(SOURCE_FILES
        Application.cpp
        Application.hpp
        BrowserExtension.cpp
        BrowserExtension.hpp
        RunGui.cpp
        RunGui.hpp

        common/AccountCredentials.cpp
        common/AccountCredentials.hpp
        common/Args.cpp
        common/Args.hpp
        common/ChannelChatters.cpp
        common/ChannelChatters.hpp
        common/Channel.cpp
        common/Channel.hpp
        common/ChatterinoSetting.cpp
        common/ChatterinoSetting.hpp
        common/ChatterSet.cpp
        common/ChatterSet.hpp
        common/DiagnosticPrivacy.hpp
        common/Env.cpp
        common/Env.hpp
        common/LastMessageLineStyle.hpp
        common/LinkParser.cpp
        common/LinkParser.hpp
        common/Literals.hpp
        common/Modes.cpp
        common/Modes.hpp
        common/QLogging.cpp
        common/QLogging.hpp
        common/SecureCredentials.hpp
        common/ThumbnailPreviewMode.hpp
        common/TimeoutStackStyle.hpp
        common/WindowDescriptors.cpp
        common/WindowDescriptors.hpp

        common/enums/MessageContext.hpp
        common/enums/MessageOverflow.hpp

        common/network/NetworkCommon.cpp
        common/network/NetworkCommon.hpp
        common/network/NetworkManager.cpp
        common/network/NetworkManager.hpp
        common/network/NetworkPrivate.cpp
        common/network/NetworkPrivate.hpp
        common/network/NetworkRequest.cpp
        common/network/NetworkRequest.hpp
        common/network/NetworkResult.cpp
        common/network/NetworkResult.hpp
        common/network/NetworkTask.cpp
        common/network/NetworkTask.hpp

        common/websockets/WebSocketPool.cpp
        common/websockets/WebSocketPool.hpp
        common/websockets/detail/BalancedResolverResults.cpp
        common/websockets/detail/BalancedResolverResults.hpp
        common/websockets/detail/WebSocketConnection.cpp
        common/websockets/detail/WebSocketConnection.hpp
        common/websockets/detail/WebSocketConnectionImpl.cpp
        common/websockets/detail/WebSocketConnectionImpl.hpp
        common/websockets/detail/WebSocketPoolImpl.cpp
        common/websockets/detail/WebSocketPoolImpl.hpp

        controllers/accounts/Account.cpp
        controllers/accounts/Account.hpp
        controllers/accounts/AccountController.cpp
        controllers/accounts/AccountController.hpp
        controllers/accounts/AccountModel.cpp
        controllers/accounts/AccountModel.hpp

        controllers/automod/AutoModReviewController.cpp
        controllers/automod/AutoModReviewController.hpp
        controllers/chat/ChatAutomationController.cpp
        controllers/chat/ChatAutomationController.hpp
        controllers/commands/builtin/chatterino/Debugging.cpp
        controllers/commands/builtin/chatterino/Debugging.hpp
        controllers/commands/builtin/Misc.cpp
        controllers/commands/builtin/Misc.hpp
        controllers/commands/builtin/kick/KickRawEvent.cpp
        controllers/commands/builtin/kick/KickRawEvent.hpp
        controllers/commands/builtin/kick/ModerationActions.cpp
        controllers/commands/builtin/kick/ModerationActions.hpp
        controllers/commands/builtin/twitch/AddModerator.cpp
        controllers/commands/builtin/twitch/AddModerator.hpp
        controllers/commands/builtin/twitch/AddVIP.cpp
        controllers/commands/builtin/twitch/AddVIP.hpp
        controllers/commands/builtin/twitch/Announce.cpp
        controllers/commands/builtin/twitch/Announce.hpp
        controllers/commands/builtin/twitch/Ban.cpp
        controllers/commands/builtin/twitch/Ban.hpp
        controllers/commands/builtin/twitch/Block.cpp
        controllers/commands/builtin/twitch/Block.hpp
        controllers/commands/builtin/twitch/BlockedTerms.cpp
        controllers/commands/builtin/twitch/BlockedTerms.hpp
        controllers/commands/builtin/twitch/ChannelPoints.cpp
        controllers/commands/builtin/twitch/ChannelPoints.hpp
        controllers/commands/builtin/twitch/ChatSettings.cpp
        controllers/commands/builtin/twitch/ChatSettings.hpp
        controllers/commands/builtin/twitch/Chatters.cpp
        controllers/commands/builtin/twitch/Chatters.hpp
        controllers/commands/builtin/twitch/DeleteMessages.cpp
        controllers/commands/builtin/twitch/DeleteMessages.hpp
        controllers/commands/builtin/twitch/GetModerators.cpp
        controllers/commands/builtin/twitch/GetModerators.hpp
        controllers/commands/builtin/twitch/GetFounders.cpp
        controllers/commands/builtin/twitch/GetFounders.hpp
        controllers/commands/builtin/twitch/GetVIPs.cpp
        controllers/commands/builtin/twitch/GetVIPs.hpp
        controllers/commands/builtin/twitch/InvisibleMessage.cpp
        controllers/commands/builtin/twitch/InvisibleMessage.hpp
        controllers/commands/builtin/twitch/LowTrust.cpp
        controllers/commands/builtin/twitch/LowTrust.hpp
        controllers/commands/builtin/twitch/ModVipActions.cpp
        controllers/commands/builtin/twitch/ModVipActions.hpp
        controllers/commands/builtin/twitch/Nuke.cpp
        controllers/commands/builtin/twitch/Nuke.hpp
        controllers/commands/builtin/twitch/Pin.cpp
        controllers/commands/builtin/twitch/Pin.hpp
        controllers/commands/builtin/twitch/Poll.cpp
        controllers/commands/builtin/twitch/Poll.hpp
        controllers/commands/builtin/twitch/Prediction.cpp
        controllers/commands/builtin/twitch/Prediction.hpp
        controllers/commands/builtin/twitch/Raid.cpp
        controllers/commands/builtin/twitch/Raid.hpp
        controllers/commands/builtin/twitch/RemoveModerator.cpp
        controllers/commands/builtin/twitch/RemoveModerator.hpp
        controllers/commands/builtin/twitch/RemoveVIP.cpp
        controllers/commands/builtin/twitch/RemoveVIP.hpp
        controllers/commands/builtin/twitch/SendReply.cpp
        controllers/commands/builtin/twitch/SendReply.hpp
        controllers/commands/builtin/twitch/SendWhisper.cpp
        controllers/commands/builtin/twitch/SendWhisper.hpp
        controllers/commands/builtin/twitch/ShieldMode.cpp
        controllers/commands/builtin/twitch/ShieldMode.hpp
        controllers/commands/builtin/twitch/Shoutout.cpp
        controllers/commands/builtin/twitch/Shoutout.hpp
        controllers/commands/builtin/twitch/StartCommercial.cpp
        controllers/commands/builtin/twitch/StartCommercial.hpp
        controllers/commands/builtin/twitch/Unban.cpp
        controllers/commands/builtin/twitch/Unban.hpp
        controllers/commands/builtin/twitch/UpdateChannel.cpp
        controllers/commands/builtin/twitch/UpdateChannel.hpp
        controllers/commands/builtin/twitch/UpdateColor.cpp
        controllers/commands/builtin/twitch/UpdateColor.hpp
        controllers/commands/builtin/twitch/Warn.cpp
        controllers/commands/builtin/twitch/Warn.hpp
        controllers/commands/builtin/youtube/ModerationActions.cpp
        controllers/commands/builtin/youtube/ModerationActions.hpp
        controllers/commands/common/ChannelAction.cpp
        controllers/commands/common/ChannelAction.hpp
        controllers/commands/CommandContext.hpp
        controllers/commands/CommandController.cpp
        controllers/commands/CommandController.hpp
        controllers/commands/Command.cpp
        controllers/commands/Command.hpp
        controllers/commands/CommandModel.cpp
        controllers/commands/CommandModel.hpp

        controllers/completion/CommandUsage.cpp
        controllers/completion/CommandUsage.hpp
        controllers/completion/CompletionModel.cpp
        controllers/completion/CompletionModel.hpp
        controllers/completion/sources/Source.hpp
        controllers/completion/sources/CommandSource.cpp
        controllers/completion/sources/CommandSource.hpp
        controllers/completion/sources/EmoteSource.cpp
        controllers/completion/sources/EmoteSource.hpp
        controllers/completion/sources/Helpers.hpp
        controllers/completion/sources/UnifiedSource.cpp
        controllers/completion/sources/UnifiedSource.hpp
        controllers/completion/sources/UserSource.cpp
        controllers/completion/sources/UserSource.hpp
        controllers/completion/strategies/ClassicEmoteStrategy.cpp
        controllers/completion/strategies/ClassicEmoteStrategy.hpp
        controllers/completion/strategies/ClassicUserStrategy.cpp
        controllers/completion/strategies/ClassicUserStrategy.hpp
        controllers/completion/strategies/CommandStrategy.cpp
        controllers/completion/strategies/CommandStrategy.hpp
        controllers/completion/strategies/SmartEmoteStrategy.cpp
        controllers/completion/strategies/SmartEmoteStrategy.cpp
        controllers/completion/strategies/Strategy.hpp
        controllers/completion/TabCompletionModel.cpp
        controllers/completion/TabCompletionModel.hpp

        controllers/emotes/EmoteController.cpp
        controllers/emotes/EmoteController.hpp

        controllers/filters/FilterModel.cpp
        controllers/filters/FilterModel.hpp
        controllers/filters/FilterRecord.cpp
        controllers/filters/FilterRecord.hpp
        controllers/filters/FilterSet.cpp
        controllers/filters/FilterSet.hpp
        controllers/filters/lang/expressions/Expression.cpp
        controllers/filters/lang/expressions/Expression.hpp
        controllers/filters/lang/expressions/BinaryOperation.cpp
        controllers/filters/lang/expressions/BinaryOperation.hpp
        controllers/filters/lang/expressions/IdentifierExpression.cpp
        controllers/filters/lang/expressions/IdentifierExpression.hpp
        controllers/filters/lang/expressions/ListExpression.cpp
        controllers/filters/lang/expressions/ListExpression.hpp
        controllers/filters/lang/expressions/RegexExpression.cpp
        controllers/filters/lang/expressions/RegexExpression.hpp
        controllers/filters/lang/expressions/UnaryOperation.hpp
        controllers/filters/lang/expressions/UnaryOperation.cpp
        controllers/filters/lang/expressions/ValueExpression.cpp
        controllers/filters/lang/expressions/ValueExpression.hpp
        controllers/filters/lang/Filter.cpp
        controllers/filters/lang/Filter.hpp
        controllers/filters/lang/FilterParser.cpp
        controllers/filters/lang/FilterParser.hpp
        controllers/filters/lang/Tokenizer.cpp
        controllers/filters/lang/Tokenizer.hpp
        controllers/filters/lang/Types.cpp
        controllers/filters/lang/Types.hpp

        controllers/highlights/BadgeHighlightModel.cpp
        controllers/highlights/BadgeHighlightModel.hpp
        controllers/highlights/ChattyHighlightImport.cpp
        controllers/highlights/ChattyHighlightImport.hpp
        controllers/highlights/HighlightBadge.cpp
        controllers/highlights/HighlightBadge.hpp
        controllers/highlights/HighlightBlacklistModel.cpp
        controllers/highlights/HighlightBlacklistModel.hpp
        controllers/highlights/HighlightChannelScope.cpp
        controllers/highlights/HighlightChannelScope.hpp
        controllers/highlights/HighlightCheck.hpp
        controllers/highlights/HighlightController.cpp
        controllers/highlights/HighlightController.hpp
        controllers/highlights/HighlightModel.cpp
        controllers/highlights/HighlightModel.hpp
        controllers/highlights/HighlightPhrase.cpp
        controllers/highlights/HighlightPhrase.hpp
        controllers/highlights/HighlightResult.cpp
        controllers/highlights/HighlightResult.hpp
        controllers/highlights/HighlightWordList.cpp
        controllers/highlights/HighlightWordList.hpp
        controllers/highlights/UserHighlightModel.cpp
        controllers/highlights/UserHighlightModel.hpp

        controllers/hotkeys/ActionNames.hpp
        controllers/hotkeys/Hotkey.cpp
        controllers/hotkeys/Hotkey.hpp
        controllers/hotkeys/HotkeyCategory.hpp
        controllers/hotkeys/HotkeyController.cpp
        controllers/hotkeys/HotkeyController.hpp
        controllers/hotkeys/HotkeyHelpers.cpp
        controllers/hotkeys/HotkeyHelpers.hpp
        controllers/hotkeys/HotkeyModel.cpp
        controllers/hotkeys/HotkeyModel.hpp

        controllers/ignores/HiddenUser.cpp
        controllers/ignores/HiddenUser.hpp
        controllers/ignores/HiddenUserController.cpp
        controllers/ignores/HiddenUserController.hpp
        controllers/ignores/IgnoreController.cpp
        controllers/ignores/IgnoreController.hpp
        controllers/ignores/IgnoreModel.cpp
        controllers/ignores/IgnoreModel.hpp
        controllers/ignores/IgnorePhrase.cpp
        controllers/ignores/IgnorePhrase.hpp

        controllers/moderationactions/ModerationAction.cpp
        controllers/moderationactions/ModerationAction.hpp
        controllers/moderationactions/ModerationActionModel.cpp
        controllers/moderationactions/ModerationActionModel.hpp

        controllers/logging/ChannelLog.cpp
        controllers/logging/ChannelLog.hpp
        controllers/logging/ChannelLoggingModel.cpp
        controllers/logging/ChannelLoggingModel.hpp

        controllers/nicknames/NicknamesModel.cpp
        controllers/nicknames/NicknamesModel.hpp
        controllers/nicknames/Nickname.hpp

        controllers/notifications/NotificationController.cpp
        controllers/notifications/NotificationController.hpp
        controllers/notifications/NotificationModel.cpp
        controllers/notifications/NotificationModel.hpp

        controllers/pings/MutedChannelModel.cpp
        controllers/pings/MutedChannelModel.hpp

        controllers/recording/ChatRecordingController.cpp
        controllers/recording/ChatRecordingController.hpp
        controllers/recording/ChatRecordingMessage.cpp
        controllers/recording/ChatRecordingMessage.hpp
        controllers/recording/ChatRecordingWriter.cpp
        controllers/recording/ChatRecordingWriter.hpp
        controllers/tray/TrayController.cpp
        controllers/tray/TrayController.hpp


        controllers/plugins/api/Accounts.cpp
        controllers/plugins/api/Accounts.hpp
        controllers/plugins/api/ChannelRef.cpp
        controllers/plugins/api/ChannelRef.hpp
        controllers/plugins/api/ConnectionHandle.cpp
        controllers/plugins/api/ConnectionHandle.hpp
        controllers/plugins/api/DateTime.cpp
        controllers/plugins/api/DateTime.hpp
        controllers/plugins/api/DebugLibrary.cpp
        controllers/plugins/api/DebugLibrary.hpp
        controllers/plugins/api/EventType.hpp
        controllers/plugins/api/HTTPRequest.cpp
        controllers/plugins/api/HTTPRequest.hpp
        controllers/plugins/api/HTTPResponse.cpp
        controllers/plugins/api/HTTPResponse.hpp
        controllers/plugins/api/IOWrapper.cpp
        controllers/plugins/api/IOWrapper.hpp
        controllers/plugins/api/Images.cpp
        controllers/plugins/api/Images.hpp
        controllers/plugins/api/JSON.cpp
        controllers/plugins/api/JSON.hpp
        controllers/plugins/api/JSONParse.cpp
        controllers/plugins/api/JSONParse.hpp
        controllers/plugins/api/JSONStringify.cpp
        controllers/plugins/api/JSONStringify.hpp
        controllers/plugins/api/Menu.cpp
        controllers/plugins/api/Menu.hpp
        controllers/plugins/api/Message.cpp
        controllers/plugins/api/Message.hpp
        controllers/plugins/api/WebSocket.cpp
        controllers/plugins/api/WebSocket.hpp
        controllers/plugins/api/WindowManager.cpp
        controllers/plugins/api/WindowManager.hpp
        controllers/plugins/ConnectionManager.cpp
        controllers/plugins/ConnectionManager.hpp
        controllers/plugins/LuaAPI.cpp
        controllers/plugins/LuaAPI.hpp
        controllers/plugins/LuaUtilities.cpp
        controllers/plugins/LuaUtilities.hpp
        controllers/plugins/PluginController.cpp
        controllers/plugins/PluginController.hpp
        controllers/plugins/Plugin.cpp
        controllers/plugins/Plugin.hpp
        controllers/plugins/PluginMeta.cpp
        controllers/plugins/PluginMeta.hpp
        controllers/plugins/PluginPermission.cpp
        controllers/plugins/PluginPermission.hpp
        controllers/plugins/PluginRef.cpp
        controllers/plugins/PluginRef.hpp
        controllers/plugins/SolTypes.cpp
        controllers/plugins/SolTypes.hpp

        controllers/sound/ISoundController.hpp
        controllers/sound/MiniaudioBackend.cpp
        controllers/sound/MiniaudioBackend.hpp
        controllers/sound/NullBackend.cpp
        controllers/sound/NullBackend.hpp

        controllers/spellcheck/SpellChecker.cpp
        controllers/spellcheck/SpellChecker.hpp

        controllers/twitch/LiveController.cpp
        controllers/twitch/LiveController.hpp

        controllers/userdata/UserDataController.cpp
        controllers/userdata/UserDataController.hpp
        controllers/userdata/UserData.hpp

        debug/Benchmark.cpp
        debug/Benchmark.hpp

        messages/AsciiArt.cpp
        messages/AsciiArt.hpp
        messages/Emote.cpp
        messages/Emote.hpp
        messages/Image.cpp
        messages/Image.hpp
        messages/ImageSet.cpp
        messages/ImageSet.hpp
        messages/Link.cpp
        messages/Link.hpp
        messages/Message.cpp
        messages/Message.hpp
        messages/MessageBuilder.cpp
        messages/MessageBuilder.hpp
        messages/MessageColor.cpp
        messages/MessageColor.hpp
        messages/MessageElement.cpp
        messages/MessageElement.hpp
        messages/MessageFlag.hpp
        messages/MessageSimilarity.cpp
        messages/MessageSimilarity.hpp
        messages/MessageSink.hpp
        messages/MessageThread.cpp
        messages/MessageThread.hpp
        messages/StreamingGif.cpp
        messages/StreamingGif.hpp
        messages/StreamingImageDecoder.cpp
        messages/StreamingImageDecoder.hpp

        messages/layouts/MessageLayout.cpp
        messages/layouts/MessageLayout.hpp
        messages/layouts/MessageLayoutContainer.cpp
        messages/layouts/MessageLayoutContainer.hpp
        messages/layouts/MessageLayoutContext.cpp
        messages/layouts/MessageLayoutContext.hpp
        messages/layouts/MessageLayoutElement.cpp
        messages/layouts/MessageLayoutElement.hpp
        messages/search/AuthorPredicate.cpp
        messages/search/AuthorPredicate.hpp
        messages/search/BadgePredicate.cpp
        messages/search/BadgePredicate.hpp
        messages/search/ChannelPredicate.cpp
        messages/search/ChannelPredicate.hpp
        messages/search/LinkPredicate.cpp
        messages/search/LinkPredicate.hpp
        messages/search/MessageFlagsPredicate.cpp
        messages/search/MessageFlagsPredicate.hpp
        messages/search/RegexPredicate.cpp
        messages/search/RegexPredicate.hpp
        messages/search/SubstringPredicate.cpp
        messages/search/SubstringPredicate.hpp
        messages/search/SubtierPredicate.cpp
        messages/search/SubtierPredicate.hpp

        providers/IvrApi.cpp
        providers/IvrApi.hpp
        providers/NetworkConfigurationProvider.cpp
        providers/NetworkConfigurationProvider.hpp

        providers/bluzyrino/BluzyrinoBadges.cpp
        providers/bluzyrino/BluzyrinoBadges.hpp
        providers/bttv/BttvBadges.cpp
        providers/bttv/BttvBadges.hpp
        providers/bttv/BttvEmotes.cpp
        providers/bttv/BttvEmotes.hpp
        providers/bttv/BttvLiveUpdates.cpp
        providers/bttv/BttvLiveUpdates.hpp

        providers/bttv/liveupdates/BttvLiveUpdateClient.cpp
        providers/bttv/liveupdates/BttvLiveUpdateClient.hpp
        providers/bttv/liveupdates/BttvLiveUpdateMessages.cpp
        providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp
        providers/bttv/liveupdates/BttvLiveUpdateSubscription.cpp
        providers/bttv/liveupdates/BttvLiveUpdateSubscription.hpp

        providers/chatterino/ChatterinoBadges.cpp
        providers/chatterino/ChatterinoBadges.hpp

        providers/colors/ColorProvider.cpp
        providers/colors/ColorProvider.hpp

        providers/emoji/Emojis.cpp
        providers/emoji/Emojis.hpp
        providers/emoji/EmojiStyle.hpp

        providers/ffz/FfzBadges.cpp
        providers/ffz/FfzBadges.hpp
        providers/ffz/FfzEmotes.cpp
        providers/ffz/FfzEmotes.hpp
        providers/ffz/FfzUtil.cpp
        providers/ffz/FfzUtil.hpp

        providers/ffzap/FfzApBadges.cpp
        providers/ffzap/FfzApBadges.hpp
        providers/irc/IrcConnection2.cpp
        providers/irc/IrcConnection2.hpp

        providers/kick/KickAccount.cpp
        providers/kick/KickAccount.hpp
        providers/kick/KickAccountManager.cpp
        providers/kick/KickAccountManager.hpp
        providers/kick/KickApi.cpp
        providers/kick/KickApi.hpp
        providers/kick/KickBadges.cpp
        providers/kick/KickBadges.hpp
        providers/kick/KickChannel.cpp
        providers/kick/KickChannel.hpp
        providers/kick/KickChatServer.cpp
        providers/kick/KickChatServer.hpp
        providers/kick/KickEmotes.cpp
        providers/kick/KickEmotes.hpp
        providers/kick/KickLiveController.cpp
        providers/kick/KickLiveController.hpp
        providers/kick/KickLiveUpdates.cpp
        providers/kick/KickLiveUpdates.hpp
        providers/kick/KickMessageBuilder.cpp
        providers/kick/KickMessageBuilder.hpp

        providers/kick/ws/KickCentrifugoManager.cpp
        providers/kick/ws/KickCentrifugoManager.hpp
        providers/kick/ws/KickPusherManager.cpp
        providers/kick/ws/KickPusherManager.hpp
        providers/kick/ws/KickWebSocketCommon.cpp
        providers/kick/ws/KickWebSocketCommon.hpp
        providers/kick/ws/KickWebSocketManager.hpp
        providers/links/LinkInfo.cpp
        providers/links/LinkInfo.hpp
        providers/links/LinkResolver.cpp
        providers/links/LinkResolver.hpp
        providers/links/TikTokLinkResolver.cpp
        providers/links/TikTokLinkResolver.hpp

        providers/moltorino/MoltorinoAuth.cpp
        providers/moltorino/MoltorinoAuth.hpp
        providers/moltorino/MoltorinoAuthPagination.cpp
        providers/moltorino/MoltorinoAuthPagination.hpp
        providers/moltorino/MoltorinoDailyMessage.cpp
        providers/moltorino/MoltorinoDailyMessage.hpp
        providers/moltorino/MoltorinoFeatureFlags.hpp
        providers/moltorino/MoltorinoPresence.cpp
        providers/moltorino/MoltorinoPresence.hpp
        providers/moltorino/MoltorinoSupporterBadges.cpp
        providers/moltorino/MoltorinoSupporterBadges.hpp
        providers/moltorino/MoltorinoUpdater.cpp
        providers/moltorino/MoltorinoUpdater.hpp

        providers/potat/PotatCommands.cpp
        providers/potat/PotatCommands.hpp
        providers/repetitions/RepeatedMessageDetector.cpp
        providers/repetitions/RepeatedMessageDetector.hpp

        providers/homies/HomiesBadges.cpp
        providers/homies/HomiesBadges.hpp


        providers/liveupdates/BasicPubSubClient.hpp
        providers/liveupdates/BasicPubSubListener.hpp
        providers/liveupdates/BasicPubSubManager.hpp

        providers/pronouns/Pronouns.cpp
        providers/pronouns/Pronouns.hpp
        providers/pronouns/UserPronouns.cpp
        providers/pronouns/UserPronouns.hpp
        providers/pronouns/alejo/PronounsAlejoApi.cpp
        providers/pronouns/alejo/PronounsAlejoApi.hpp

        providers/recentmessages/Api.cpp
        providers/recentmessages/Api.hpp
        providers/recentmessages/Impl.cpp
        providers/recentmessages/Impl.hpp

        providers/tiktok/TikTokAccount.cpp
        providers/tiktok/TikTokAccount.hpp
        providers/tiktok/TikTokAccountManager.cpp
        providers/tiktok/TikTokAccountManager.hpp
        providers/tiktok/TikTokApi.cpp
        providers/tiktok/TikTokApi.hpp
        providers/tiktok/TikTokBadge.cpp
        providers/tiktok/TikTokBadge.hpp
        providers/tiktok/TikTokChannel.cpp
        providers/tiktok/TikTokChannel.hpp
        providers/tiktok/TikTokChatServer.cpp
        providers/tiktok/TikTokChatServer.hpp
        providers/tiktok/TikTokEmotes.cpp
        providers/tiktok/TikTokEmotes.hpp
        providers/tiktok/TikTokMessageBuilder.cpp
        providers/tiktok/TikTokMessageBuilder.hpp
        providers/tiktok/TikTokProtocol.cpp
        providers/tiktok/TikTokProtocol.hpp
        providers/tiktok/TikTokRequestSigning.cpp
        providers/tiktok/TikTokRequestSigning.hpp
        providers/tiktok/TikTokSession.cpp
        providers/tiktok/TikTokSession.hpp
        providers/tiktok/TikTokSigning.cpp
        providers/tiktok/TikTokSigning.hpp
        providers/tiktok/TikTokSigningState.cpp
        providers/tiktok/TikTokSigningState.hpp
        providers/tiktok/TikTokText.cpp
        providers/tiktok/TikTokText.hpp
        providers/tiktok/TikTokTicketGuard.cpp
        providers/tiktok/TikTokTicketGuard.hpp
        providers/tiktok/TikTokTypes.cpp
        providers/tiktok/TikTokTypes.hpp
        providers/translation/TranslationCredentials.cpp
        providers/translation/TranslationRequest.cpp
        providers/translation/TranslationRequest.hpp
        providers/translation/Translator.cpp
        providers/translation/Translator.hpp

        providers/seventv/SeventvAPI.cpp
        providers/seventv/SeventvAPI.hpp
        providers/seventv/SeventvBadges.cpp
        providers/seventv/SeventvBadges.hpp
        providers/seventv/SeventvCosmetics.hpp
        providers/seventv/SeventvEmotes.cpp
        providers/seventv/SeventvEmotes.hpp
        providers/seventv/SeventvEventAPI.cpp
        providers/seventv/SeventvEventAPI.hpp
        providers/seventv/SeventvPaints.cpp
        providers/seventv/SeventvPaints.hpp
        providers/seventv/SeventvPersonalEmotes.cpp
        providers/seventv/SeventvPersonalEmotes.hpp

        providers/seventv/paints/Paint.hpp
        providers/seventv/paints/Paint.cpp
        providers/seventv/paints/PaintDropShadow.hpp
        providers/seventv/paints/PaintDropShadow.cpp
        providers/seventv/paints/LinearGradientPaint.hpp
        providers/seventv/paints/LinearGradientPaint.cpp
        providers/seventv/paints/RadialGradientPaint.hpp
        providers/seventv/paints/RadialGradientPaint.cpp
        providers/seventv/paints/UrlPaint.hpp
        providers/seventv/paints/UrlPaint.cpp

        providers/seventv/eventapi/Client.cpp
        providers/seventv/eventapi/Client.hpp
        providers/seventv/eventapi/Dispatch.cpp
        providers/seventv/eventapi/Dispatch.hpp
        providers/seventv/eventapi/Message.cpp
        providers/seventv/eventapi/Message.hpp
        providers/seventv/eventapi/Subscription.cpp
        providers/seventv/eventapi/Subscription.hpp

        providers/twitch/ChannelManagement.cpp
        providers/twitch/ChannelManagement.hpp
        providers/twitch/ChannelPointReward.cpp
        providers/twitch/ChannelPointReward.hpp
        providers/twitch/IrcMessageHandler.cpp
        providers/twitch/IrcMessageHandler.hpp
        providers/twitch/ModerationActionLogs.cpp
        providers/twitch/ModerationActionLogs.hpp
        providers/twitch/PubSubClient.cpp
        providers/twitch/PubSubClient.hpp
        providers/twitch/PubSubManager.cpp
        providers/twitch/PubSubManager.hpp
        providers/twitch/PubSubMessages.hpp
        providers/twitch/TwitchAccount.cpp
        providers/twitch/TwitchAccount.hpp
        providers/twitch/TwitchAccountManager.cpp
        providers/twitch/TwitchAccountManager.hpp
        providers/twitch/TwitchBadge.cpp
        providers/twitch/TwitchBadge.hpp
        providers/twitch/TwitchBadges.cpp
        providers/twitch/TwitchBadges.hpp
        providers/twitch/api/HelixEnums.hpp
        providers/twitch/api/TwitchGifs.cpp
        providers/twitch/api/TwitchGifs.hpp
        providers/twitch/api/TwitchGql.cpp
        providers/twitch/TwitchChannel.cpp
        providers/twitch/TwitchChannel.hpp
        providers/twitch/TwitchCommon.cpp
        providers/twitch/TwitchCommon.hpp
        providers/twitch/TwitchEmotes.cpp
        providers/twitch/TwitchEmotes.hpp
        providers/twitch/TwitchHelpers.cpp
        providers/twitch/TwitchHelpers.hpp
        providers/twitch/TwitchIrc.cpp
        providers/twitch/TwitchIrc.hpp
        providers/twitch/TwitchIrcServer.cpp
        providers/twitch/TwitchIrcServer.hpp
        providers/twitch/TwitchNameHistory.cpp
        providers/twitch/TwitchNameHistory.hpp
        providers/twitch/TwitchNameHistoryCache.hpp
        providers/twitch/TwitchRoles.cpp
        providers/twitch/TwitchRoles.hpp
        providers/twitch/TwitchUser.cpp
        providers/twitch/TwitchUser.hpp
        providers/twitch/TwitchUserLogs.cpp
        providers/twitch/TwitchUserLogs.hpp
        providers/twitch/TwitchUsers.cpp
        providers/twitch/TwitchUsers.hpp
        providers/twitch/UserColor.cpp
        providers/twitch/UserColor.hpp

        providers/twitch/eventsub/Connection.cpp
        providers/twitch/eventsub/Connection.hpp
        providers/twitch/eventsub/Controller.cpp
        providers/twitch/eventsub/Controller.hpp
        providers/twitch/eventsub/MessageBuilder.cpp
        providers/twitch/eventsub/MessageBuilder.hpp
        providers/twitch/eventsub/MessageHandlers.cpp
        providers/twitch/eventsub/MessageHandlers.hpp
        providers/twitch/eventsub/SubscriptionHandle.cpp
        providers/twitch/eventsub/SubscriptionHandle.hpp
        providers/twitch/eventsub/SubscriptionRequest.cpp
        providers/twitch/eventsub/SubscriptionRequest.hpp

        providers/twitch/pubsubmessages/Base.cpp
        providers/twitch/pubsubmessages/Base.hpp
        providers/twitch/pubsubmessages/ChannelPoints.cpp
        providers/twitch/pubsubmessages/ChannelPoints.hpp
        providers/twitch/pubsubmessages/Listen.cpp
        providers/twitch/pubsubmessages/Listen.hpp
        providers/twitch/pubsubmessages/Message.hpp
        providers/twitch/pubsubmessages/PinnedChat.cpp
        providers/twitch/pubsubmessages/PinnedChat.hpp
        providers/twitch/pubsubmessages/Unlisten.cpp
        providers/twitch/pubsubmessages/Unlisten.hpp
        providers/twitch/pubsubmessages/PredictionEvent.cpp
        providers/twitch/pubsubmessages/PredictionEvent.hpp
        providers/twitch/pubsubmessages/PredictionUserEvent.cpp
        providers/twitch/pubsubmessages/PredictionUserEvent.hpp

        providers/twitch/api/Helix.cpp
        providers/twitch/api/Helix.hpp

        providers/youtube/YouTubeAccount.cpp
        providers/youtube/YouTubeAccount.hpp
        providers/youtube/YouTubeAccountManager.cpp
        providers/youtube/YouTubeAccountManager.hpp
        providers/youtube/YouTubeApi.cpp
        providers/youtube/YouTubeApi.hpp
        providers/youtube/YouTubeChannel.cpp
        providers/youtube/YouTubeChannel.hpp
        providers/youtube/YouTubeChatServer.cpp
        providers/youtube/YouTubeChatServer.hpp
        providers/youtube/YouTubeCredentialStore.cpp
        providers/youtube/YouTubeCredentialStore.hpp
        providers/youtube/YouTubeEmotes.cpp
        providers/youtube/YouTubeEmotes.hpp
        providers/youtube/YouTubeInnertubeParser.cpp
        providers/youtube/YouTubeInnertubeParser.hpp
        providers/youtube/YouTubeInnertubeSession.cpp
        providers/youtube/YouTubeInnertubeSession.hpp
        providers/youtube/YouTubeLiveChatSession.cpp
        providers/youtube/YouTubeLiveChatSession.hpp
        providers/youtube/YouTubeMessageBuilder.cpp
        providers/youtube/YouTubeMessageBuilder.hpp
        providers/youtube/YouTubeMessagePacing.hpp
        providers/youtube/YouTubeOAuth.cpp
        providers/youtube/YouTubeOAuth.hpp
        providers/youtube/YouTubeTypes.cpp
        providers/youtube/YouTubeTypes.hpp
        singletons/CrashHandler.cpp
        singletons/CrashHandler.hpp
        singletons/FileLogger.cpp
        singletons/FileLogger.hpp
        singletons/Fonts.cpp
        singletons/Fonts.hpp
        singletons/ImageUploader.cpp
        singletons/ImageUploader.hpp
        singletons/Logging.cpp
        singletons/Logging.hpp
        singletons/NativeMessaging.cpp
        singletons/NativeMessaging.hpp
        singletons/Paths.cpp
        singletons/Paths.hpp
        singletons/Resources.cpp
        singletons/Resources.hpp
        singletons/Settings.cpp
        singletons/Settings.hpp
        singletons/StreamerMode.cpp
        singletons/StreamerMode.hpp
        singletons/Theme.cpp
        singletons/Theme.hpp
        singletons/ThemeCustomization.cpp
        singletons/ThemeCustomization.hpp
        singletons/ThemeVideo.cpp
        singletons/ThemeVideo.hpp
        singletons/ThemeVideoDecoder.cpp
        singletons/ThemeVideoDecoder.hpp
        singletons/ThemeVideoImport.cpp
        singletons/ThemeVideoImport.hpp
        singletons/ThemeWallpaper.cpp
        singletons/ThemeWallpaper.hpp
        singletons/Toasts.cpp
        singletons/Toasts.hpp
        singletons/Updates.cpp
        singletons/Updates.hpp
        singletons/WindowManager.cpp
        singletons/WindowManager.hpp

        singletons/helper/GifTimer.cpp
        singletons/helper/GifTimer.hpp
        singletons/helper/LoggingChannel.cpp
        singletons/helper/LoggingChannel.hpp

        util/AbandonObject.hpp
        util/AttachToConsole.cpp
        util/AttachToConsole.hpp
        util/Backup.cpp
        util/Backup.hpp
        util/BadgeRegistry.cpp
        util/BadgeRegistry.hpp
        util/BoostJsonWrap.cpp
        util/BoostJsonWrap.hpp
        util/CancellationToken.hpp
        util/ChannelHelpers.hpp
        util/ChannelPointAmount.cpp
        util/ChannelPointAmount.hpp
        util/Clipboard.cpp
        util/Clipboard.hpp
        util/CustomPlayer.cpp
        util/CustomPlayer.hpp
        util/DebugCount.cpp
        util/DebugCount.hpp
        util/DisplayBadge.cpp
        util/DisplayBadge.hpp
        util/Expected.hpp
        util/FilesystemHelpers.hpp
        util/FormatTime.cpp
        util/FormatTime.hpp
        util/FunctionEventFilter.cpp
        util/FunctionEventFilter.hpp
        util/FuzzyConvert.cpp
        util/FuzzyConvert.hpp
        util/Helpers.cpp
        util/Helpers.hpp
        util/HttpServer.cpp
        util/HttpServer.hpp
        util/ImageUploader.cpp
        util/ImageUploader.hpp
        util/IncognitoBrowser.cpp
        util/IncognitoBrowser.hpp
        util/IpcQueue.cpp
        util/IpcQueue.hpp
        util/IrcHelpers.cpp
        util/IrcHelpers.hpp
        util/LayoutHelper.cpp
        util/LayoutHelper.hpp
        util/LoadPixmap.cpp
        util/LoadPixmap.hpp
        util/MemoryReclaimer.cpp
        util/MemoryReclaimer.hpp
        util/MultiChannel.cpp
        util/MultiChannel.hpp
        util/OnceFlag.cpp
        util/OnceFlag.hpp
        util/RapidJsonSerializeQStringView.hpp
        util/RapidjsonHelpers.cpp
        util/RapidjsonHelpers.hpp
        util/RatelimitBucket.cpp
        util/RatelimitBucket.hpp
        util/RenameThread.cpp
        util/RenameThread.hpp
        util/SampleData.cpp
        util/SampleData.hpp
        util/SelfCheck.cpp
        util/SelfCheck.hpp
        util/SettingsTransfer.cpp
        util/SettingsTransfer.hpp
        util/SharedPtrElementLess.hpp
        util/SignalListener.hpp
        util/StreamLink.cpp
        util/StreamLink.hpp
        util/ThreadGuard.hpp
        util/Twitch.cpp
        util/Twitch.hpp
        util/TypeName.hpp
        util/UnixSignalHandler.cpp
        util/UnixSignalHandler.hpp
        util/Variant.hpp
        util/VectorMessageSink.cpp
        util/VectorMessageSink.hpp
        util/WidgetHelpers.cpp
        util/WidgetHelpers.hpp
        util/WindowsHelper.cpp
        util/WindowsHelper.hpp
        util/XDGDesktopFile.cpp
        util/XDGDesktopFile.hpp
        util/XDGDirectory.cpp
        util/XDGDirectory.hpp
        util/XDGHelper.cpp
        util/XDGHelper.hpp

        util/serialize/Container.hpp
        util/serialize/List.hpp

        widgets/AccountSwitchPopup.cpp
        widgets/AccountSwitchPopup.hpp
        widgets/AccountSwitchWidget.cpp
        widgets/AccountSwitchWidget.hpp
        widgets/AttachedWindow.cpp
        widgets/AttachedWindow.hpp
        widgets/BasePopup.cpp
        widgets/BasePopup.hpp
        widgets/BaseWidget.cpp
        widgets/BaseWidget.hpp
        widgets/BaseWindow.cpp
        widgets/BaseWindow.hpp
        widgets/ChatterListWidget.cpp
        widgets/ChatterListWidget.hpp
        widgets/DraggablePopup.cpp
        widgets/DraggablePopup.hpp
        widgets/FramelessEmbedWindow.cpp
        widgets/FramelessEmbedWindow.hpp
        widgets/Label.cpp
        widgets/Label.hpp
        widgets/MarkdownLabel.cpp
        widgets/MarkdownLabel.hpp
        widgets/Notebook.cpp
        widgets/Notebook.hpp
        widgets/OverlayWindow.cpp
        widgets/OverlayWindow.hpp
        widgets/PluginRepl.cpp
        widgets/PluginRepl.hpp
        widgets/Scrollbar.cpp
        widgets/Scrollbar.hpp
        widgets/TooltipEntryWidget.cpp
        widgets/TooltipEntryWidget.hpp
        widgets/TooltipWidget.cpp
        widgets/TooltipWidget.hpp
        widgets/Window.cpp
        widgets/Window.hpp

        widgets/buttons/Button.cpp
        widgets/buttons/Button.hpp
        widgets/buttons/DimButton.cpp
        widgets/buttons/DimButton.hpp
        widgets/buttons/DrawnButton.cpp
        widgets/buttons/DrawnButton.hpp
        widgets/buttons/InitMoltorinoUpdateButton.cpp
        widgets/buttons/InitMoltorinoUpdateButton.hpp
        widgets/buttons/InitUpdateButton.cpp
        widgets/buttons/InitUpdateButton.hpp
        widgets/buttons/LabelButton.cpp
        widgets/buttons/LabelButton.hpp
        widgets/buttons/PixmapButton.cpp
        widgets/buttons/PixmapButton.hpp
        widgets/buttons/SignalLabel.cpp
        widgets/buttons/SignalLabel.hpp
        widgets/buttons/SvgButton.cpp
        widgets/buttons/SvgButton.hpp
        widgets/buttons/TitlebarButton.cpp
        widgets/buttons/TitlebarButton.hpp
        widgets/buttons/TitlebarButtons.cpp
        widgets/buttons/TitlebarButtons.hpp

        widgets/dialogs/BadgePickerDialog.cpp
        widgets/dialogs/BadgePickerDialog.hpp
        widgets/dialogs/ChannelFilterEditorDialog.cpp
        widgets/dialogs/ChannelFilterEditorDialog.hpp
        widgets/dialogs/ChannelManagementDialog.cpp
        widgets/dialogs/ChannelManagementDialog.hpp
        widgets/dialogs/ChannelPointsDialog.cpp
        widgets/dialogs/ChannelPointsDialog.hpp
        widgets/dialogs/ChatAutomationDialog.cpp
        widgets/dialogs/ChatAutomationDialog.hpp
        widgets/dialogs/ColorPickerDialog.cpp
        widgets/dialogs/ColorPickerDialog.hpp
        widgets/dialogs/CrossBanDialog.cpp
        widgets/dialogs/CrossBanDialog.hpp
        widgets/dialogs/DialogLinkText.cpp
        widgets/dialogs/DialogLinkText.hpp
        widgets/dialogs/EditHotkeyDialog.cpp
        widgets/dialogs/EditHotkeyDialog.hpp
        widgets/dialogs/EditUserNotesDialog.cpp
        widgets/dialogs/EditUserNotesDialog.hpp
        widgets/dialogs/EmotePopup.cpp
        widgets/dialogs/EmotePopup.hpp

        widgets/dialogs/font/FontDialog.cpp
        widgets/dialogs/font/FontDialog.hpp
        widgets/dialogs/font/FontFamilyWidget.cpp
        widgets/dialogs/font/FontFamilyWidget.hpp
        widgets/dialogs/font/FontSettingDialog.cpp
        widgets/dialogs/font/FontSettingDialog.hpp
        widgets/dialogs/font/FontSizeWidget.cpp
        widgets/dialogs/font/FontSizeWidget.hpp
        widgets/dialogs/font/FontWeightWidget.cpp
        widgets/dialogs/font/FontWeightWidget.hpp
        widgets/dialogs/font/IntItem.cpp
        widgets/dialogs/font/IntItem.hpp
        widgets/dialogs/font/PreviewWidget.cpp
        widgets/dialogs/font/PreviewWidget.hpp

        widgets/dialogs/GifPickerDialog.cpp
        widgets/dialogs/GifPickerDialog.hpp
        widgets/dialogs/KickLoginPage.cpp
        widgets/dialogs/KickLoginPage.hpp
        widgets/dialogs/LastRunCrashDialog.cpp
        widgets/dialogs/LastRunCrashDialog.hpp
        widgets/dialogs/LoginDialog.cpp
        widgets/dialogs/LoginDialog.hpp
        widgets/dialogs/ModerationReportDialog.cpp
        widgets/dialogs/ModerationReportDialog.hpp
        widgets/dialogs/ModeratorCommentsView.cpp
        widgets/dialogs/ModeratorCommentsView.hpp
        widgets/dialogs/MoltorinoDialogTheme.hpp
        widgets/dialogs/PopupControlMetrics.hpp
        widgets/dialogs/QualityPopup.cpp
        widgets/dialogs/QualityPopup.hpp
        widgets/dialogs/PredictionDialog.cpp
        widgets/dialogs/PredictionDialog.hpp
        widgets/dialogs/PollDialog.cpp
        widgets/dialogs/PollDialog.hpp
        widgets/dialogs/ReplyThreadPopup.cpp
        widgets/dialogs/ReplyThreadPopup.hpp
        widgets/dialogs/RestoreBackupsDialog.cpp
        widgets/dialogs/RestoreBackupsDialog.hpp
        widgets/dialogs/RewardRequestQueueDialog.cpp
        widgets/dialogs/RewardRequestQueueDialog.hpp
        widgets/dialogs/SelectChannelDialog.cpp
        widgets/dialogs/SelectChannelDialog.hpp
        widgets/dialogs/SelectChannelFiltersDialog.cpp
        widgets/dialogs/SelectChannelFiltersDialog.hpp
        widgets/dialogs/SettingsDialog.cpp
        widgets/dialogs/SettingsDialog.hpp
        widgets/dialogs/TabGroupDialog.cpp
        widgets/dialogs/TabGroupDialog.hpp
        widgets/dialogs/TikTokLoginPage.cpp
        widgets/dialogs/TikTokLoginPage.hpp
        widgets/dialogs/TranslationProviderDialog.cpp
        widgets/dialogs/TranslationProviderDialog.hpp
        widgets/dialogs/UnbanRequestsDialog.cpp
        widgets/dialogs/UnbanRequestsDialog.hpp
        widgets/dialogs/UpdateDialog.cpp
        widgets/dialogs/UpdateDialog.hpp
        widgets/dialogs/UserInfoPopup.cpp
        widgets/dialogs/UserInfoPopup.hpp
        widgets/dialogs/UserLogsView.cpp
        widgets/dialogs/UserLogsView.hpp
        widgets/dialogs/UserRolesView.cpp
        widgets/dialogs/UserRolesView.hpp
        widgets/dialogs/VanityDialog.cpp
        widgets/dialogs/VanityDialog.hpp
        widgets/dialogs/WelcomeDialog.cpp
        widgets/dialogs/WelcomeDialog.hpp
        widgets/dialogs/YouTubeLoginPage.cpp
        widgets/dialogs/YouTubeLoginPage.hpp
        widgets/dialogs/switcher/NewPopupItem.cpp
        widgets/dialogs/switcher/NewPopupItem.hpp
        widgets/dialogs/switcher/NewTabItem.cpp
        widgets/dialogs/switcher/NewTabItem.hpp
        widgets/dialogs/switcher/QuickSwitcherModel.cpp
        widgets/dialogs/switcher/QuickSwitcherModel.hpp
        widgets/dialogs/switcher/QuickSwitcherPopup.cpp
        widgets/dialogs/switcher/QuickSwitcherPopup.hpp
        widgets/dialogs/switcher/SwitchSplitItem.cpp
        widgets/dialogs/switcher/SwitchSplitItem.hpp

        widgets/helper/color/AlphaSlider.cpp
        widgets/helper/color/AlphaSlider.hpp
        widgets/helper/color/Checkerboard.cpp
        widgets/helper/color/Checkerboard.hpp
        widgets/helper/color/ColorButton.cpp
        widgets/helper/color/ColorButton.hpp
        widgets/helper/color/ColorInput.cpp
        widgets/helper/color/ColorInput.hpp
        widgets/helper/color/ColorItemDelegate.cpp
        widgets/helper/color/ColorItemDelegate.hpp
        widgets/helper/color/HueSlider.cpp
        widgets/helper/color/HueSlider.hpp
        widgets/helper/color/SBCanvas.cpp
        widgets/helper/color/SBCanvas.hpp

        widgets/helper/AutoModReviewBar.cpp
        widgets/helper/AutoModReviewBar.hpp
        widgets/helper/ChannelCategoryPicker.cpp
        widgets/helper/ChannelCategoryPicker.hpp
        widgets/helper/ChannelView.cpp
        widgets/helper/ChannelView.hpp
        widgets/helper/CmdDeleteKeyFilter.cpp
        widgets/helper/CmdDeleteKeyFilter.hpp
        widgets/helper/ComboBoxItemDelegate.cpp
        widgets/helper/ComboBoxItemDelegate.hpp
        widgets/helper/DebugPopup.cpp
        widgets/helper/DebugPopup.hpp
        widgets/helper/EditableModelView.cpp
        widgets/helper/EditableModelView.hpp
        widgets/helper/FontSettingWidget.cpp
        widgets/helper/FontSettingWidget.hpp
        widgets/helper/IconDelegate.cpp
        widgets/helper/IconDelegate.hpp
        widgets/helper/InvisibleSizeGrip.cpp
        widgets/helper/InvisibleSizeGrip.hpp
        widgets/helper/KickAccountSwitchWidget.cpp
        widgets/helper/KickAccountSwitchWidget.hpp
        widgets/helper/MessageView.cpp
        widgets/helper/MessageView.hpp
        widgets/helper/MicroNotebook.cpp
        widgets/helper/MicroNotebook.hpp
        widgets/helper/NotebookTab.cpp
        widgets/helper/NotebookTab.hpp
        widgets/helper/OverlayInteraction.cpp
        widgets/helper/OverlayInteraction.hpp
        widgets/helper/PinnedMessageBanner.cpp
        widgets/helper/PinnedMessageBanner.hpp
        widgets/helper/PollBanner.cpp
        widgets/helper/PollBanner.hpp
        widgets/helper/PredictionBanner.cpp
        widgets/helper/PredictionBanner.hpp
        widgets/helper/RegExpItemDelegate.cpp
        widgets/helper/RegExpItemDelegate.hpp
        widgets/helper/ResizingTextEdit.cpp
        widgets/helper/ResizingTextEdit.hpp
        widgets/helper/ScalingSpacerItem.cpp
        widgets/helper/ScalingSpacerItem.hpp
        widgets/helper/ScrollbarHighlight.cpp
        widgets/helper/ScrollbarHighlight.hpp
        widgets/helper/SearchPopup.cpp
        widgets/helper/SearchPopup.hpp
        widgets/helper/SettingsDialogTab.cpp
        widgets/helper/SettingsDialogTab.hpp
        widgets/helper/LiveIndicator.cpp
        widgets/helper/LiveIndicator.hpp
        widgets/helper/SettingsTheme.hpp
        widgets/helper/TableStyles.cpp
        widgets/helper/TableStyles.hpp
        widgets/helper/TikTokAccountSwitchWidget.cpp
        widgets/helper/TikTokAccountSwitchWidget.hpp
        widgets/helper/TrimRegExpValidator.cpp
        widgets/helper/TrimRegExpValidator.hpp
        widgets/helper/YouTubeAccountSwitchWidget.cpp
        widgets/helper/YouTubeAccountSwitchWidget.hpp

        widgets/layout/FlowLayout.cpp
        widgets/layout/FlowLayout.hpp

        widgets/listview/GenericItemDelegate.cpp
        widgets/listview/GenericItemDelegate.hpp
        widgets/listview/GenericListItem.cpp
        widgets/listview/GenericListItem.hpp
        widgets/listview/GenericListModel.cpp
        widgets/listview/GenericListModel.hpp
        widgets/listview/GenericListView.cpp
        widgets/listview/GenericListView.hpp

        widgets/settingspages/AboutPage.cpp
        widgets/settingspages/AboutPage.hpp
        widgets/settingspages/AccountsPage.cpp
        widgets/settingspages/AccountsPage.hpp
        widgets/settingspages/BackupSharingPage.cpp
        widgets/settingspages/BackupSharingPage.hpp
        widgets/settingspages/CommandPage.cpp
        widgets/settingspages/CommandPage.hpp
        widgets/settingspages/CustomWidgets.hpp
        widgets/settingspages/CustomizationPage.cpp
        widgets/settingspages/CustomizationPage.hpp
        widgets/settingspages/ExternalToolsPage.cpp
        widgets/settingspages/ExternalToolsPage.hpp
        widgets/settingspages/FiltersPage.cpp
        widgets/settingspages/FiltersPage.hpp
        widgets/settingspages/GeneralPage.cpp
        widgets/settingspages/GeneralPage.hpp
        widgets/settingspages/GeneralPageView.cpp
        widgets/settingspages/GeneralPageView.hpp
        widgets/settingspages/HighlightingPage.cpp
        widgets/settingspages/HighlightingPage.hpp
        widgets/settingspages/IgnoresPage.cpp
        widgets/settingspages/IgnoresPage.hpp
        widgets/settingspages/KeyboardSettingsPage.cpp
        widgets/settingspages/KeyboardSettingsPage.hpp
        widgets/settingspages/ModerationPage.cpp
        widgets/settingspages/ModerationPage.hpp
        widgets/settingspages/MoltorinoPage.cpp
        widgets/settingspages/MoltorinoPage.hpp
        widgets/settingspages/NicknamesPage.cpp
        widgets/settingspages/NicknamesPage.hpp
        widgets/settingspages/NotificationPage.cpp
        widgets/settingspages/NotificationPage.hpp
        widgets/settingspages/PluginsPage.cpp
        widgets/settingspages/PluginsPage.hpp
        widgets/settingspages/SettingsPage.cpp
        widgets/settingspages/SettingsPage.hpp
        widgets/settingspages/SettingWidget.cpp
        widgets/settingspages/SettingWidget.hpp
        widgets/settingspages/UsercardModerationSettings.cpp
        widgets/settingspages/UsercardModerationSettings.hpp

        widgets/splits/ClosedSplits.cpp
        widgets/splits/ClosedSplits.hpp
        widgets/splits/DraggedSplit.cpp
        widgets/splits/DraggedSplit.hpp
        widgets/splits/InputCompletionItem.cpp
        widgets/splits/InputCompletionItem.hpp
        widgets/splits/InputCompletionPopup.cpp
        widgets/splits/InputCompletionPopup.hpp
        widgets/splits/InputHighlighter.cpp
        widgets/splits/InputHighlighter.hpp
        widgets/splits/Split.cpp
        widgets/splits/Split.hpp
        widgets/splits/SplitCommon.hpp
        widgets/splits/SplitContainer.cpp
        widgets/splits/SplitContainer.hpp
        widgets/splits/SplitHeader.cpp
        widgets/splits/SplitHeader.hpp
        widgets/splits/SplitInput.cpp
        widgets/splits/SplitInput.hpp
        widgets/splits/SplitOverlay.cpp
        widgets/splits/SplitOverlay.hpp
        )

if (APPLE)
    set(MACOS_BUNDLE_ICON_FILE "${CMAKE_SOURCE_DIR}/resources/moltorino.icns")
    list(APPEND SOURCE_FILES "${MACOS_BUNDLE_ICON_FILE}")
    set_source_files_properties(${MACOS_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    list(APPEND SOURCE_FILES util/MacOsHelpers.mm)
endif ()

if(MOLTORINO_YOUTUBE_ACCOUNTS OR MOLTORINO_TIKTOK_ACCOUNTS)
    list(APPEND SOURCE_FILES common/SecureCredentials.cpp)
else()
    list(APPEND SOURCE_FILES common/SecureCredentialsUnavailable.cpp)
endif()

# Generate source groups for use in IDEs
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${SOURCE_FILES})

# Add autogenerated files
list(APPEND SOURCE_FILES ${RES_AUTOGEN_FILES})

set_source_files_properties(providers/moltorino/MoltorinoUpdater.cpp PROPERTIES
    COMPILE_DEFINITIONS "MOLTORINO_DEFAULT_UPDATE_CHANNEL=\"${MOLTORINO_UPDATE_CHANNEL}\""
)

add_library(${LIBRARY_PROJECT} OBJECT ${SOURCE_FILES})
add_library(moltorino-tiktok-login STATIC
    ../lib/quirc/quirc.c
    ../lib/quirc/decode.c
    ../lib/quirc/identify.c
    ../lib/quirc/version_db.c
    ../lib/qrcodegen/qrcodegen.cpp
    widgets/dialogs/TikTokLoginQrCode.cpp
    widgets/dialogs/TikTokLoginQrCode.hpp
    widgets/dialogs/tiktok_login.qrc
    widgets/dialogs/TikTokLoginView.cpp
    widgets/dialogs/TikTokLoginView.hpp
    widgets/dialogs/TikTokLoginBrowser.cpp
    widgets/dialogs/TikTokLoginBrowser.hpp
)
target_include_directories(moltorino-tiktok-login PRIVATE
    .
    ../lib/expected-lite/include
    ../lib/quirc
    ../lib/qrcodegen
)
set_target_properties(moltorino-tiktok-login PROPERTIES AUTORCC ON)
install(FILES ../lib/quirc/LICENSE DESTINATION licenses RENAME quirc.txt)
install(FILES ../lib/qrcodegen/LICENSE DESTINATION licenses RENAME qrcodegen.txt)
target_link_libraries(moltorino-tiktok-login PRIVATE
    Pajlada::Signals
    Qt${MAJOR_QT_VERSION}::Core
    Qt${MAJOR_QT_VERSION}::Widgets
    Qt${MAJOR_QT_VERSION}::Network
    Qt${MAJOR_QT_VERSION}::Svg
)
if(TARGET MoltorinoWebView2)
    target_sources(moltorino-tiktok-login PRIVATE
        widgets/dialogs/TikTokLoginBrowserWindows.cpp)
    target_compile_definitions(moltorino-tiktok-login PRIVATE
        MOLTORINO_TIKTOK_WEBVIEW2=1)
    target_link_libraries(moltorino-tiktok-login PRIVATE MoltorinoWebView2)
    install(FILES "${MOLTORINO_WEBVIEW2_LICENSE}"
        DESTINATION licenses RENAME WebView2.txt)
elseif(APPLE AND MOLTORINO_TIKTOK_ACCOUNTS)
    target_sources(moltorino-tiktok-login PRIVATE
        widgets/dialogs/TikTokLoginBrowserMac.mm)
    set_source_files_properties(widgets/dialogs/TikTokLoginBrowserMac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(moltorino-tiktok-login PROPERTIES
        OBJCXX_STANDARD ${CMAKE_CXX_STANDARD}
        OBJCXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(moltorino-tiktok-login PRIVATE
        MOLTORINO_TIKTOK_WEBKIT=1)
    target_link_libraries(moltorino-tiktok-login PRIVATE
        "-framework Cocoa"
        "-framework WebKit"
    )
elseif(UNIX AND MOLTORINO_TIKTOK_ACCOUNTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TIKTOK_WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1>=2.42)
    pkg_check_modules(TIKTOK_JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
    add_executable(moltorino-tiktok-browser
        widgets/dialogs/TikTokLoginBrowserLinux.cpp
        widgets/dialogs/TikTokLoginBrowserLinuxMain.cpp
        widgets/dialogs/TikTokLoginBrowserLinux.hpp)
    target_link_libraries(moltorino-tiktok-browser PRIVATE
        PkgConfig::TIKTOK_WEBKIT
        PkgConfig::TIKTOK_JSON_GLIB
    )
    set_target_properties(moltorino-tiktok-browser PROPERTIES
        AUTOMOC OFF
        CXX_VISIBILITY_PRESET hidden
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_definitions(moltorino-tiktok-login PRIVATE
        MOLTORINO_TIKTOK_WEBKITGTK=1)
    add_dependencies(moltorino-tiktok-login moltorino-tiktok-browser)
    install(TARGETS moltorino-tiktok-browser RUNTIME DESTINATION libexec/moltorino)
endif()

target_link_libraries(${LIBRARY_PROJECT} PRIVATE moltorino-tiktok-login)

if(TARGET Velopack::Velopack)
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC Velopack::Velopack)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC MOLTORINO_VELOPACK_ENABLED=1)
    add_dependencies(${LIBRARY_PROJECT} moltorino-velopack-runtime)
endif()

if(CHATTERINO_PLUGINS)
    target_compile_definitions(${LIBRARY_PROJECT}
        PUBLIC
        CHATTERINO_HAVE_PLUGINS
    )
    message(STATUS "Building Chatterino with lua plugin support enabled.")
endif()

if (CHATTERINO_GENERATE_COVERAGE)
    include(CodeCoverage)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "(GNU|Clang)")
        append_coverage_compiler_flags_to_target(${LIBRARY_PROJECT})
    endif()
    message(STATUS "project source dir: ${PROJECT_SOURCE_DIR}/src")
    setup_target_for_coverage_gcovr_html(
        NAME coverage
        EXECUTABLE ctest
        EXCLUDE "/usr/include/*"
        EXCLUDE "build*/*"
        # EXCLUDE "lib/*"
        EXCLUDE "lib/certify/*"
        EXCLUDE "lib/expected-lite/*"
        EXCLUDE "lib/googletest/*"
        EXCLUDE "lib/libcommuni/*"
        EXCLUDE "lib/lrucache/*"
        EXCLUDE "lib/lua/*"
        EXCLUDE "lib/magic_enum/*"
        EXCLUDE "lib/miniaudio/*"
        EXCLUDE "lib/qtkeychain/*"
        EXCLUDE "lib/rapidjson/*"
        EXCLUDE "lib/semver/*"
        EXCLUDE "lib/serialize/*"
        EXCLUDE "lib/settings/*"
        EXCLUDE "lib/signals/*"
        EXCLUDE "lib/sol2/*"
        EXCLUDE "lib/WinToast/*"

        EXCLUDE "*/ui_*.h"
        EXCLUDE "*/moc_*.cpp"
        )
endif ()

if(TARGET rapidjson)
    message(STATUS "rapidjson: Linking to rapidjson target")
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC rapidjson)
elseif(DEFINED RAPIDJSON_INCLUDE_DIRS)
    message(STATUS "rapidjson: RAPIDJSON_INCLUDE_DIRS defined (${RAPIDJSON_INCLUDE_DIRS}). Most likely a system install.")
    target_include_directories(${LIBRARY_PROJECT} SYSTEM PUBLIC ${RAPIDJSON_INCLUDE_DIRS})
elseif(DEFINED RapidJSON_SOURCE_DIR)
    message(STATUS "rapidjson: RapidJSON_SOURCE_DIR defined, assuming this is a submodule/source build. (${RapidJSON_SOURCE_DIR})")
    target_include_directories(${LIBRARY_PROJECT} SYSTEM PUBLIC ${RapidJSON_SOURCE_DIR}/include)
endif()

target_link_libraries(${LIBRARY_PROJECT}
        PUBLIC
        Qt${MAJOR_QT_VERSION}::Core
        Qt${MAJOR_QT_VERSION}::Widgets
        Qt${MAJOR_QT_VERSION}::WidgetsPrivate # for pixmap filters (paints)
        Qt${MAJOR_QT_VERSION}::Gui
        Qt${MAJOR_QT_VERSION}::Network
        Qt${MAJOR_QT_VERSION}::NetworkPrivate # for DELETE with a body (Kick)
        Qt${MAJOR_QT_VERSION}::Svg
        Qt${MAJOR_QT_VERSION}::SvgWidgets
        Qt${MAJOR_QT_VERSION}::Sql
        Qt${MAJOR_QT_VERSION}::Concurrent

        LibCommuni::LibCommuni
        Pajlada::Serialize
        Pajlada::Settings
        Pajlada::Signals
        Threads::Threads
        LRUCache
        MagicEnum
        $<$<BOOL:${WIN32}>:Wtsapi32>
        twitch-eventsub-ws
        BoostCertify
        )
target_link_libraries(${LIBRARY_PROJECT} PRIVATE ${CHATTERINO_WEBP_TARGETS})

if(MOLTORINO_YOUTUBE_ACCOUNTS OR MOLTORINO_TIKTOK_ACCOUNTS)
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC keychain::keychain)
endif()

target_compile_definitions(${LIBRARY_PROJECT} PUBLIC
    MOLTORINO_HAVE_YOUTUBE_ACCOUNTS=$<BOOL:${MOLTORINO_YOUTUBE_ACCOUNTS}>)
if(MOLTORINO_TIKTOK_ACCOUNTS)
    if(OPENSSL_VERSION VERSION_LESS 3.0)
        message(FATAL_ERROR "TikTok accounts require OpenSSL 3.0 or newer")
    endif()
    set_source_files_properties(providers/tiktok/TikTokRequestSigning.cpp
        PROPERTIES COMPILE_DEFINITIONS MOLTORINO_HAVE_TIKTOK_ACCOUNTS=1)
endif()
install(FILES providers/tiktok/TikTokSigning.LICENSE.txt
    DESTINATION licenses RENAME TikTokSigning.txt)

if (CHATTERINO_PLUGINS)
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC lua sol2::sol2)
endif()

if (CHATTERINO_SPELLCHECK)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC CHATTERINO_WITH_SPELLCHECK)
    target_link_libraries(${LIBRARY_PROJECT} PRIVATE hunspell::hunspell)
endif()

if (CHATTERINO_ALLOW_PRIVATE_QT_API)
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC Qt${MAJOR_QT_VERSION}::GuiPrivate)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC
        CHATTERINO_WITH_PRIVATE_QT_API)
endif()

# Set the output of TARGET to be
# - CMAKE_BIN_DIR/lib for libraries
# - CMAKE_BIN_DIR/bin for BINARIES
# an additional argument specifies the subdirectory.
function(set_target_directory_hierarchy TARGET)
    set_target_properties(${TARGET}
        PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${ARGV1}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${ARGV1}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${ARGV1}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/${ARGV1}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/${ARGV1}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/${ARGV1}"
    )
endfunction()

if (BUILD_APP)
    if (APPLE)
        add_executable(${EXECUTABLE_PROJECT} ${MACOS_BUNDLE_ICON_FILE} main.cpp)
    else()
        add_executable(${EXECUTABLE_PROJECT} main.cpp)
    endif()

    if (CHATTERINO_WITH_AVIF_PLUGIN)
        target_link_libraries(${EXECUTABLE_PROJECT} PRIVATE kimageformats)
        target_compile_definitions(${EXECUTABLE_PROJECT} PRIVATE CHATTERINO_WITH_AVIF_PLUGIN)
    endif()

    if(CHATTERINO_SANITIZER_SUPPORT)
        add_sanitizers(${EXECUTABLE_PROJECT})
    endif()

    if (CHATTERINO_STATIC_QT_BUILD)
        qt_import_plugins(${EXECUTABLE_PROJECT} INCLUDE_BY_TYPE
            platforms Qt::QXcbIntegrationPlugin
            Qt::QMinimalIntegrationPlugin
        )
    endif ()

    target_include_directories(${EXECUTABLE_PROJECT} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/autogen/)

    target_link_libraries(${EXECUTABLE_PROJECT} PUBLIC ${LIBRARY_PROJECT})

    set_target_directory_hierarchy(${EXECUTABLE_PROJECT})
    set_target_properties(${EXECUTABLE_PROJECT} PROPERTIES OUTPUT_NAME "Moltorino7")

    set(MOLTORINO_MEDIA_TOOLS_DIR "${CMAKE_BINARY_DIR}/bin/support/media" CACHE PATH
        "Directory containing the bundled ffmpeg, ffprobe and licenses")
    if(NOT EXISTS "${MOLTORINO_MEDIA_TOOLS_DIR}/ffmpeg${CMAKE_EXECUTABLE_SUFFIX}")
        find_program(MOLTORINO_FFMPEG ffmpeg PATH_SUFFIXES support/media)
        if(MOLTORINO_FFMPEG)
            get_filename_component(MOLTORINO_MEDIA_TOOLS_DIR "${MOLTORINO_FFMPEG}" DIRECTORY)
        endif()
    endif()
    foreach(program ffmpeg ffprobe)
        if(NOT EXISTS "${MOLTORINO_MEDIA_TOOLS_DIR}/${program}${CMAKE_EXECUTABLE_SUFFIX}")
            message(FATAL_ERROR "Missing background converter ${program}. Run Conan install or set MOLTORINO_MEDIA_TOOLS_DIR to the converter bundle.")
        endif()
    endforeach()
    if(NOT EXISTS "${MOLTORINO_MEDIA_TOOLS_DIR}/licenses")
        message(FATAL_ERROR "The background converter license directory is missing.")
    endif()
    add_custom_command(TARGET ${EXECUTABLE_PROJECT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOLTORINO_MEDIA_TOOLS_DIR}"
            "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/support/media" VERBATIM)
    if(WIN32)
        set(media_install_dir support/media)
        add_custom_command(TARGET ${EXECUTABLE_PROJECT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/licenses"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/lib/quirc/LICENSE"
                "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/licenses/quirc.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/lib/qrcodegen/LICENSE"
                "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/licenses/qrcodegen.txt" VERBATIM)
        if(TARGET MoltorinoWebView2)
            add_custom_command(TARGET ${EXECUTABLE_PROJECT} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOLTORINO_WEBVIEW2_LICENSE}"
                    "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/licenses/WebView2.txt" VERBATIM)
        endif()
    elseif(APPLE)
        set(media_install_dir bin/Moltorino7.app/Contents/MacOS/support/media)
    else()
        set(media_install_dir bin/support/media)
    endif()
    install(DIRECTORY "${MOLTORINO_MEDIA_TOOLS_DIR}/" DESTINATION "${media_install_dir}"
        USE_SOURCE_PERMISSIONS)

    if (WIN32)
        if (WINDEPLOYQT_PATH)
            file(TO_CMAKE_PATH "${WINDEPLOYQT_PATH}" WINDEPLOYQT_PATH)
        else()
            if (VCPKG_INSTALLED_DIR AND (CMAKE_BUILD_TYPE STREQUAL "Debug"))
                find_program(WINDEPLOYQT_PATH NAMES windeployqt.debug.bat)
            else()
                find_program(WINDEPLOYQT_PATH NAMES windeployqt)
            endif()
        endif()

        if (NOT EXISTS ${WINDEPLOYQT_PATH})
            message(FATAL_ERROR "windeployqt.exe not found")
        endif()

        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(WINDEPLOYQT_MODE --debug)
            get_target_property(QT_CORE_LOC Qt${MAJOR_QT_VERSION}::Core LOCATION_DEBUG)
        else()
            set(WINDEPLOYQT_MODE --release)
            get_target_property(QT_CORE_LOC Qt${MAJOR_QT_VERSION}::Core LOCATION)
        endif()
        get_filename_component(QT_BIN_DIR ${QT_CORE_LOC} DIRECTORY)

        add_custom_command(TARGET ${EXECUTABLE_PROJECT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt${MAJOR_QT_VERSION}::Sql>"
                "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/sqldrivers"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt${MAJOR_QT_VERSION}::QSQLiteDriverPlugin>"
                "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/sqldrivers"
            VERBATIM)

        # This assumes the installed CRT is up-to-date (see .CI/deploy-crt.ps1)
        set(WINDEPLOYQT_COMMAND_ARGV
            "${WINDEPLOYQT_PATH}"
            "$<TARGET_FILE:${EXECUTABLE_PROJECT}>"
            ${WINDEPLOYQT_MODE}
            --no-compiler-runtime
            --no-translations
            --no-opengl-sw
        )
        if (Qt6_VERSION VERSION_GREATER_EQUAL "6.8.0")
            # windeployqt expects to find openssl in '/bin' but we deploy it in '/'.
            list(APPEND WINDEPLOYQT_COMMAND_ARGV --force-openssl)
        endif()
        string(REPLACE ";" " " WINDEPLOYQT_COMMAND "${WINDEPLOYQT_COMMAND_ARGV}")

        set(MOLTORINO_SUPPORT_7ZIP_DIR "${CMAKE_SOURCE_DIR}/tools/support/7zip")
        if (EXISTS "${MOLTORINO_SUPPORT_7ZIP_DIR}/7z.exe")
            add_custom_command(TARGET ${EXECUTABLE_PROJECT} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${MOLTORINO_SUPPORT_7ZIP_DIR}"
                    "$<TARGET_FILE_DIR:${EXECUTABLE_PROJECT}>/support/7zip"
                VERBATIM)

            install(DIRECTORY "${MOLTORINO_SUPPORT_7ZIP_DIR}/"
                DESTINATION support/7zip)
        endif()

        install(TARGETS ${EXECUTABLE_PROJECT}
            RUNTIME_DEPENDENCIES
                PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
                POST_EXCLUDE_REGEXES ".*system32/.*\\.dll"
                DIRECTORIES ${QT_BIN_DIR}
            RUNTIME DESTINATION .)

        # Hardcoded list of DLLs to install from Qt - these are marked as optional since they only exist for vcpkg
        install(FILES
            ${QT_BIN_DIR}/jpeg62.dll
            ${QT_BIN_DIR}/libwebpdemux.dll
            ${QT_BIN_DIR}/libwebpmux.dll
            ${QT_BIN_DIR}/libwebp.dll
            ${QT_BIN_DIR}/libsharpyuv.dll
            DESTINATION .
            OPTIONAL)

        install(CODE "message(\"-- Running: ${WINDEPLOYQT_COMMAND} --dir \\\"\${CMAKE_INSTALL_PREFIX}\\\"\")")
        install(CODE "execute_process(COMMAND ${WINDEPLOYQT_COMMAND} --dir \"\${CMAKE_INSTALL_PREFIX}\" COMMAND_ERROR_IS_FATAL ANY)")
    elseif (APPLE)
        install(TARGETS ${EXECUTABLE_PROJECT}
                RUNTIME DESTINATION bin
                BUNDLE DESTINATION bin
                LIBRARY DESTINATION lib
                ARCHIVE DESTINATION lib/static
                )
    else ()
        install(TARGETS ${EXECUTABLE_PROJECT}
                RUNTIME DESTINATION bin
                LIBRARY DESTINATION lib
                ARCHIVE DESTINATION lib/static
                )

        install(FILES ${CMAKE_SOURCE_DIR}/resources/com.chatterino.chatterino.desktop
                RENAME com.moltobenne.moltorino.desktop
                DESTINATION share/applications
                )

        install(FILES ${CMAKE_SOURCE_DIR}/resources/icon.png
                RENAME com.moltobenne.moltorino.png
                DESTINATION share/icons/hicolor/256x256/apps
                )
    endif ()

    if(TARGET Velopack::Velopack)
        if(WIN32)
            set(_velopack_runtime_destination .)
            set(_velopack_license_destination licenses)
            set(_velopack_identity_destination .)
        elseif(APPLE)
            set(_velopack_runtime_destination bin/Moltorino7.app/Contents/Frameworks)
            set(_velopack_license_destination bin/Moltorino7.app/Contents/Resources/licenses)
            set(_velopack_identity_destination bin/Moltorino7.app/Contents/MacOS)
        else()
            set(_velopack_runtime_destination lib)
            set(_velopack_license_destination share/licenses/moltorino)
            set(_velopack_identity_destination bin)
        endif()
        if(MOLTORINO_VELOPACK_RUNTIME_OUTPUT)
            install(FILES "${MOLTORINO_VELOPACK_RUNTIME_OUTPUT}"
                DESTINATION "${_velopack_runtime_destination}")
        endif()
        install(FILES "${MOLTORINO_VELOPACK_LICENSE_FILE}"
            DESTINATION "${_velopack_license_destination}" RENAME Velopack.txt)
        install(FILES "${CMAKE_BINARY_DIR}/bin/moltorino-build-identity.json"
            DESTINATION "${_velopack_identity_destination}")
    endif()

    if(CHATTERINO_ENABLE_LTO)
        message(STATUS "Enabling LTO for ${EXECUTABLE_PROJECT}")
        set_property(TARGET ${EXECUTABLE_PROJECT}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif ()

if (USE_PRECOMPILED_HEADERS AND NOT APPLE)
    message(STATUS "Building with precompiled headers")
    target_precompile_headers(${LIBRARY_PROJECT} PRIVATE PrecompiledHeader.hpp)
else ()
    message(STATUS "Building without precompiled headers")
endif ()

# Enable autogeneration of Qts MOC/RCC/UIC
set_target_properties(${LIBRARY_PROJECT}
    PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
    )

# The version project has definitions about the build.
# To avoid recompilations because of changing preprocessor definitions,
# this is its own project.
set(VERSION_SOURCE_FILES common/Version.cpp common/Version.hpp)
add_library(${VERSION_PROJECT} STATIC ${VERSION_SOURCE_FILES})
set(MOLTORINO_INTERNAL_VERSION "M${PROJECT_VERSION}")

set(_moltorino_windows_app_id "")
if(WIN32)
    if(MOLTORINO_WINDOWS_APP_ID)
        set(_moltorino_windows_app_id "${MOLTORINO_WINDOWS_APP_ID}")
    elseif(NOT TARGET Velopack::Velopack)
        set(_moltorino_windows_app_id "Moltorino.SourceBuild")
    elseif(MOLTORINO_UPDATE_CHANNEL STREQUAL "internal")
        set(_moltorino_windows_app_id "MoltoBenne.Moltorino7UpdaterTest")
    else()
        set(_moltorino_windows_app_id "MoltoBenne.Moltorino7")
    endif()
    target_compile_definitions(${VERSION_PROJECT} PRIVATE
        MOLTORINO_WINDOWS_APP_ID="${_moltorino_windows_app_id}")
endif()

if(TARGET Velopack::Velopack)
    if(GIT_MODIFIED)
        set(_moltorino_source_dirty true)
    else()
        set(_moltorino_source_dirty false)
    endif()
    string(CONFIGURE [=[
{
  "schemaVersion": 1,
  "publicVersion": "@PROJECT_VERSION@",
  "buildId": "@MOLTORINO_INTERNAL_VERSION@",
  "channel": "@MOLTORINO_UPDATE_CHANNEL@",
  "appUserModelId": "@_moltorino_windows_app_id@",
  "platform": "@MOLTORINO_VELOPACK_PLATFORM@",
  "architecture": "@MOLTORINO_VELOPACK_RUNTIME_ID@",
  "sourceCommit": "@GIT_COMMIT@",
  "sourceDirty": @_moltorino_source_dirty@,
  "qtVersion": "@Qt6Core_VERSION@",
  "velopackVersion": "@MOLTORINO_VELOPACK_VERSION@"
}
]=] _moltorino_build_identity @ONLY)
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/bin/moltorino-build-identity.json"
        CONTENT "${_moltorino_build_identity}"
    )
endif()
target_compile_definitions(${VERSION_PROJECT} PRIVATE
    CHATTERINO_EXTRA_BUILD_STRING=${CHATTERINO_EXTRA_BUILD_STRING}
    MOLTORINO_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${WIN32}>:USEWINSDK>
    $<$<BOOL:${BUILD_WITH_CRASHPAD}>:CHATTERINO_WITH_CRASHPAD>
)

if(MOLTORINO_UPDATE_CHANNEL STREQUAL "internal")
    target_compile_definitions(${VERSION_PROJECT} PRIVATE MOLTORINO_INTERNAL_UPDATE_CHANNEL=1)
endif()

# source group for IDEs
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${VERSION_SOURCE_FILES})
target_include_directories(${VERSION_PROJECT} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${VERSION_PROJECT} PRIVATE Qt${MAJOR_QT_VERSION}::Core)
target_compile_definitions(${VERSION_PROJECT} PRIVATE
    CHATTERINO_GIT_HASH=\"${GIT_HASH}\"
    CHATTERINO_GIT_RELEASE=\"${GIT_RELEASE}\"
    CHATTERINO_GIT_COMMIT=\"${GIT_COMMIT}\"
    CHATTERINO_GIT_MODIFIED=${GIT_MODIFIED}
    CHATTERINO_NIGHTLY_BUILD=$<BOOL:${CHATTERINO_NIGHTLY_BUILD}>

    CHATTERINO_CMAKE_GEN_DATE=\"${cmake_gen_date}\"
)

target_link_libraries(${LIBRARY_PROJECT} PRIVATE ${VERSION_PROJECT})

target_compile_definitions(${LIBRARY_PROJECT} PUBLIC
    CHATTERINO
    UNICODE
    IRC_STATIC
    IRC_NAMESPACE=Communi
    $<$<BOOL:${WIN32}>:_WIN32_WINNT=0x0A00> # Windows 10
    $<$<BOOL:${BUILD_TESTS}>:CHATTERINO_WITH_TESTS>
    # Disable min/max macros from Windows.h
    $<$<BOOL:${WIN32}>:NOMINMAX>
    # Exclude some headers when including Windows.h
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    )

set(_c2_qt_defs
    QT_DISABLE_DEPRECATED_BEFORE=${CHATTERINO_MIN_QT_VERSION}
    QT_WARN_DEPRECATED_UP_TO=${CHATTERINO_MIN_QT_VERSION}
    QT_NO_KEYWORDS
)
target_compile_definitions(${VERSION_PROJECT} PUBLIC ${_c2_qt_defs})
target_compile_definitions(${LIBRARY_PROJECT} PUBLIC ${_c2_qt_defs})

if (USE_SYSTEM_QTKEYCHAIN)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC
        CMAKE_BUILD
        )
endif ()
if (WIN32)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC
        USEWINSDK
        )
    if (BUILD_APP)
        set_target_properties(${EXECUTABLE_PROJECT} PROPERTIES WIN32_EXECUTABLE TRUE)
    endif ()
endif ()
if (CHATTERINO_DEBUG_NATIVE_MESSAGES)
    target_compile_definitions(${LIBRARY_PROJECT} PRIVATE CHATTERINO_DEBUG_NM)
endif ()

if (MSVC)
    target_compile_options(${LIBRARY_PROJECT} PUBLIC /EHsc /bigobj /utf-8)
endif ()

if (APPLE AND BUILD_APP)
    set_target_properties(${EXECUTABLE_PROJECT} PROPERTIES MACOSX_BUNDLE TRUE)
    set_target_properties(${EXECUTABLE_PROJECT}
        PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Moltorino"
        MACOSX_BUNDLE_GUI_IDENTIFIER "app.moltobenne.moltorino7"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE moltorino.icns
        )
endif ()

target_include_directories(${LIBRARY_PROJECT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/autogen/)

# semver dependency https://github.com/Neargye/semver
target_include_directories(${LIBRARY_PROJECT} PUBLIC ${CMAKE_SOURCE_DIR}/lib/semver/include)

# expected-lite dependency https://github.com/martinmoene/expected-lite
target_include_directories(${LIBRARY_PROJECT} PUBLIC ${CMAKE_SOURCE_DIR}/lib/expected-lite/include)

# miniaudio dependency https://github.com/mackron/miniaudio
if (USE_SYSTEM_MINIAUDIO)
    message(STATUS "Building with system miniaudio")
    include(CheckIncludeFileCXX)
    CHECK_INCLUDE_FILE_CXX("miniaudio.h" MINIAUDIO_FOUND)
    if (NOT MINIAUDIO_FOUND)
        message(FATAL_ERROR "miniaudio.h not found on your system")
    endif()
else ()
    target_include_directories(${LIBRARY_PROJECT} PUBLIC ${CMAKE_SOURCE_DIR}/lib/miniaudio)
endif ()

if (UNIX)
    if (CMAKE_DL_LIBS)
        # libdl is a requirement for miniaudio on Linux
        message(STATUS "Linking with CMake DL libs: '${CMAKE_DL_LIBS}'")
        target_link_libraries(${LIBRARY_PROJECT} PUBLIC ${CMAKE_DL_LIBS})
    endif ()

    if (NOT APPLE AND BUILD_WITH_LIBNOTIFY)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)
        target_link_libraries(${LIBRARY_PROJECT} PRIVATE PkgConfig::LIBNOTIFY)
        target_compile_definitions(${LIBRARY_PROJECT} PUBLIC CHATTERINO_WITH_LIBNOTIFY)
    endif ()
endif ()

if (WIN32)
    target_link_libraries(${LIBRARY_PROJECT}
        PUBLIC
        WinToast
        crypt32)
endif ()

target_link_libraries(${LIBRARY_PROJECT}
        PUBLIC
        ${Boost_LIBRARIES}
        libvpx::libvpx
        webm::webm
        libyuv::libyuv
        )

target_link_libraries(${LIBRARY_PROJECT}
        PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        )

if (LIBRT)
    target_link_libraries(${LIBRARY_PROJECT}
        PUBLIC
        ${LIBRT}
        )
endif ()

if (BUILD_WITH_CRASHPAD)
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC CHATTERINO_WITH_CRASHPAD)
    target_link_libraries(${LIBRARY_PROJECT} PUBLIC crashpad::client)
endif()

# Configure compiler warnings
if (MSVC)
    # Change flags for RelWithDebInfo

    # Default: "/debug /INCREMENTAL"
    # Changes:
    #   - Disable incremental linking to reduce padding
    #   - Enable all optimizations - by default when /DEBUG is specified,
    #     these optimizations will be disabled. We need /DEBUG to generate a PDB.
    # See https://gitlab.kitware.com/cmake/cmake/-/issues/20812 for more details.
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "/DEBUG /INCREMENTAL:NO /OPT:REF,ICF,LBR")

    # Use the function inlining level from 'Release' mode (2).
    string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

    # Configure warnings

    # Someone adds /W3 before we add /W4.
    # This makes sure, only /W4 is specified.
    string(REPLACE "/W3" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # 4100 - "unreferenced formal parameter"
    #        There are a lot of functions and methods where
    #        an argument was given a name but never used.
    #        There's a clang-tidy rule that will catch this
    #        for new/updated functions/methods.
    #
    # 4267 - "possible loss of data in return"
    #        These are implicit conversions from size_t to int/qsizetype.
    #        We don't use size_t in a lot of cases, since
    #        Qt doesn't use it - it uses int (or qsizetype in Qt6).
    #
    # 4458 - "declaration of 'identifier' hides class member"
    #        We have a rule of exclusively using `this->`
    #        to access class members, thus it's fine to reclare a variable
    #        with the same name as a class member.
    target_compile_options(${LIBRARY_PROJECT} PUBLIC
        /W4
        # 5038 - warnings about initialization order
        /w15038
        # 4855 - implicit capture of 'this' via '[=]' is deprecated
        /w14855
        # Disable the following warnings (see reasoning above)
        /wd4100
        /wd4267
        /wd4458
        # Enable updated '__cplusplus' macro - workaround for CMake#18837
        /Zc:__cplusplus
    )

    # Warnings on clang-cl
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${LIBRARY_PROJECT} PUBLIC
            -Wno-missing-designated-field-initializers
        )
    endif()
else ()
    target_compile_options(${LIBRARY_PROJECT} PUBLIC
        -Wall
        # Disable the following warnings
        -Wno-unused-function
        -Wno-switch
        -Wno-deprecated-declarations
        -Wno-sign-compare

        # Disabling strict-aliasing warnings for now, although we probably want to re-enable this in the future
        -Wno-strict-aliasing

        -Werror=return-type
        -Werror=reorder
    )

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${LIBRARY_PROJECT} PUBLIC
            -Wno-unused-local-typedef
            -Wno-unused-private-field
            -Werror=inconsistent-missing-override
            -Werror=final-dtor-non-final-class
            -Werror=ambiguous-reversed-operator

        )
    else ()
        target_compile_options(${LIBRARY_PROJECT} PUBLIC
            -Wno-class-memaccess
        )
    endif()
endif ()

if(CHATTERINO_ENABLE_LTO)
    message(STATUS "Enabling LTO for ${LIBRARY_PROJECT}")
    set_property(TARGET ${LIBRARY_PROJECT}
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(NOT CHATTERINO_UPDATER)
    message(STATUS "Disabling the updater.")
    target_compile_definitions(${LIBRARY_PROJECT} PUBLIC CHATTERINO_DISABLE_UPDATER)
endif()

if (DOXYGEN_FOUND)
    message(STATUS "Doxygen found, adding doxygen target")
    # output will be in docs/html
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/docs")

    doxygen_add_docs(
        doxygen
        ${CMAKE_CURRENT_LIST_DIR}
    )
endif ()
