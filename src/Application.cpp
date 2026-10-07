#include "Application.hpp"

#include "common/Args.hpp"
#include "common/Channel.hpp"
#include "common/Modes.hpp"
#include "common/Version.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/automod/AutoModReviewController.hpp"
#include "controllers/chat/ChatAutomationController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/ffzap/FfzApBadges.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/IvrApi.hpp"
#include "providers/potat/PotatCommands.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/tiktok/TikTokChatServer.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/youtube/YouTubeChatServer.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/NativeMessaging.hpp"
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/PluginController.hpp"
#endif
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/sound/MiniaudioBackend.hpp"
#include "controllers/sound/NullBackend.hpp"
#include "controllers/twitch/LiveController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/repetitions/RepeatedMessageDetector.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/PubSubMessages.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/helper/LoggingChannel.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoDailyMessage.hpp"
#include "providers/moltorino/MoltorinoPresence.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/moltorino/MoltorinoUpdater.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/Updates.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/PostToThread.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <miniaudio.h>
#include <QApplication>
#include <QDateTime>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

using namespace chatterino;

const QString BTTV_LIVE_UPDATES_URL = "wss://sockets.betterttv.net/ws";
const QString SEVENTV_EVENTAPI_URL = "wss://events.7tv.io/v3";
constexpr int STARTUP_MEMORY_RELIEF_DELAY_MS = 30'000;

std::atomic<bool> STOPPED{false};
std::atomic<bool> ABOUT_TO_QUIT{false};

ISoundController *makeSoundController(Settings &settings)
{
    SoundBackend soundBackend = settings.soundBackend;
    switch (soundBackend)
    {
        case SoundBackend::Miniaudio: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;

        case SoundBackend::Null: {
            return new NullBackend();
        }
        break;

        default: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;
    }
}

BttvLiveUpdates *makeBttvLiveUpdates(Settings &settings)
{
    bool enabled =
        settings.enableBTTVLiveUpdates &&
        (settings.enableBTTVChannelEmotes || settings.showBadgesBttv);

    if (enabled)
    {
        return new BttvLiveUpdates(BTTV_LIVE_UPDATES_URL);
    }

    return nullptr;
}

SeventvEventAPI *makeSeventvEventAPI(Settings &settings)
{
    bool enabled = settings.enableSevenTVEventAPI;

    if (enabled)
    {
        return new SeventvEventAPI(SEVENTV_EVENTAPI_URL %
                                   "?app=Chatterino&version=" %
                                   Version::instance().version());
    }

    return nullptr;
}

const QString TWITCH_PUBSUB_URL = "wss://pubsub-edge.twitch.tv";

IApplication *INSTANCE = nullptr;

}

namespace chatterino {

IApplication::IApplication()
{
    INSTANCE = this;
}

IApplication::~IApplication()
{
    INSTANCE = nullptr;
}

Application::Application(Settings &_settings, const Paths &paths,
                         const Args &_args, Updates &_updates)
    : paths_(paths)
    , args_(_args)
    , themes(new Theme(paths))
    , fonts(new Fonts(_settings))
    , logging(new Logging(_settings))
    , emotes(new EmoteController)
    , accounts(new AccountController)
    , eventSub(new eventsub::Controller())
    , hotkeys(new HotkeyController)
    , windows(new WindowManager(_args, paths, _settings, *this->themes,
                                *this->fonts))
    , toasts(new Toasts)
    , imageUploader(new ImageUploader)
    , seventvAPI(new SeventvAPI)
    , crashHandler(new CrashHandler(paths))

