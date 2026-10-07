#pragma once

#include <cassert>
#include <memory>

namespace chatterino {

class ChatAutomationController;
class ChatRecordingController;
class HiddenUserController;
class FfzApBadges;
class BluzyrinoBadges;
class PotatCommands;
class YouTubeChatServer;
class TikTokChatServer;
namespace automod {
class AutoModReviewController;
}
class MoltorinoDailyMessage;
class Args;
class TwitchIrcServer;
class ITwitchIrcServer;
class PubSub;
class Updates;

class CommandController;
class AccountController;
class NotificationController;
class HighlightController;
class HotkeyController;
class IUserDataController;
class UserDataController;
class ISoundController;
class SoundController;
class ITwitchLiveController;
class TwitchLiveController;
class TwitchBadges;
#ifdef CHATTERINO_HAVE_PLUGINS
class PluginController;
#endif

class Modes;
class Theme;
class WindowManager;
class ILogging;
class Logging;
class Paths;
class EmoteController;
class Settings;
class Fonts;
class Toasts;
class IChatterinoBadges;
class ChatterinoBadges;
class SeventvPaints;
class FfzBadges;
class BttvBadges;
class SeventvBadges;
class SeventvPersonalEmotes;
class ImageUploader;
class SeventvAPI;
class CrashHandler;
class BttvEmotes;
class BttvLiveUpdates;
class FfzEmotes;
class SeventvEmotes;
class SeventvEventAPI;
class ILinkResolver;
class HomiesBadges;
class MoltorinoSupporterBadges;
class RepeatedMessageDetector;
class IStreamerMode;
class ITwitchUsers;
class NativeMessagingServer;
namespace pronouns {
class Pronouns;
}
namespace eventsub {
class IController;
}
class SpellChecker;

class KickChatServer;

class IApplication
{
public:
    IApplication();
    virtual ~IApplication();

    IApplication(const IApplication &) = delete;
    IApplication(IApplication &&) = delete;
    IApplication &operator=(const IApplication &) = delete;
    IApplication &operator=(IApplication &&) = delete;

    virtual bool isTest() const = 0;

    virtual ChatAutomationController *getChatAutomations() = 0;
    virtual ChatRecordingController *getChatRecordings() = 0;
    virtual HiddenUserController *getHiddenUsers() = 0;
    virtual FfzApBadges *getFfzApBadges() = 0;
    virtual BluzyrinoBadges *getBluzyrinoBadges() = 0;
    virtual PotatCommands *getPotatCommands() = 0;
    virtual YouTubeChatServer *getYouTubeChatServer() = 0;
    virtual TikTokChatServer *getTikTokChatServer() = 0;
    virtual automod::AutoModReviewController *getAutoModReview() = 0;

    virtual const Paths &getPaths() = 0;
    virtual const Args &getArgs() = 0;
    virtual Theme *getThemes() = 0;
    virtual Fonts *getFonts() = 0;
    virtual EmoteController *getEmotes() = 0;
    virtual AccountController *getAccounts() = 0;
    virtual HotkeyController *getHotkeys() = 0;
    virtual WindowManager *getWindows() = 0;
    virtual Toasts *getToasts() = 0;
    virtual CrashHandler *getCrashHandler() = 0;
    virtual CommandController *getCommands() = 0;
    virtual HighlightController *getHighlights() = 0;
    virtual NotificationController *getNotifications() = 0;
    virtual ITwitchIrcServer *getTwitch() = 0;
    virtual PubSub *getTwitchPubSub() = 0;
    virtual ILogging *getChatLogger() = 0;
    virtual IChatterinoBadges *getChatterinoBadges() = 0;
    virtual FfzBadges *getFfzBadges() = 0;
    virtual BttvBadges *getBttvBadges() = 0;
    virtual SeventvBadges *getSeventvBadges() = 0;
    virtual HomiesBadges *getHomiesBadges() = 0;
    virtual MoltorinoSupporterBadges *getMoltorinoSupporterBadges() = 0;
    virtual RepeatedMessageDetector *getRepeatedMessageDetector() = 0;
    virtual IUserDataController *getUserData() = 0;
    virtual ISoundController *getSound() = 0;
    virtual ITwitchLiveController *getTwitchLiveController() = 0;