    , commands(new CommandController(paths))
    , chatAutomations(new ChatAutomationController(paths, this->commands.get()))
    , chatRecordings(new ChatRecordingController(paths))
    , notifications(new NotificationController)
    , seventvPaints(new SeventvPaints)
    , hiddenUsers(new HiddenUserController(_settings.hiddenUsers))
    , highlights(new HighlightController(_settings, this->accounts.get()))
    , twitch(new TwitchIrcServer)
    , ffzBadges(new FfzBadges)
    , ffzApBadges(new FfzApBadges)
    , bluzyrinoBadges(new BluzyrinoBadges)
    , potatCommands(new PotatCommands)
    , bttvBadges(new BttvBadges)
    , seventvBadges(new SeventvBadges)
    , homiesBadges(new HomiesBadges)
    , moltorinoSupporterBadges(new MoltorinoSupporterBadges)
    , repeatedMessageDetector(new RepeatedMessageDetector)
    , autoModReview(new automod::AutoModReviewController)
    , seventvPersonalEmotes(new SeventvPersonalEmotes)
    , userData(new UserDataController(paths))
    , sound(makeSoundController(_settings))
    , twitchLiveController(new TwitchLiveController)
    , twitchPubSub(new PubSub(TWITCH_PUBSUB_URL))
    , twitchBadges(new TwitchBadges)
    , chatterinoBadges(new ChatterinoBadges)
    , bttvEmotes(new BttvEmotes)
    , bttvLiveUpdates(makeBttvLiveUpdates(_settings))
    , ffzEmotes(new FfzEmotes)
    , seventvEmotes(new SeventvEmotes)
    , seventvEventAPI(makeSeventvEventAPI(_settings))
    , linkResolver(new LinkResolver)
    , streamerMode(new StreamerMode)
    , twitchUsers(new TwitchUsers)
    , pronouns(new pronouns::Pronouns)
    , spellChecker(new SpellChecker)
    , kickChatServer(new KickChatServer)
    , youtubeChatServer(new YouTubeChatServer)
    , tiktokChatServer(new TikTokChatServer)
    , dailyMessage(new MoltorinoDailyMessage)
#ifdef CHATTERINO_HAVE_PLUGINS
    , plugins(new PluginController(paths))
#endif
    , nmServer(new NativeMessagingServer())
    , updates(_updates)
{
}

Application::~Application()
{

    INSTANCE = nullptr;
}

void Application::initialize(Settings &settings, const Modes &modes,
                             const Paths &paths)
{
    assert(!this->initialized);

    if (!this->args_.isFramelessEmbed)
    {
        getSettings()->currentVersion.setValue(Version::instance().version());
    }
    this->emotes->initialize();
    IvrApi::initialize();

    this->accounts->load();
    this->autoModReview->initialize();

    this->windows->initialize();

    this->ffzBadges->load();
    this->ffzApBadges->initialize();
    this->bluzyrinoBadges->initialize();
    this->moltorinoSupporterBadges->initialize();

    this->bttvEmotes->loadEmotes();
    this->ffzEmotes->loadEmotes();
    this->seventvEmotes->loadGlobalEmotes();

    this->twitch->initialize();
    this->kickChatServer->initialize();
    this->youtubeChatServer->initialize();
    this->tiktokChatServer->initialize();

    this->notifications->initialize();

    this->twitchBadges->loadTwitchBadges();

#ifdef CHATTERINO_HAVE_PLUGINS
    this->plugins->initialize(settings);
#endif

#ifndef Q_OS_WIN
    if (!this->args_.isFramelessEmbed && this->args_.crashRecovery)
    {
        if (auto *selected =
                this->windows->getMainWindow().getNotebook().getSelectedPage())
        {
            if (auto *container = dynamic_cast<SplitContainer *>(selected))
            {
                for (auto &&split : container->getSplits())
                {
                    if (auto channel = split->getChannel(); !channel->isEmpty())
                    {
                        channel->addSystemMessage(
                            "Chatterino unexpectedly crashed and restarted. "
                            "You can disable automatic restarts in the "
                            "settings.");
                    }
                }
            }
        }
    }
#endif

    if (!this->args_.isFramelessEmbed)
    {
        this->initNm(modes, paths);
    }

    this->twitch->initEventAPIs(this->bttvLiveUpdates.get(),
                                this->seventvEventAPI.get());

    this->streamerMode->start();

    getMoltorinoPresence()->init();
    getMoltorinoUpdater()->init(!this->args_.isFramelessEmbed &&
                                !modes.isPortable &&
                                !modes.isExternallyPackaged);

    {
        auto &s = *getSettings();
        const auto clientId = s.botBadgeClientID.getValue().trimmed();
        const auto clientSecret = s.botBadgeClientSecret.getValue().trimmed();
        const auto expiryStr = s.botBadgeAppTokenExpiry.getValue().trimmed();

        if (!clientId.isEmpty() && !clientSecret.isEmpty())
        {
            bool needsRefresh = false;

            if (expiryStr.isEmpty())
            {
                needsRefresh = true;
            }
            else
            {
                auto expiry =
                    QDateTime::fromString(expiryStr, Qt::ISODate);

                needsRefresh =
                    !expiry.isValid() ||
                    QDateTime::currentDateTimeUtc().secsTo(expiry) < 86400;
            }

            if (needsRefresh)
            {
                QUrl tokenUrl("https://id.twitch.tv/oauth2/token");
                QUrlQuery tokenQuery;
                tokenQuery.addQueryItem("client_id", clientId);
                tokenQuery.addQueryItem("client_secret", clientSecret);
                tokenQuery.addQueryItem("grant_type", "client_credentials");
                NetworkRequest(tokenUrl, NetworkRequestType::Post)
                    .header("Content-Type", "application/x-www-form-urlencoded")
                    .payload(tokenQuery.toString(QUrl::FullyEncoded).toUtf8())
                    .hideRequestBody()
                    .maximumResponseSize(1024 * 1024)
                    .timeout(15000)
                    .onSuccess([clientId, clientSecret,
                                previousToken =
                                    s.botBadgeAppAccessToken.getValue()](
                                   const NetworkResult &res) {
                        auto json = res.parseJson();
                        auto token =
                            json.value("access_token").toString().trimmed();
                        auto expiresIn = json.value("expires_in").toInt();

                        if (!token.isEmpty())
                        {
                            auto &settings = *getSettings();
                            if (settings.botBadgeClientID.getValue().trimmed() !=
                                    clientId ||
                                settings.botBadgeClientSecret.getValue().trimmed() !=
                                    clientSecret ||
                                settings.botBadgeAppAccessToken.getValue() !=
                                    previousToken)
                            {
                                return;
                            }
                            settings.botBadgeAppAccessToken = token;
                            settings.botBadgeAppTokenExpiry =
                                QDateTime::currentDateTimeUtc()
                                    .addSecs(expiresIn)
                                    .toString(Qt::ISODate);
                            settings.requestSave();

                            qCDebug(chatterinoApp)
                                << "Bot badge app token refreshed.";
                        }
                    })
                    .onError([](const NetworkResult &res) {
                        qCWarning(chatterinoApp)
                            << "Failed to refresh bot badge app token:"
                            << res.formatError();
                    })
                    .execute();
            }
        }
    }

    this->initialized = true;
}

int Application::run()
{
    QTimer::singleShot(0, this->chatRecordings.get(), [this] {
        this->chatRecordings->checkRecovery();
    });
    assert(this->initialized);

    this->twitch->connect();

    if (!this->args_.isFramelessEmbed)
    {
        this->windows->getMainWindow().show();
    }

    getSettings()->enableBTTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllBTTVChannelEmotes();
        },
        false);
    getSettings()->enableFFZChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllFFZChannelEmotes();
        },
        false);
    getSettings()->enableSevenTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllSevenTVChannelEmotes();
        },
        false);

    QTimer::singleShot(2500, qApp, [] {
        getMoltorinoPresence()->startHeartbeat();
    });
    QTimer::singleShot(STARTUP_MEMORY_RELIEF_DELAY_MS, qApp, [] {
        requestMemoryPressureRelief();
    });

    MoltorinoAuth::scheduleStartupRefresh();
    this->dailyMessage->start();

    return QApplication::exec();
}

Theme *Application::getThemes()
{
    assertInGuiThread();
    assert(this->themes);

    return this->themes.get();
}

Fonts *Application::getFonts()
{
    assertInGuiThread();
    assert(this->fonts);

    return this->fonts.get();
}

EmoteController *Application::getEmotes()
{
    assertInGuiThread();
    assert(this->emotes);

    return this->emotes.get();
}

AccountController *Application::getAccounts()
{
    assertInGuiThread();
    assert(this->accounts);

    return this->accounts.get();
}

HotkeyController *Application::getHotkeys()
{
    assertInGuiThread();
    assert(this->hotkeys);

    return this->hotkeys.get();
}

WindowManager *Application::getWindows()
{
    assertInGuiThread();
    assert(this->windows);

    return this->windows.get();
}

Toasts *Application::getToasts()
{
    assertInGuiThread();
    assert(this->toasts);

    return this->toasts.get();
}

CrashHandler *Application::getCrashHandler()
{
    assertInGuiThread();
    assert(this->crashHandler);

    return this->crashHandler.get();
}

CommandController *Application::getCommands()
{
    assertInGuiThread();
    assert(this->commands);

    return this->commands.get();
}

NotificationController *Application::getNotifications()
{
    assertInGuiThread();
    assert(this->notifications);

    return this->notifications.get();
}

HighlightController *Application::getHighlights()
{
    assertInGuiThread();
    assert(this->highlights);

    return this->highlights.get();
}