    virtual SeventvPersonalEmotes *getSeventvPersonalEmotes() = 0;
    virtual SeventvPaints *getSeventvPaints() = 0;
    virtual TwitchBadges *getTwitchBadges() = 0;
    virtual ImageUploader *getImageUploader() = 0;
    virtual SeventvAPI *getSeventvAPI() = 0;
#ifdef CHATTERINO_HAVE_PLUGINS
    virtual PluginController *getPlugins() = 0;
#endif
    virtual Updates &getUpdates() = 0;
    virtual BttvEmotes *getBttvEmotes() = 0;
    virtual BttvLiveUpdates *getBttvLiveUpdates() = 0;
    virtual FfzEmotes *getFfzEmotes() = 0;
    virtual SeventvEmotes *getSeventvEmotes() = 0;
    virtual SeventvEventAPI *getSeventvEventAPI() = 0;
    virtual ILinkResolver *getLinkResolver() = 0;
    virtual IStreamerMode *getStreamerMode() = 0;
    virtual ITwitchUsers *getTwitchUsers() = 0;
    virtual pronouns::Pronouns *getPronouns() = 0;
    virtual eventsub::IController *getEventSub() = 0;
    virtual SpellChecker *getSpellChecker() = 0;
    virtual KickChatServer *getKickChatServer() = 0;
};

class Application : public IApplication
{
    const Paths &paths_;
    const Args &args_;
    int argc_{};
    char **argv_{};

public:
    Application(Settings &_settings, const Paths &paths, const Args &_args,
                Updates &_updates);
    ~Application() override;

    Application(const Application &) = delete;
    Application(Application &&) = delete;
    Application &operator=(const Application &) = delete;
    Application &operator=(Application &&) = delete;

    bool isTest() const override
    {
        return false;
    }

    void initialize(Settings &settings, const Modes &modes, const Paths &paths);
    void load();
    void aboutToQuit();
    void stop();

    int run();