FfzBadges *Application::getFfzBadges()
{
    assertInGuiThread();
    assert(this->ffzBadges);

    return this->ffzBadges.get();
}

BttvBadges *Application::getBttvBadges()
{

    assert(this->bttvBadges);

    return this->bttvBadges.get();
}

SeventvBadges *Application::getSeventvBadges()
{

    assert(this->seventvBadges);

    return this->seventvBadges.get();
}

HomiesBadges *Application::getHomiesBadges()
{

    assert(this->homiesBadges);

    return this->homiesBadges.get();
}

MoltorinoSupporterBadges *Application::getMoltorinoSupporterBadges()
{
    assert(this->moltorinoSupporterBadges);

    return this->moltorinoSupporterBadges.get();
}

RepeatedMessageDetector *Application::getRepeatedMessageDetector()
{
    assertInGuiThread();
    assert(this->repeatedMessageDetector);

    return this->repeatedMessageDetector.get();
}

IUserDataController *Application::getUserData()
{
    assertInGuiThread();

    return this->userData.get();
}

ISoundController *Application::getSound()
{
    assertInGuiThread();

    return this->sound.get();
}

ITwitchLiveController *Application::getTwitchLiveController()
{
    assertInGuiThread();
    assert(this->twitchLiveController);

    return this->twitchLiveController.get();
}

TwitchBadges *Application::getTwitchBadges()
{
    assertInGuiThread();
    assert(this->twitchBadges);

    return this->twitchBadges.get();
}

IChatterinoBadges *Application::getChatterinoBadges()
{
    assertInGuiThread();
    assert(this->chatterinoBadges);

    return this->chatterinoBadges.get();
}

ImageUploader *Application::getImageUploader()
{
    assertInGuiThread();
    assert(this->imageUploader);

    return this->imageUploader.get();
}

SeventvAPI *Application::getSeventvAPI()
{
    assertInGuiThread();
    assert(this->seventvAPI);

    return this->seventvAPI.get();
}

#ifdef CHATTERINO_HAVE_PLUGINS
PluginController *Application::getPlugins()
{
    assertInGuiThread();
    assert(this->plugins);

    return this->plugins.get();
}
#endif

Updates &Application::getUpdates()
{
    assertInGuiThread();

    return this->updates;
}

ITwitchIrcServer *Application::getTwitch()
{
    return this->twitch.get();
}

PubSub *Application::getTwitchPubSub()
{
    assertInGuiThread();

    return this->twitchPubSub.get();
}

ILogging *Application::getChatLogger()
{
    assertInGuiThread();
    assert(this->logging);

    return this->logging.get();
}

ILinkResolver *Application::getLinkResolver()
{
    assertInGuiThread();

    return this->linkResolver.get();
}

IStreamerMode *Application::getStreamerMode()
{
    return this->streamerMode.get();
}

ITwitchUsers *Application::getTwitchUsers()
{
    assertInGuiThread();
    assert(this->twitchUsers);

    return this->twitchUsers.get();
}

BttvEmotes *Application::getBttvEmotes()
{
    assertInGuiThread();
    assert(this->bttvEmotes);

    return this->bttvEmotes.get();
}

BttvLiveUpdates *Application::getBttvLiveUpdates()
{
    assertInGuiThread();

    return this->bttvLiveUpdates.get();
}

FfzEmotes *Application::getFfzEmotes()
{
    assertInGuiThread();
    assert(this->ffzEmotes);

    return this->ffzEmotes.get();
}

SeventvEmotes *Application::getSeventvEmotes()
{
    assertInGuiThread();
    assert(this->seventvEmotes);

    return this->seventvEmotes.get();
}

SeventvPersonalEmotes *Application::getSeventvPersonalEmotes()
{
    assert(this->seventvPersonalEmotes);

    return this->seventvPersonalEmotes.get();
}

SeventvPaints *Application::getSeventvPaints()
{
    assert(this->seventvPaints);

    return this->seventvPaints.get();
}

SeventvEventAPI *Application::getSeventvEventAPI()
{
    assertInGuiThread();

    return this->seventvEventAPI.get();
}

pronouns::Pronouns *Application::getPronouns()
{

    assert(this->pronouns);

    return this->pronouns.get();
}

eventsub::IController *Application::getEventSub()
{
    assert(this->eventSub);

    return this->eventSub.get();
}

SpellChecker *Application::getSpellChecker()
{
    assertInGuiThread();
    assert(this->spellChecker);

    return this->spellChecker.get();
}