    friend void test();

private:
    std::unique_ptr<Theme> themes;
    std::unique_ptr<Fonts> fonts;
    std::unique_ptr<Logging> logging;
    std::unique_ptr<EmoteController> emotes;
    std::unique_ptr<AccountController> accounts;
    std::unique_ptr<eventsub::IController> eventSub;
    std::unique_ptr<HotkeyController> hotkeys;
    std::unique_ptr<WindowManager> windows;
    std::unique_ptr<Toasts> toasts;
    std::unique_ptr<ImageUploader> imageUploader;
    std::unique_ptr<SeventvAPI> seventvAPI;
    std::unique_ptr<CrashHandler> crashHandler;
    std::unique_ptr<CommandController> commands;
    std::unique_ptr<ChatAutomationController> chatAutomations;
    std::unique_ptr<ChatRecordingController> chatRecordings;
    std::unique_ptr<NotificationController> notifications;
    std::unique_ptr<SeventvPaints> seventvPaints;
    std::unique_ptr<HiddenUserController> hiddenUsers;
    std::unique_ptr<HighlightController> highlights;
    std::unique_ptr<TwitchIrcServer> twitch;
    std::unique_ptr<FfzBadges> ffzBadges;
    std::unique_ptr<FfzApBadges> ffzApBadges;
    std::unique_ptr<BluzyrinoBadges> bluzyrinoBadges;
    std::unique_ptr<PotatCommands> potatCommands;
    std::unique_ptr<BttvBadges> bttvBadges;
    std::unique_ptr<SeventvBadges> seventvBadges;
    std::unique_ptr<HomiesBadges> homiesBadges;
    std::unique_ptr<MoltorinoSupporterBadges> moltorinoSupporterBadges;
    std::unique_ptr<RepeatedMessageDetector> repeatedMessageDetector;
    std::unique_ptr<automod::AutoModReviewController> autoModReview;
    std::unique_ptr<SeventvPersonalEmotes> seventvPersonalEmotes;
    std::unique_ptr<UserDataController> userData;
    std::unique_ptr<ISoundController> sound;
    std::unique_ptr<TwitchLiveController> twitchLiveController;
    std::unique_ptr<PubSub> twitchPubSub;
    std::unique_ptr<TwitchBadges> twitchBadges;
    std::unique_ptr<ChatterinoBadges> chatterinoBadges;
    std::unique_ptr<BttvEmotes> bttvEmotes;
    std::unique_ptr<BttvLiveUpdates> bttvLiveUpdates;
    std::unique_ptr<FfzEmotes> ffzEmotes;
    std::unique_ptr<SeventvEmotes> seventvEmotes;
    std::unique_ptr<SeventvEventAPI> seventvEventAPI;
    std::unique_ptr<ILinkResolver> linkResolver;
    std::unique_ptr<IStreamerMode> streamerMode;
    std::unique_ptr<ITwitchUsers> twitchUsers;
    std::unique_ptr<pronouns::Pronouns> pronouns;
    std::unique_ptr<SpellChecker> spellChecker;
    std::unique_ptr<KickChatServer> kickChatServer;
    std::unique_ptr<YouTubeChatServer> youtubeChatServer;
    std::unique_ptr<TikTokChatServer> tiktokChatServer;
    std::unique_ptr<MoltorinoDailyMessage> dailyMessage;
#ifdef CHATTERINO_HAVE_PLUGINS
    std::unique_ptr<PluginController> plugins;
#endif

public:
    const Paths &getPaths() override
    {
        return this->paths_;
    }
    const Args &getArgs() override
    {
        return this->args_;
    }
    ChatAutomationController *getChatAutomations() override;
    ChatRecordingController *getChatRecordings() override;
    HiddenUserController *getHiddenUsers() override;
    FfzApBadges *getFfzApBadges() override;
    BluzyrinoBadges *getBluzyrinoBadges() override;
    PotatCommands *getPotatCommands() override;
    YouTubeChatServer *getYouTubeChatServer() override;
    TikTokChatServer *getTikTokChatServer() override;
    automod::AutoModReviewController *getAutoModReview() override;
    Theme *getThemes() override;
    Fonts *getFonts() override;
    EmoteController *getEmotes() override;
    AccountController *getAccounts() override;
    HotkeyController *getHotkeys() override;
    WindowManager *getWindows() override;
    Toasts *getToasts() override;
    CrashHandler *getCrashHandler() override;
    CommandController *getCommands() override;
    NotificationController *getNotifications() override;
    HighlightController *getHighlights() override;
    ITwitchIrcServer *getTwitch() override;
    PubSub *getTwitchPubSub() override;
    ILogging *getChatLogger() override;
    FfzBadges *getFfzBadges() override;
    BttvBadges *getBttvBadges() override;
    SeventvBadges *getSeventvBadges() override;
    HomiesBadges *getHomiesBadges() override;
    MoltorinoSupporterBadges *getMoltorinoSupporterBadges() override;
    RepeatedMessageDetector *getRepeatedMessageDetector() override;
    IUserDataController *getUserData() override;
    ISoundController *getSound() override;
    ITwitchLiveController *getTwitchLiveController() override;
    TwitchBadges *getTwitchBadges() override;
    IChatterinoBadges *getChatterinoBadges() override;
    ImageUploader *getImageUploader() override;
    SeventvAPI *getSeventvAPI() override;
#ifdef CHATTERINO_HAVE_PLUGINS
    PluginController *getPlugins() override;
#endif
    Updates &getUpdates() override;

    SeventvPersonalEmotes *getSeventvPersonalEmotes() override;
    SeventvPaints *getSeventvPaints() override;

    BttvEmotes *getBttvEmotes() override;
    BttvLiveUpdates *getBttvLiveUpdates() override;
    FfzEmotes *getFfzEmotes() override;
    SeventvEmotes *getSeventvEmotes() override;
    SeventvEventAPI *getSeventvEventAPI() override;
    pronouns::Pronouns *getPronouns() override;
    eventsub::IController *getEventSub() override;

    ILinkResolver *getLinkResolver() override;
    IStreamerMode *getStreamerMode() override;
    ITwitchUsers *getTwitchUsers() override;
    SpellChecker *getSpellChecker() override;
    KickChatServer *getKickChatServer() override;

private:
    void initNm(const Modes &modes, const Paths &paths);

    std::unique_ptr<NativeMessagingServer> nmServer;
    Updates &updates;

    bool initialized{false};
};

IApplication *getApp();

IApplication *tryGetApp();

bool isAppAboutToQuit();
void requestApplicationQuit();

}