KickChatServer *Application::getKickChatServer()
{
    assertInGuiThread();
    assert(this->kickChatServer);

    return this->kickChatServer.get();
}

ChatRecordingController *Application::getChatRecordings()
{
    return this->chatRecordings.get();
}

ChatAutomationController *Application::getChatAutomations()
{
    assertInGuiThread();
    assert(this->chatAutomations);

    return this->chatAutomations.get();
}

FfzApBadges *Application::getFfzApBadges()
{
    assert(this->ffzApBadges);
    return this->ffzApBadges.get();
}

BluzyrinoBadges *Application::getBluzyrinoBadges()
{
    assert(this->bluzyrinoBadges);
    return this->bluzyrinoBadges.get();
}

PotatCommands *Application::getPotatCommands()
{
    assertInGuiThread();
    assert(this->potatCommands);
    return this->potatCommands.get();
}

HiddenUserController *Application::getHiddenUsers()
{
    assert(this->hiddenUsers);
    return this->hiddenUsers.get();
}

automod::AutoModReviewController *Application::getAutoModReview()
{
    assertInGuiThread();
    assert(this->autoModReview);
    return this->autoModReview.get();
}

YouTubeChatServer *Application::getYouTubeChatServer()
{
    assertInGuiThread();
    assert(this->youtubeChatServer);

    return this->youtubeChatServer.get();
}

TikTokChatServer *Application::getTikTokChatServer()
{
    assertInGuiThread();
    return this->tiktokChatServer.get();
}

void Application::aboutToQuit()
{
    this->chatRecordings->shutdown();
    ABOUT_TO_QUIT.store(true);

    getMoltorinoPresence()->stopHeartbeat();

    this->eventSub->setQuitting();

    this->twitch->aboutToQuit();

    this->hotkeys->save();
    this->windows->save();

    getMoltorinoUpdater()->prepareForApplicationQuit();

    this->windows->closeAll();
}

void Application::stop()
{
    getMoltorinoPresence()->stopHeartbeat();
    this->nmServer.reset();
    this->tiktokChatServer.reset();
    this->dailyMessage.reset();
    this->chatRecordings.reset();
    this->youtubeChatServer.reset();
#ifdef CHATTERINO_HAVE_PLUGINS
    this->plugins.reset();
#endif
    this->pronouns.reset();
    this->twitchUsers.reset();
    this->streamerMode.reset();
    this->linkResolver.reset();
    this->seventvEventAPI.reset();
    this->seventvEmotes.reset();
    this->ffzEmotes.reset();
    this->bttvLiveUpdates.reset();
    this->bttvEmotes.reset();
    this->chatterinoBadges.reset();
    this->twitchBadges.reset();
    this->twitchPubSub.reset();
    this->twitchLiveController.reset();
    this->sound.reset();
    this->userData.reset();
    this->seventvBadges.reset();
    this->ffzBadges.reset();
    this->ffzApBadges.reset();
    this->bluzyrinoBadges.reset();
    this->potatCommands.reset();
    this->homiesBadges.reset();
    this->twitch.reset();
    this->highlights.reset();
    this->notifications.reset();
    this->crashHandler.reset();
    this->seventvAPI.reset();
    this->imageUploader.reset();
    this->toasts.reset();
    this->windows.reset();
    this->chatAutomations.reset();
    this->commands.reset();
    this->hotkeys.reset();
    this->eventSub.reset();
    this->accounts.reset();
    this->emotes.reset();
    this->logging.reset();
    this->fonts.reset();
    this->themes.reset();
    this->spellChecker.reset();

    STOPPED.store(true);
}

void Application::initNm(const Modes &modes, const Paths &paths)
{
    (void)modes;
    (void)paths;

#if defined QT_NO_DEBUG || defined CHATTERINO_DEBUG_NM
    registerNmHost(modes, paths);
    this->nmServer->start();
#endif
}

IApplication *getApp()
{
    assert(INSTANCE != nullptr);
    assert(STOPPED.load() == false);

    return INSTANCE;
}

IApplication *tryGetApp()
{
    return INSTANCE;
}

bool isAppAboutToQuit()
{
    return ABOUT_TO_QUIT.load();
}

void requestApplicationQuit()
{
    if (auto *app = tryGetApp())
    {
        if (auto *recordings = app->getChatRecordings())
        {
            recordings->finishBeforeQuit([] {
                QApplication::exit();
            });
            return;
        }
    }
    QApplication::exit();
}

}
