// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitHeader.hpp"

#include "Application.hpp"
#include "common/network/NetworkCommon.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/chat/ChatAutomationController.hpp"
#include "controllers/commands/builtin/Misc.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/twitch/ChannelManagement.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutHelper.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/ChannelManagementDialog.hpp"
#include "widgets/dialogs/ChatAutomationDialog.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/AutoModReviewBar.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CommonTexts.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/TooltipWidget.hpp"
#include "widgets/Window.hpp"

#include <QDrag>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using namespace chatterino;

/// The width of the standard button.
constexpr const int BUTTON_WIDTH = 28;

/// The width of the "Add split" button.
///
/// This matches the scrollbar's full width.
constexpr const int ADD_SPLIT_BUTTON_WIDTH = 16;

// 5 minutes
constexpr const qint64 THUMBNAIL_MAX_AGE_MS = 5LL * 60 * 1000;

int streamThumbnailHeight()
{
    switch (getSettings()->thumbnailSizeStream.getValue())
    {
        case 1:
            return 45;
        case 2:
            return 90;
        case 3:
            return 203;
        default:
            return 0;
    }
}

bool hasAnyModRights(const ChannelPtr &rootChannel)
{
    if (auto *multi = dynamic_cast<MultiChannel *>(rootChannel.get()))
    {
        return std::ranges::any_of(multi->channels(), [&](const auto &child) {
            return child.channel && child.channel->hasModRights();
        });
    }
    return rootChannel && rootChannel->hasModRights();
}

bool canShowChatterList(const ChannelPtr &rootChannel)
{
    const auto canUseTwitchChannel = [](const ChannelPtr &channel) {
        const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
        return twitch && (twitch->hasModRights() ||
                          getSettings()->showChatterListInAllTwitchChannels);
    };

    if (const auto multi = std::dynamic_pointer_cast<MultiChannel>(rootChannel))
    {
        return std::ranges::any_of(multi->channels(), [&](const auto &child) {
            return child.platform == MultiChannel::Platform::TikTok ||
                   (child.platform == MultiChannel::Platform::Twitch &&
                    canUseTwitchChannel(child.channel));
        });
    }
    return (rootChannel && rootChannel->isTikTokChannel()) ||
           canUseTwitchChannel(rootChannel);
}

bool canShowChannelManagementButton(const TwitchChannel &channel)
{
    if (channel.isEmpty())
    {
        return false;
    }

    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (current && !current->isAnon())
    {
        const auto roomId = channel.roomId();
        if ((!roomId.isEmpty() && current->getUserId() == roomId) ||
            (roomId.isEmpty() &&
             current->getUserName().compare(channel.getName(),
                                            Qt::CaseInsensitive) == 0))
        {
            return true;
        }
    }

    if (ChannelManagement::hasVerifiedEditorAccess(channel.roomId()))
    {
        return true;
    }

    const auto matchesChannel = [&channel](const QString &userId,
                                           const QString &login) {
        if (!channel.roomId().isEmpty() && !userId.isEmpty())
        {
            return userId == channel.roomId();
        }
        return !login.isEmpty() &&
               login.compare(channel.getName(), Qt::CaseInsensitive) == 0;
    };

    for (const auto &account : MoltorinoAuth::accounts())
    {
        if (!account.enabled || !account.valid || account.token.isEmpty())
        {
            continue;
        }
        if (matchesChannel(account.userId, account.login))
        {
            return true;
        }
    }
    return false;
}

QString formatStreamThumbnail(const QString &thumbnail, bool limitSize = false)
{
    const auto height = streamThumbnailHeight();
    if (height == 0)
    {
        return {};
    }

    if (thumbnail.isEmpty())
    {
        return QStringLiteral("Couldn't fetch thumbnail<br>");
    }

    QString size;
    if (limitSize)
    {
        size = QStringLiteral(" height=\"") % QString::number(height) % '"';
    }

    return u"<img " % size % u" src=\"data:image/jpg;base64, " % thumbnail %
           u"\"><br>";
}

QString formatYouTubeTooltip(const QString &title, const QString &thumbnail)
{
    const auto escapedTitle = title.toHtmlEscaped();
    const auto titleLine = escapedTitle.isEmpty()
                               ? QString{}
                               : escapedTitle + QStringLiteral("<br><br>");
    return u"<p style=\"text-align: center;\">" % titleLine %
           formatStreamThumbnail(thumbnail, true) % u"Live on YouTube</p>";
}

bool canUseFollowButtonForChannel(const TwitchChannel &channel)
{
    QString ignored;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&ignored);
    if (!auth.hasToken())
    {
        return false;
    }

    const auto roomId = channel.roomId();
    if (!roomId.isEmpty() && !auth.userId.isEmpty() && auth.userId == roomId)
    {
        return false;
    }

    return auth.login.isEmpty() ||
           auth.login.compare(channel.getName(), Qt::CaseInsensitive) != 0;
}

auto formatRoomModeUnclean(const TwitchChannel::RoomModes &modes) -> QString
{
    QString text;

    if (modes.r9k)
    {
        text += "r9k, ";
    }
    if (modes.slowMode > 0)
    {
        text += QString("slow(%1), ").arg(localizeNumbers(modes.slowMode));
    }
    if (modes.emoteOnly)
    {
        text += "emote, ";
    }
    if (modes.submode)
    {
        text += "sub, ";
    }
    if (modes.followerOnly != -1)
    {
        if (modes.followerOnly != 0)
        {
            text += QString("follow(%1), ")
                        .arg(formatDurationExact(
                            std::chrono::minutes{modes.followerOnly}));
        }
        else
        {
            text += QString("follow, ");
        }
    }

    return text;
}

QString formatRoomModeUnclean(const KickChannel::RoomModes &modes)
{
    TwitchChannel::RoomModes twitch{
        .submode = modes.subscribersMode,
        .r9k = false,
        .emoteOnly = modes.emotesMode,
        .followerOnly = -1,
        .slowMode = 0,
    };
    if (modes.followersModeDuration)
    {
        twitch.followerOnly =
            static_cast<int>(modes.followersModeDuration->count());
    }
    if (modes.slowModeDuration)
    {
        twitch.slowMode = static_cast<int>(modes.slowModeDuration->count());
    }
    return formatRoomModeUnclean(twitch);
}

void cleanRoomModeText(QString &text, bool hasModRights)
{
    if (text.length() > 2)
    {
        text = text.mid(0, text.size() - 2);
    }

    if (!text.isEmpty())
    {
        static QRegularExpression commaReplacement("^(.+?, .+?,) (.+)$");

        auto match = commaReplacement.match(text);
        if (match.hasMatch())
        {
            text = match.captured(1) + '\n' + match.captured(2);
        }
    }

    if (text.isEmpty() && hasModRights)
    {
        text = "none";
    }
}

auto formatTooltip(const TwitchChannel::StreamStatus &s, QString thumbnail,
                   bool limitSize = false)
{
    auto title = [&s]() -> QString {
        if (s.title.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.title.toHtmlEscaped() + "<br><br>";
    }();

    auto tooltip = formatStreamThumbnail(thumbnail, limitSize);

    auto game = [&s]() -> QString {
        if (s.game.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.game.toHtmlEscaped() + "<br>";
    }();

    auto extraStreamData = [&s]() -> QString {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideViewerCountAndDuration)
        {
            return QStringLiteral(
                "<span style=\"color: #808892;\">&lt;Streamer "
                "Mode&gt;</span>");
        }

        return QString("%1 for %2 with %3 viewers")
            .arg(s.rerun ? "Vod-casting" : "Live")
            .arg(s.uptime)
            .arg(localizeNumbers(s.viewerCount));
    }();

    return QString("<p style=\"text-align: center;\">" +  //
                   title +                                //
                   tooltip +                              //
                   game +                                 //
                   extraStreamData +                      //
                   "</p>"                                 //
    );
}

auto formatOfflineTooltip(const TwitchChannel::StreamStatus &s)
{
    return QString("<p style=\"text-align: center;\">Offline<br>%1</p>")
        .arg(s.title.toHtmlEscaped());
}

auto formatTitle(const TwitchChannel::StreamStatus &s, Settings &settings)
{
    auto title = QString();

    // live
    if (s.rerun)
    {
        title += " (rerun)";
    }
    else if (s.streamType.isEmpty())
    {
        title += " (" + s.streamType + ")";
    }
    else
    {
        title += " (live)";
    }

    // description
    if (settings.headerUptime)
    {
        title += " - " + s.uptime;
    }
    if (settings.headerViewerCount)
    {
        title += " - " + localizeNumbers(s.viewerCount);
    }
    if (settings.headerGame && !s.game.isEmpty())
    {
        title += " - " + s.game;
    }
    if (settings.headerStreamTitle && !s.title.isEmpty())
    {
        title += " - " + s.title.simplified();
    }

    return title;
}

SvgButton::Src followButtonSource(bool following)
{
    if (following)
    {
        return {
            .dark = ":/buttons/followEnabled-darkMode.svg",
            .light = ":/buttons/followEnabled-lightMode.svg",
            .useAccent = true,
        };
    }

    return {
        .dark = ":/buttons/followDisabled-darkMode.svg",
        .light = ":/buttons/followDisabled-lightMode.svg",
    };
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const KickChannel::StreamData &data)
{
    return {
        .live = data.isLive,
        .viewerCount = static_cast<unsigned>(data.viewerCount),
        .title = data.title,
        .game = data.category,
        .uptime = data.uptime,
        .streamType = QStringLiteral("live"),
    };
}

auto distance(QPoint a, QPoint b)
{
    auto x = std::abs(a.x() - b.x());
    auto y = std::abs(a.y() - b.y());

    return std::sqrt(x * x + y * y);
}

}  // namespace

namespace chatterino {

SplitHeader::SplitHeader(Split *split)
    : BaseWidget(split)
    , split_(split)
{
    this->initializeLayout();

    this->setMouseTracking(true);
    this->updateChannelText();
    this->handleChannelChanged();
    this->updateIcons();

    if (auto *recordings = getApp()->getChatRecordings())
    {
        auto updateRecording = [this, recordings] {
            auto *tab = recordingTabFor(this->split_);
            const auto state = recordings->status(tab);
            const bool active = recordings->isActive(tab);
            const bool available = active || recordings->canStart(tab);
            const auto action =
                active ? QString("Stop tab recording") : QString("Record tab");
            const auto hotkey = getApp()->getHotkeys()->getDisplaySequence(
                HotkeyCategory::Split, "toggleChatRecording");
            this->recordingButton_->setVisible(
                available &&
                recordings->options().value("showHeaderButton").toBool());
            const bool light = this->theme->isLightTheme();
            this->recordingButton_->setColor(
                !active                ? QColor(light ? "#424242" : "#c0c0c0")
                : state == "Recording" ? QColor(light ? "#c63743" : "#e95762")
                                       : QColor(light ? "#98671b" : "#d6a34a"));
            this->recordingButton_->setAccessibleName(action);
            this->recordingButton_->setToolTip(
                action +
                (hotkey.isEmpty()
                     ? QString{}
                     : " (" + hotkey.toString(QKeySequence::NativeText) + ")") +
                (state.isEmpty() ? QString{} : ". " + state));
            this->recordingButton_->setEnabled(state != "Saving" && available);
        };
        connect(recordings, &ChatRecordingController::stateChanged, this,
                updateRecording);
        this->managedConnections_.managedConnect(
            getApp()->getHotkeys()->onItemsUpdated, updateRecording);
        this->managedConnections_.managedConnect(this->theme->updated,
                                                 updateRecording);
        connect(this->recordingButton_, &Button::leftClicked, this,
                [this, recordings] {
                    recordings->toggle(recordingTabFor(this->split_));
                });
        updateRecording();
    }

    // The lifetime of these signals are tied to the lifetime of the Split.
    // Since the SplitHeader is owned by the Split, they will always be destroyed
    // at the same time.
    std::ignore = this->split_->focused.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->focusLost.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->channelChanged.connect([this]() {
        this->handleChannelChanged();
    });

    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this] {
            if (auto *twitchChannel =
                    dynamic_cast<TwitchChannel *>(
                        this->split_->getSelectedChannel().get());
                twitchChannel != nullptr && !twitchChannel->isEmpty() &&
                getSettings()->showFollowButtonInSplitHeader &&
                canUseFollowButtonForChannel(*twitchChannel))
            {
                twitchChannel->refreshFollowingStatus(false);
            }
            this->updateIcons();
        });
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->youtube.currentChanged, [this] {
            this->updateIcons();
        });

    auto _ = [this](const auto &, const auto &) {
        this->updateChannelText();
    };
    getSettings()->headerViewerCount.connect(_, this->managedConnections_);
    getSettings()->headerStreamTitle.connect(_, this->managedConnections_);
    getSettings()->headerGame.connect(_, this->managedConnections_);
    getSettings()->thumbnailSizeStream.connect(_, this->managedConnections_);
    getSettings()->headerUptime.connect(_, this->managedConnections_);
    getSettings()->showFollowButtonInSplitHeader.connect(
        [this](bool enabled, auto) {
            if (enabled)
            {
                if (auto *twitchChannel =
                        dynamic_cast<TwitchChannel *>(
                            this->split_->getSelectedChannel().get()))
                {
                    if (canUseFollowButtonForChannel(*twitchChannel))
                    {
                        twitchChannel->refreshFollowingStatus(false);
                    }
                }
            }
            this->updateIcons();
        },
        this->managedConnections_);
    getSettings()->showChatterListInAllTwitchChannels.connect(
        [this](bool, auto) {
            this->updateIcons();
        },
        this->managedConnections_);
    getSettings()->showSharedChatChannelSelector.connect(
        [this](bool enabled, auto) {
            if (!enabled && !this->hiddenSharedChatSources_.isEmpty())
            {
                this->hiddenSharedChatSources_.clear();
                this->applySharedChatFilter();
            }
            this->updateSharedChatButton();
        },
        this->managedConnections_);
    getSettings()->showEditStreamInfoButtonInSplitHeader.connect(
        [this](bool, auto) {
            this->updateIcons();
        },
        this->managedConnections_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            if (auto *twitchChannel =
                    dynamic_cast<TwitchChannel *>(
                        this->split_->getSelectedChannel().get());
                twitchChannel != nullptr && !twitchChannel->isEmpty() &&
                getSettings()->showFollowButtonInSplitHeader &&
                canUseFollowButtonForChannel(*twitchChannel))
            {
                twitchChannel->refreshFollowingStatus(true);
            }
            this->updateIcons();
        },
        this->managedConnections_);

    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        // Hack: In some cases Qt doesn't send the leaveEvent the "actual" last mouse receiver.
        // This can happen when quickly moving the mouse out of the window and right clicking.
        // To prevent the tooltip from getting stuck, we use the window's leaveEvent.
        this->managedConnections_.managedConnect(window->leaving, [this] {
            this->hideTooltip();
        });
    }

    this->scaleChangedEvent(this->scale());
}

SplitHeader::~SplitHeader()
{
    delete this->tooltipWidget_;
}

TooltipWidget *SplitHeader::ensureTooltipWidget()
{
    if (this->tooltipWidget_ == nullptr)
    {
        this->tooltipWidget_ = new TooltipWidget(this);

        this->tooltipWidget_->setOverrideScale(this->scale());
    }
    return this->tooltipWidget_;
}

void SplitHeader::hideTooltip()
{
    if (this->tooltipWidget_ != nullptr)
    {
        this->tooltipWidget_->hide();
    }
}

void SplitHeader::releaseTooltip()
{
    delete std::exchange(this->tooltipWidget_, nullptr);
}

void SplitHeader::initializeLayout()
{
    assert(this->layout() == nullptr);

    this->manageChannelButton_ = new SvgButton(
        {
            .dark = ":/buttons/editStreamInfo-darkMode.svg",
            .light = ":/buttons/editStreamInfo-lightMode.svg",
        },
        this, {6, 6});
    this->manageChannelButton_->setToolTip("Edit stream info");
    this->manageChannelButton_->setAccessibleName("Edit stream info");

    this->moderationButton_ = new SvgButton(
        {
            .dark = ":/buttons/moderationDisabled-darkMode.svg",
            .light = ":/buttons/moderationDisabled-lightMode.svg",
        },
        this, {5, 5});

    this->recordingButton_ = new SvgButton(
        {.dark = ":/buttons/record.svg", .light = ":/buttons/record.svg"}, this,
        {5, 5});
    this->recordingButton_->setObjectName("tabRecordingButton");
    this->recordingButton_->setAccessibleName("Record tab");
    this->recordingButton_->hide();

    this->chattersButton_ = new SvgButton(
        {
            .dark = ":/buttons/chatters-darkMode.svg",
            .light = ":/buttons/chatters-lightMode.svg",
        },
        this, {4, 4});

    this->followButton_ =
        new SvgButton(followButtonSource(false), this, {4, 4});

    this->addButton_ = new DrawnButton(DrawnButton::Symbol::Plus,
                                       {
                                           .padding = 3,
                                           .thickness = 1,
                                       },
                                       this);

    this->dropdownButton_ =
        new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);

    /// XXX: this never gets disconnected
    QObject::connect(this->dropdownButton_, &Button::leftMousePress, this,
                     [this] {
                         this->dropdownButton_->setMenu(this->createMainMenu());
                     });

    auto *layout = makeLayout<QHBoxLayout>({
        // follow
        this->followButton_,
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // title
        this->titleLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::MinimumExpanding,
                             QSizePolicy::Preferred);
            w->setCentered(true);
            w->setPadding(QMargins{});
        }),
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // mode
        this->modeButton_ = makeWidget<LabelButton>([&](auto w) {
            w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            w->hide();
        }),

        this->sharedChatButton_ = makeWidget<LabelButton>([&](auto w) {
            w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            w->setPadding({5, 0});
            w->setToolTip("Choose which Shared Chat channels to show");
            w->setAccessibleName("Shared Chat channels");
            w->hide();
        }),

        this->manageChannelButton_,
        // moderator
        this->moderationButton_,
        this->recordingButton_,
        // chatter list
        this->chattersButton_,
        // dropdown
        this->dropdownButton_,
        // add split
        this->addButton_,
    });

    QObject::connect(this->modeButton_, &Button::leftMousePress, this, [this] {
        if (this->modeButton_->menu() == nullptr)
        {
            this->modeButton_->setMenu(this->createChatModeMenu());
            this->updateRoomModes();
        }
    });

    QObject::connect(
        this->manageChannelButton_, &Button::leftClicked, this, [this] {
            auto channel = std::dynamic_pointer_cast<TwitchChannel>(
                this->split_->getSelectedChannel());
            if (!channel || channel->isEmpty())
            {
                return;
            }

            this->manageChannelButton_->setEnabled(false);
            const QPointer<SplitHeader> self(this);
            ChannelManagement::verifyAccess(
                channel, true,
                [self, channel](ChannelManagementAccess) {
                    if (!self)
                    {
                        return;
                    }
                    self->manageChannelButton_->setEnabled(true);
                    const auto selected =
                        std::dynamic_pointer_cast<TwitchChannel>(
                            self->split_->getSelectedChannel());
                    if (selected != channel)
                    {
                        self->updateIcons();
                        return;
                    }
                    ChannelManagementDialog::showForChannel(channel,
                                                            self->split_);
                },
                [self, channel](const QString &error) {
                    channel->addSystemMessage(error);
                    if (!self)
                    {
                        return;
                    }
                    self->manageChannelButton_->setEnabled(true);
                    self->updateIcons();
                });
        });

    QObject::connect(
        this->moderationButton_, &Button::clicked, this,
        [this](Qt::MouseButton button) mutable {
            switch (button)
            {
                case Qt::LeftButton:
                    if (getSettings()->moderationActions.empty())
                    {
                        getApp()->getWindows()->showSettingsDialog(
                            this, SettingsDialogPreference::ModerationActions);
                        this->split_->setModerationMode(true);
                    }
                    else
                    {
                        auto moderationMode = this->split_->getModerationMode();

                        this->split_->setModerationMode(!moderationMode);
                        // w->setDim(moderationMode ? DimButton::Dim::Some
                        //                          : DimButton::Dim::None);
                    }
                    break;

                case Qt::RightButton:
                case Qt::MiddleButton:
                    getApp()->getWindows()->showSettingsDialog(
                        this, SettingsDialogPreference::ModerationActions);
                    break;

                default:
                    break;
            }
        });

    QObject::connect(this->chattersButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->openChatterList();
                     });

    QObject::connect(this->followButton_, &Button::leftClicked, this,
                     [this]() {
                         this->toggleFollow();
                     });

    QObject::connect(
        this->sharedChatButton_, &Button::leftMousePress, this, [this] {
            this->sharedChatButton_->setMenu(this->createSharedChatMenu());
        });

    QObject::connect(this->addButton_, &Button::leftClicked, this, [this]() {
        this->split_->addSibling();
    });

    getSettings()->customURIScheme.connect(
        [this] {
            if (auto *const drop = this->dropdownButton_)
            {
                drop->setMenu(this->createMainMenu());
            }
        },
        this->managedConnections_);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    this->autoModHeaderBalance_ =
        new QSpacerItem(0, 0, QSizePolicy::Preferred, QSizePolicy::Minimum);
    layout->insertSpacerItem(layout->indexOf(this->titleLabel_),
                             this->autoModHeaderBalance_);
    this->setLayout(layout);

    this->setAddButtonVisible(false);
}

std::unique_ptr<QMenu> SplitHeader::createMainMenu()
{
    // top level menu
    const auto &h = getApp()->getHotkeys();
    auto menu = std::make_unique<QMenu>();
    menu->setToolTipsVisible(true);
    if (auto *recordings = getApp()->getChatRecordings())
    {
        auto *tab = recordingTabFor(this->split_);
        const auto active = recordings->isActive(tab);
        auto *action = menu->addAction(
            active ? "Stop tab recording" : "Record tab", this,
            [this, recordings] {
                recordings->toggle(recordingTabFor(this->split_));
            });
        action->setEnabled(active ? recordings->status(tab) != "Saving"
                                  : recordings->canStart(tab));
        menu->addSeparator();
    }
    menu->addAction(
        "Change channel",
        h->getDisplaySequence(HotkeyCategory::Split, "changeChannel"),
        this->split_, &Split::changeChannel);
    menu->addAction("Close",
                    h->getDisplaySequence(HotkeyCategory::Split, "delete"),
                    this->split_, &Split::deleteFromContainer);
    menu->addSeparator();
    menu->addAction(
        "Popup",
        h->getDisplaySequence(HotkeyCategory::Window, "popup", {{"split"}}),
        this->split_, &Split::popup);
    menu->addAction(
        "Popup overlay",
        h->getDisplaySequence(HotkeyCategory::Split, "popupOverlay"),
        this->split_, &Split::showOverlayWindow);
    menu->addAction("Search",
                    h->getDisplaySequence(HotkeyCategory::Split, "showSearch"),
                    this->split_, [this] {
                        this->split_->showSearch(true);
                    });
    menu->addAction("Search all open tabs",
                    h->getDisplaySequence(HotkeyCategory::Split,
                                          "showGlobalSearch"),
                    this->split_, [this] {
                        this->split_->showSearch(false);
                    });
    menu->addAction("Set filters",
                    h->getDisplaySequence(HotkeyCategory::Split, "pickFilters"),
                    this->split_, &Split::setFiltersDialog);
    menu->addSeparator();

    auto selected = this->split_->getSelectedChannel();
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(selected.get());
    auto *kickChannel = dynamic_cast<KickChannel *>(selected.get());
    auto *youtubeChannel = dynamic_cast<YouTubeChannel *>(selected.get());

    if (twitchChannel || kickChannel || youtubeChannel ||
        selected->isTikTokChannel())
    {
        menu->addAction(
            OPEN_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openInBrowser);
        if (twitchChannel)
        {
            menu->addAction(OPEN_PLAYER_IN_BROWSER,
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openPlayerInBrowser"),
                            this->split_, &Split::openBrowserPlayer);
        }
        if (twitchChannel || kickChannel)
        {
            menu->addAction(OPEN_IN_STREAMLINK,
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openInStreamlink"),
                            this->split_, &Split::openInStreamlink);

            if (!getSettings()->customURIScheme.getValue().isEmpty())
            {
                menu->addAction("Open in custom player",
                                h->getDisplaySequence(HotkeyCategory::Split,
                                                      "openInCustomPlayer"),
                                this->split_, &Split::openWithCustomScheme);
            }

            if (this->split_->getChannel()->hasModRights())
            {
                menu->addAction(
                    OPEN_MOD_VIEW_IN_BROWSER,
                    h->getDisplaySequence(HotkeyCategory::Split, "openModView"),
                    this->split_, &Split::openModViewInBrowser);
            }
        }

        if (twitchChannel)
        {
            auto managedChannel =
                std::dynamic_pointer_cast<TwitchChannel>(selected);
            auto *manageAction =
                menu->addAction("Manage channel...", this->split_,
                                [managedChannel, parent = this->split_] {
                                    ChannelManagementDialog::showForChannel(
                                        managedChannel, parent);
                                });
            manageAction->setToolTip(
                "Edit stream information or run a commercial as the "
                "broadcaster or a verified channel editor.");

            menu->addAction(
                    "Create a clip",
                    h->getDisplaySequence(HotkeyCategory::Split, "createClip"),
                    this->split_,
                    [twitchChannel] {
                        twitchChannel->createClip({}, {});
                    })
                ->setVisible(twitchChannel->isLive());
        }

        if (this->split_->getIndirectChannel().getType() ==
            Channel::Type::TwitchWatching)
        {
            menu->addAction("Reset /watching", this->split_, [] {
                if (!getApp()
                         ->getTwitch()
                         ->getWatchingChannel()
                         .get()
                         ->isEmpty())
                {
                    getApp()->getTwitch()->setWatchingChannel(
                        Channel::getEmpty());
                }
            });
        }

        menu->addSeparator();
    }

    if (this->split_->getSelectedChannel()->getType() ==
        Channel::Type::TwitchWhispers)
    {
        menu->addAction(
            OPEN_WHISPERS_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openWhispersInBrowser);
        menu->addSeparator();
    }

    // reload / reconnect
    if (this->split_->getChannel()->canReconnect())
    {
        menu->addAction(
            "Reconnect",
            h->getDisplaySequence(HotkeyCategory::Split, "reconnect"), this,
            &SplitHeader::reconnect);
    }

    if (twitchChannel || kickChannel)
    {
        auto bothSeq = h->getDisplaySequence(
            HotkeyCategory::Split, "reloadEmotes", {std::vector<QString>()});
        auto channelSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                "reloadEmotes", {{"channel"}});
        auto subSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "reloadEmotes", {{"subscriber"}});
        menu->addAction("Reload channel emotes",
                        channelSeq.isEmpty() ? bothSeq : channelSeq, this,
                        &SplitHeader::reloadChannelEmotes);
        if (twitchChannel)
        {
            menu->addAction("Reload subscriber emotes",
                            subSeq.isEmpty() ? bothSeq : subSeq, this,
                            &SplitHeader::reloadSubscriberEmotes);
        }
    }

    if ((twitchChannel || kickChannel || youtubeChannel ||
         selected->isTikTokChannel()) &&
        !selected->isEmpty())
    {
        menu->addSeparator();

        const auto selectedChannelName =
            this->split_->getSelectedChannel()->getName();
        auto *autoTranslateAction = new QAction(menu.get());
        autoTranslateAction->setText("Auto-translate messages (risky)");
        autoTranslateAction->setCheckable(true);
        autoTranslateAction->setToolTip(
            "Translates new visible chat messages in this channel as they "
            "arrive. Risky in fast chats because translation requests can hit "
            "rate limits, fail, or skip messages.");
        autoTranslateAction->setStatusTip(autoTranslateAction->toolTip());

        QObject::connect(menu.get(), &QMenu::aboutToShow, this,
                         [autoTranslateAction, selectedChannelName]() {
                             autoTranslateAction->setChecked(
                                getSettings()->isAutoTranslateChannel(
                                    selectedChannelName));
                         });
        QObject::connect(autoTranslateAction, &QAction::triggered, this,
                         [autoTranslateAction, selectedChannelName]() {
                             autoTranslateAction->setChecked(
                                getSettings()->toggleAutoTranslateChannel(
                                    selectedChannelName));
                         });

        menu->addAction(autoTranslateAction);
    }

    if (auto *automations = getApp()->getChatAutomations(); automations)
    {
        QString initialChannel;
        if (twitchChannel)
        {
            initialChannel = twitchChannel->getName();
        }
        else if (auto multi = std::dynamic_pointer_cast<MultiChannel>(
                     this->split_->getChannel()))
        {
            for (const auto &child : multi->channels())
            {
                if (child.platform == MultiChannel::Platform::Twitch)
                {
                    initialChannel = child.channel->getName();
                    break;
                }
            }
        }

        if (!initialChannel.isEmpty())
        {
            menu->addSeparator();
            menu->addAction("Chat automations (self bot)...", this->split_,
                            [initialChannel] {
                                ChatAutomationDialog::showDialog(
                                    initialChannel,
                                    &getApp()->getWindows()->getMainWindow());
                            });
        }
    }

    menu->addSeparator();

    {
        // "How to..." sub menu
        auto *subMenu = new QMenu("How to...", menu.get());
        subMenu->addAction("move split", this->split_, &Split::explainMoving);
        subMenu->addAction("add/split", this->split_, &Split::explainSplitting);
        menu->addMenu(subMenu);
    }

    menu->addSeparator();

    // sub menu
    auto *moreMenu = new QMenu("More", menu.get());

    auto modModeSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "setModerationMode", {{"toggle"}});
    if (modModeSeq.isEmpty())
    {
        modModeSeq =
            h->getDisplaySequence(HotkeyCategory::Split, "setModerationMode",
                                  {std::vector<QString>()});
        // this makes a full std::optional<> with an empty vector inside
    }
    moreMenu->addAction(
        "Toggle moderation mode", modModeSeq, this->split_, [this]() {
            this->split_->setModerationMode(!this->split_->getModerationMode());
        });

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchMentions)
    {
        auto *action = new QAction(moreMenu);
        action->setText("Enable /mention tab highlights");
        action->setCheckable(true);

        QObject::connect(moreMenu, &QMenu::aboutToShow, this, [action]() {
            action->setChecked(getSettings()->highlightMentions);
        });
        QObject::connect(action, &QAction::triggered, this, []() {
            getSettings()->highlightMentions =
                !getSettings()->highlightMentions;
        });

        moreMenu->addAction(action);
    }

    if (canShowChatterList(this->split_->getChannel()))
    {
        moreMenu->addAction(
            "Show chatter list",
            h->getDisplaySequence(HotkeyCategory::Split, "openViewerList"),
            this->split_, &Split::openChatterList);
    }

    if (twitchChannel)
    {
        moreMenu->addAction("Subscribe",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openSubscriptionPage"),
                            this->split_, &Split::openSubPage);
    }

    if (twitchChannel || youtubeChannel)
    {
        const auto platform =
            youtubeChannel ? Platform::YouTube : Platform::Twitch;
        {
            auto *action = new QAction(moreMenu);
            action->setText("Notify when live");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setChannelNotification", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setChannelNotification",
                                                  {std::vector<QString>()});
                // this makes a full std::optional<> with an empty vector inside
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this,
                [action, selected, platform, youtubeChannel]() {
                    action->setChecked(
                        getApp()->getNotifications()->isChannelNotified(
                            selected->getName(), platform,
                            youtubeChannel ? youtubeChannel->channelID()
                                           : QString{}));
                });
            QObject::connect(
                action, &QAction::triggered, this,
                [selected, platform, youtubeChannel]() {
                    getApp()->getNotifications()->updateChannelNotification(
                        selected->getName(), platform,
                        youtubeChannel ? youtubeChannel->channelID()
                                       : QString{});
                });

            moreMenu->addAction(action);
        }
    }

    if (twitchChannel)
    {
        {
            auto *action = new QAction(moreMenu);
            action->setText("Mute highlight sounds");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setHighlightSounds", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setHighlightSounds",
                                                  {std::vector<QString>()});
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(getSettings()->isMutedChannel(
                        this->split_->getSelectedChannel()->getName()));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getSettings()->toggleMutedChannel(
                    this->split_->getSelectedChannel()->getName());
            });

            moreMenu->addAction(action);
        }
    }

    moreMenu->addSeparator();
    moreMenu->addAction(
        "Clear messages",
        h->getDisplaySequence(HotkeyCategory::Split, "clearMessages"),
        this->split_, &Split::clear);
    //    moreMenu->addSeparator();
    //    moreMenu->addAction("Show changelog", this,
    //    SLOT(moreMenuShowChangelog()));
    menu->addMenu(moreMenu);

    return menu;
}

std::unique_ptr<QMenu> SplitHeader::createChatModeMenu()
{
    auto menu = std::make_unique<QMenu>();

    this->modeActionSetSub = new QAction("Subscriber only", this);
    this->modeActionSetEmote = new QAction("Emote only", this);
    this->modeActionSetSlow = new QAction("Slow", this);
    this->modeActionSetR9k = new QAction("R9K", this);
    this->modeActionSetFollowers = new QAction("Followers only", this);

    this->modeActionSetFollowers->setCheckable(true);
    this->modeActionSetSub->setCheckable(true);
    this->modeActionSetEmote->setCheckable(true);
    this->modeActionSetSlow->setCheckable(true);
    this->modeActionSetR9k->setCheckable(true);

    menu->addAction(this->modeActionSetEmote);
    menu->addAction(this->modeActionSetSub);
    menu->addAction(this->modeActionSetSlow);
    menu->addAction(this->modeActionSetR9k);
    menu->addAction(this->modeActionSetFollowers);

    auto execCommand = [this](const QString &command) {
        auto channel = this->split_->getSelectedChannel();
        auto text = getApp()->getCommands()->execCommand(
            command, channel, false);
        channel->sendMessage(text);
    };
    auto toggle = [execCommand](const QString &command,
                                QAction *action) mutable {
        execCommand(command + (action->isChecked() ? "" : "off"));
        action->setChecked(!action->isChecked());
    };

    QObject::connect(this->modeActionSetSub, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/subscribers", this->modeActionSetSub);
                     });

    QObject::connect(this->modeActionSetEmote, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/emoteonly", this->modeActionSetEmote);
                     });

    QObject::connect(this->modeActionSetSlow, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetSlow->isChecked())
                         {
                             execCommand("/slowoff");
                             this->modeActionSetSlow->setChecked(false);
                             return;
                         };
                         const auto channel =
                             this->split_->getSelectedChannel();
                         const QPointer<SplitHeader> self(this);
                         auto ok = bool();
                         auto seconds = QInputDialog::getInt(
                             this, "", "Seconds:", 10, 0, 500, 1, &ok,
                             Qt::FramelessWindowHint);
                         if (!self)
                         {
                             return;
                         }
                         if (channel != this->split_->getSelectedChannel())
                         {
                             this->updateRoomModes();
                             return;
                         }
                         if (ok)
                         {
                             execCommand(QString("/slow %1").arg(seconds));
                         }
                         else
                         {
                             this->modeActionSetSlow->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetFollowers, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetFollowers->isChecked())
                         {
                             execCommand("/followersoff");
                             this->modeActionSetFollowers->setChecked(false);
                             return;
                         };
                         const auto channel =
                             this->split_->getSelectedChannel();
                         const QPointer<SplitHeader> self(this);
                         auto ok = bool();
                         auto time = QInputDialog::getText(
                             this, "", "Time:", QLineEdit::Normal, "15m", &ok,
                             Qt::FramelessWindowHint,
                             Qt::ImhLowercaseOnly | Qt::ImhPreferNumbers);
                         if (!self)
                         {
                             return;
                         }
                         if (channel != this->split_->getSelectedChannel())
                         {
                             this->updateRoomModes();
                             return;
                         }
                         if (ok)
                         {
                             execCommand(QString("/followers %1").arg(time));
                         }
                         else
                         {
                             this->modeActionSetFollowers->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetR9k, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/r9kbeta", this->modeActionSetR9k);
                     });

    return menu;
}

void SplitHeader::updateRoomModes()
{
    assert(this->modeButton_ != nullptr);
    auto chan = this->split_->getSelectedChannel();

    // Update the mode button
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get()))
    {
        this->modeButton_->setEnabled(twitchChannel->hasModRights());

        QString text;
        {
            auto roomModes = twitchChannel->accessRoomModes();
            text = formatRoomModeUnclean(*roomModes);

            if (this->modeActionSetR9k != nullptr)
            {
                // Set menu action
                this->modeActionSetR9k->setChecked(roomModes->r9k);
                this->modeActionSetSlow->setChecked(roomModes->slowMode > 0);
                this->modeActionSetEmote->setChecked(roomModes->emoteOnly);
                this->modeActionSetSub->setChecked(roomModes->submode);
                this->modeActionSetFollowers->setChecked(
                    roomModes->followerOnly != -1);
            }
        }
        cleanRoomModeText(text, twitchChannel->hasModRights());

        // set the label text

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }

        // Update the mode button menu actions
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(chan.get()))
    {
        this->modeButton_->setEnabled(false);

        QString text = formatRoomModeUnclean(kc->roomModes());
        cleanRoomModeText(text, false);

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }
    }
    else
    {
        this->modeButton_->hide();
    }
}

void SplitHeader::resetThumbnail()
{
    this->lastThumbnail_.invalidate();
    this->thumbnail_.clear();
    this->thumbnailSource_.clear();
}

void SplitHeader::handleChannelChanged()
{
    this->resetThumbnail();

    this->updateChannelText();
    this->updateAutoModLayout();

    this->channelConnections_.clear();
    this->resetSharedChatFilter();
    auto connectSelectedChannel = [this] {
        auto selected = this->split_->getSelectedChannel();
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(selected.get()))
        {
            this->channelConnections_.managedConnect(
                twitchChannel->streamStatusChanged, [this]() {
                    this->updateChannelText();
                });
            this->channelConnections_.managedConnect(
                twitchChannel->followingStatusChanged, [this]() {
                    this->updateIcons();
                });
            this->channelConnections_.managedConnect(
                twitchChannel->roomIdChanged, [this]() {
                    this->updateIcons();
                });
            if (getSettings()->showFollowButtonInSplitHeader &&
                canUseFollowButtonForChannel(*twitchChannel))
            {
                twitchChannel->refreshFollowingStatus(false);
            }
        }
        else if (auto *kickChannel =
                     dynamic_cast<KickChannel *>(selected.get()))
        {
            this->channelConnections_.managedConnect(
                kickChannel->streamDataChanged, [this]() {
                    this->updateChannelText();
                });
        }
        else if (auto *youtubeChannel =
                     dynamic_cast<YouTubeChannel *>(selected.get()))
        {
            this->channelConnections_.managedConnect(
                youtubeChannel->streamDataChanged, [this] {
                    this->updateChannelText();
                });
            this->channelConnections_.managedConnect(
                youtubeChannel->liveStatusChanged, [this] {
                    this->updateChannelText();
                });
        }
        else if (auto *tiktokChannel =
                     dynamic_cast<TikTokChannel *>(selected.get()))
        {
            this->channelConnections_.managedConnect(
                tiktokChannel->streamDataChanged, [this] {
                    this->updateChannelText();
                });
            this->channelConnections_.managedConnect(
                tiktokChannel->liveStatusChanged, [this] {
                    this->updateChannelText();
                });
        }
    };

    auto channel = this->split_->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        connectSelectedChannel();

        const auto selectedChannel = this->split_->getSelectedChannel();
        for (const auto &child : multiChannel->channels())
        {
            if (child.channel == selectedChannel)
            {
                continue;
            }
            if (auto *twitch =
                    dynamic_cast<TwitchChannel *>(child.channel.get()))
            {
                this->channelConnections_.managedConnect(
                    twitch->userStateChanged, [this] {
                        this->split_->setModerationMode(
                            this->split_->getModerationMode());
                    });
            }
            else if (auto *kick =
                         dynamic_cast<KickChannel *>(child.channel.get()))
            {
                this->channelConnections_.managedConnect(
                    kick->userStateChanged, [this] {
                        this->split_->setModerationMode(
                            this->split_->getModerationMode());
                    });
            }
            else if (auto *youtube =
                         dynamic_cast<YouTubeChannel *>(child.channel.get()))
            {
                this->channelConnections_.managedConnect(
                    youtube->userStateChanged, [this] {
                        this->split_->setModerationMode(
                            this->split_->getModerationMode());
                    });
            }
        }
        this->channelConnections_.managedConnect(
            multiChannel->activeChannelChanged, [this] {
                this->handleChannelChanged();
                this->updateRoomModes();
            });
    }
    else
    {
        connectSelectedChannel();
    }

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            twitchChannel->messageAppended,
            [this](const MessagePtr &message, const auto &) {
                this->recordSharedChatSource(message);
            });

        for (const auto &message : twitchChannel->getMessageSnapshot())
        {
            this->recordSharedChatSource(message);
        }

        this->applySharedChatFilter();
    }
    else if (channel->getType() == Channel::Type::TwitchAutomod)
    {
        this->sharedChatFilterActive_ = false;
    }
    else if (this->sharedChatFilterActive_)
    {
        this->split_->getChannelView().setMessagePredicate({});
        this->sharedChatFilterActive_ = false;
    }

    this->updateSharedChatButton();

    this->updateIcons();
}

void SplitHeader::scaleChangedEvent(float scale)
{
    if (this->tooltipWidget_ != nullptr)
    {
        this->tooltipWidget_->setOverrideScale(scale);
    }

    int w = int(BUTTON_WIDTH * scale);
    int addSplitWidth = int(ADD_SPLIT_BUTTON_WIDTH * scale);

    this->setFixedHeight(w);
    this->dropdownButton_->setFixedWidth(w);
    this->followButton_->setFixedWidth(w);
    this->manageChannelButton_->setFixedWidth(w);
    this->moderationButton_->setFixedWidth(w);
    this->recordingButton_->setFixedWidth(w);
    this->chattersButton_->setFixedWidth(w);
    this->sharedChatButton_->setMaximumWidth(int(104 * scale));

    this->addButton_->setFixedWidth(addSplitWidth);
    this->updateAutoModLayout();
}

void SplitHeader::resetSharedChatFilter()
{
    this->sharedChatSources_.clear();
    this->hiddenSharedChatSources_.clear();

    auto channel = this->split_->getChannel();
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitchChannel == nullptr || twitchChannel->isEmpty() ||
        twitchChannel->roomId().isEmpty())
    {
        this->sharedChatButton_->hide();
        return;
    }

    const auto user =
        getApp()->getTwitchUsers()->resolveID({twitchChannel->roomId()});
    this->sharedChatSources_.insert(twitchChannel->roomId(), user);
}

void SplitHeader::recordSharedChatSource(const MessagePtr &message)
{
    if (!message || message->sharedChatSourceId.isEmpty())
    {
        return;
    }

    const auto &sourceId = message->sharedChatSourceId;
    if (this->sharedChatSources_.contains(sourceId))
    {
        return;
    }

    const auto source = getApp()->getTwitchUsers()->resolveID({sourceId});
    this->sharedChatSources_.insert(sourceId, source);
    this->updateSharedChatButton();
}

void SplitHeader::applySharedChatFilter()
{
    if (this->hiddenSharedChatSources_.isEmpty())
    {
        if (this->sharedChatFilterActive_)
        {
            this->split_->getChannelView().setMessagePredicate({});
            this->sharedChatFilterActive_ = false;
        }
        return;
    }

    const QPointer<SplitHeader> self(this);
    this->split_->getChannelView().setMessagePredicate(
        [self](const MessagePtr &message) {
            if (!self || !message || message->sharedChatSourceId.isEmpty())
            {
                return true;
            }
            return !self->hiddenSharedChatSources_.contains(
                message->sharedChatSourceId);
        });
    this->sharedChatFilterActive_ = true;
}

void SplitHeader::updateSharedChatButton()
{
    if (!getSettings()->showSharedChatChannelSelector ||
        this->sharedChatSources_.size() < 2)
    {
        this->sharedChatButton_->hide();
        return;
    }

    const auto visible =
        this->sharedChatSources_.size() - this->hiddenSharedChatSources_.size();
    QString text;
    if (this->hiddenSharedChatSources_.isEmpty())
    {
        text = QStringLiteral("All chats");
    }
    else if (visible == 1)
    {
        for (auto it = this->sharedChatSources_.cbegin();
             it != this->sharedChatSources_.cend(); ++it)
        {
            if (this->hiddenSharedChatSources_.contains(it.key()))
            {
                continue;
            }
            const auto &user = it.value();
            text = QStringLiteral("One chat");
            if (user && !user->displayName.trimmed().isEmpty())
            {
                text = user->displayName.trimmed();
            }
            else if (user && !user->name.trimmed().isEmpty())
            {
                text = user->name.trimmed();
            }
            break;
        }
    }
    else
    {
        text = QStringLiteral("%1 chats").arg(visible);
    }

    const auto maxTextWidth = std::max(24, int(78 * this->scale()));
    text = QFontMetrics(this->font())
               .elidedText(text, Qt::ElideRight, maxTextWidth);
    this->sharedChatButton_->setText(text + QStringLiteral(" ▾"));
    this->sharedChatButton_->show();
}

std::unique_ptr<QMenu> SplitHeader::createSharedChatMenu()
{
    auto menu = std::make_unique<QMenu>();
    menu->setToolTipsVisible(true);
    const auto weakChannel = std::weak_ptr<Channel>(this->split_->getChannel());

    auto *showAll = menu->addAction("Show all channels");
    showAll->setCheckable(true);
    showAll->setChecked(this->hiddenSharedChatSources_.isEmpty());
    showAll->setEnabled(!this->hiddenSharedChatSources_.isEmpty());
    QObject::connect(showAll, &QAction::triggered, this, [this, weakChannel] {
        if (weakChannel.lock() != this->split_->getChannel())
        {
            return;
        }
        this->hiddenSharedChatSources_.clear();
        this->applySharedChatFilter();
        this->updateSharedChatButton();
    });
    menu->addSeparator();

    struct SourceRow {
        QString id;
        QString label;
        std::shared_ptr<TwitchUser> user;
        bool home = false;
    };
    std::vector<SourceRow> rows;
    rows.reserve(this->sharedChatSources_.size());

    QString homeId;
    if (auto *home =
            dynamic_cast<TwitchChannel *>(this->split_->getChannel().get()))
    {
        homeId = home->roomId();
    }

    for (auto it = this->sharedChatSources_.cbegin();
         it != this->sharedChatSources_.cend(); ++it)
    {
        const auto &user = it.value();
        QString label;
        if (user && !user->displayName.trimmed().isEmpty())
        {
            label = user->displayName.trimmed();
        }
        else if (user && !user->name.trimmed().isEmpty())
        {
            label = user->name.trimmed();
        }
        else
        {
            label = QStringLiteral("Channel %1").arg(it.key().right(6));
        }
        rows.push_back({it.key(), label, user, it.key() == homeId});
    }
    std::ranges::sort(rows, [](const SourceRow &a, const SourceRow &b) {
        if (a.home != b.home)
        {
            return a.home;
        }
        return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });

    const auto visibleCount =
        this->sharedChatSources_.size() - this->hiddenSharedChatSources_.size();
    for (const auto &row : rows)
    {
        auto *action = menu->addAction(row.label);
        action->setCheckable(true);
        const bool checked = !this->hiddenSharedChatSources_.contains(row.id);
        action->setChecked(checked);
        action->setEnabled(!checked || visibleCount > 1);
        action->setToolTip(
            row.home ? QStringLiteral("This channel")
                     : QStringLiteral("Shared from %1")
                           .arg(row.user ? row.user->name : row.label));

        if (row.user && !row.user->profilePictureUrl.isEmpty())
        {
            const auto image =
                Image::fromUrl(Url{row.user->profilePictureUrl}, 1, {32, 32});
            if (const auto pixmap = image->pixmapOrLoad())
            {
                action->setIcon(QIcon(*pixmap));
            }
        }

        QObject::connect(
            action, &QAction::triggered, this,
            [this, weakChannel, id = row.id](bool show) {
                if (weakChannel.lock() != this->split_->getChannel() ||
                    !this->sharedChatSources_.contains(id))
                {
                    return;
                }
                if (show)
                {
                    this->hiddenSharedChatSources_.remove(id);
                }
                else if (this->hiddenSharedChatSources_.size() + 1 <
                         this->sharedChatSources_.size())
                {
                    this->hiddenSharedChatSources_.insert(id);
                }
                this->applySharedChatFilter();
                this->updateSharedChatButton();
            });
    }
    return menu;
}

void SplitHeader::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateAutoModLayout();
}

void SplitHeader::setAutoModReviewBar(AutoModReviewBar *bar)
{
    assert(this->autoModReviewBar_ == nullptr);
    assert(bar != nullptr);

    this->autoModReviewBar_ = bar;
    auto *layout = qobject_cast<QHBoxLayout *>(this->layout());
    assert(layout != nullptr);
    layout->insertWidget(layout->indexOf(this->dropdownButton_), bar);
    bar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    this->updateAutoModLayout();
}

void SplitHeader::updateAutoModLayout()
{
    const bool isAutoMod = this->split_->getIndirectChannel().getType() ==
                           Channel::Type::TwitchAutomod;

    if (this->autoModReviewBar_ != nullptr)
    {
        this->autoModReviewBar_->setVisible(isAutoMod);
    }
    int balanceWidth = 0;
    if (isAutoMod && this->autoModReviewBar_ != nullptr)
    {
        auto mirroredWidth = this->autoModReviewBar_->sizeHint().width() +
                             this->dropdownButton_->width();
        if (!this->addButton_->isHidden())
        {
            mirroredWidth += this->addButton_->width();
        }
        const auto titleMetrics = getApp()->getFonts()->getFontMetrics(
            this->titleLabel_->getFontStyle(), this->scale());
        const auto titleWidth = static_cast<int>(std::ceil(
            titleMetrics.horizontalAdvance(this->titleLabel_->getText())));
        const bool hasCenteringRoom =
            this->width() >= mirroredWidth * 2 + titleWidth +
                                 static_cast<int>(16 * this->scale());
        if (hasCenteringRoom)
        {
            balanceWidth = mirroredWidth;
        }
    }
    if (this->autoModHeaderBalance_->sizeHint().width() != balanceWidth)
    {
        this->autoModHeaderBalance_->changeSize(
            balanceWidth, 0, QSizePolicy::Preferred, QSizePolicy::Minimum);
        this->layout()->invalidate();
    }
    this->titleLabel_->setShouldElide(isAutoMod);
    this->titleLabel_->setCentered(true);
    this->titleLabel_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                     QSizePolicy::Preferred);
}

void SplitHeader::hideEvent(QHideEvent *event)
{
    this->releaseTooltip();
    BaseWidget::hideEvent(event);
}

void SplitHeader::setAddButtonVisible(bool value)
{
    this->addButton_->setVisible(value);
    this->updateAutoModLayout();
}

void SplitHeader::toggleFollow()
{
    auto channel = this->split_->getSelectedChannel();
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitchChannel == nullptr || twitchChannel->isEmpty())
    {
        return;
    }

    const auto command =
        twitchChannel->isFollowingStatusKnown() && twitchChannel->isFollowing()
            ? QStringLiteral("/unfollow")
            : QStringLiteral("/follow");

    if (command == QStringLiteral("/unfollow") &&
        getSettings()->confirmUnfollowFromSplitHeader)
    {
        const auto displayName = channel->getLocalizedName().isEmpty()
                                     ? channel->getName()
                                     : channel->getLocalizedName();

        QPointer<QMessageBox> box = new QMessageBox(this);
        const auto cleanup = qScopeGuard([box] {
            delete box;
        });
        box->setWindowTitle("Unfollow channel?");
        box->setIcon(QMessageBox::Question);
        box->setText(
            QString("Are you sure you want to unfollow %1?").arg(displayName));

        auto *confirmButton =
            box->addButton("Confirm", QMessageBox::DestructiveRole);
        auto *cancelButton = box->addButton("Cancel", QMessageBox::RejectRole);
        box->setDefaultButton(cancelButton);
        box->setEscapeButton(cancelButton);
        box->exec();

        if (!box || box->clickedButton() != confirmButton)
        {
            return;
        }
    }

    CommandContext ctx{
        .words = {command},
        .rawText = command,
        .channel = channel,
        .twitchChannel = twitchChannel,
        .kickChannel = nullptr,
        .youtubeChannel = nullptr,
    };
    const auto text = command == QStringLiteral("/unfollow")
                          ? commands::unfollow(ctx)
                          : commands::follow(ctx);
    if (!text.isEmpty())
    {
        channel->sendMessage(text);
    }
}

void SplitHeader::updateChannelText()
{
    auto indirectChannel = this->split_->getIndirectChannel();
    this->isLive_ = false;
    this->tooltipText_ = QString();

    auto selectedChannel = this->split_->getSelectedChannel();

    auto title = selectedChannel->getLocalizedName();

    if (indirectChannel.getType() == Channel::Type::TwitchWatching)
    {
        title = "watching: " + (title.isEmpty() ? "none" : title);
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(selectedChannel.get()))
    {
        if (twitchChannel->isReadingAnonymously() && !title.isEmpty())
        {
            title += " (anonymous)";
        }

        const auto streamStatus = twitchChannel->accessStreamStatus();

        if (streamStatus->live)
        {
            this->isLive_ = true;
            // XXX: This URL format can be figured out from the Helix Get Streams API which we parse in TwitchChannel::parseLiveStatus
            QString url = "https://static-cdn.jtvnw.net/"
                          "previews-ttv/live_user_" +
                          selectedChannel->getName().toLower();
            switch (getSettings()->thumbnailSizeStream.getValue())
            {
                case 1:
                    url.append("-80x45.jpg");
                    break;
                case 2:
                    url.append("-160x90.jpg");
                    break;
                case 3:
                    url.append("-360x203.jpg");
                    break;
                default:
                    url = "";
            }
            if (this->thumbnailSource_ != url)
            {
                this->resetThumbnail();
                this->thumbnailSource_ = url;
            }
            if (!url.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(url, NetworkRequestType::Get)
                    .caller(this)
                    .timeout(10000)
                    .maximumResponseSize(5 * 1024 * 1024)
                    .onSuccess([this, url](auto result) {
                        assert(!isAppAboutToQuit());

                        if (this->thumbnailSource_ != url)
                        {
                            return;
                        }

                        // NOTE: We do not follow the redirects, so we need to make sure we only treat code 200 as a valid image
                        if (result.status() == 200)
                        {
                            this->thumbnail_ = QString::fromLatin1(
                                result.getData().toBase64());
                        }
                        else
                        {
                            this->thumbnail_.clear();
                        }
                        this->updateChannelText();
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ = formatTooltip(*streamStatus, this->thumbnail_);
            title += formatTitle(*streamStatus, *getSettings());
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(*streamStatus);
        }
    }
    else if (auto *kickChannel =
                 dynamic_cast<KickChannel *>(selectedChannel.get()))
    {
        const auto &stream = kickChannel->streamData();
        auto twitch = toTwitchStreamStatus(stream);
        if (stream.isLive)
        {
            this->isLive_ = true;
            if (this->thumbnailSource_ != stream.thumbnailUrl)
            {
                this->resetThumbnail();
                this->thumbnailSource_ = stream.thumbnailUrl;
            }
            if (!stream.thumbnailUrl.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(stream.thumbnailUrl, NetworkRequestType::Get)
                    .caller(this)
                    .timeout(10000)
                    .maximumResponseSize(5 * 1024 * 1024)
                    .followRedirects(true)
                    .onSuccess([this, source = stream.thumbnailUrl](
                                   const auto &result) {
                        assert(!isAppAboutToQuit());

                        if (this->thumbnailSource_ != source)
                        {
                            return;
                        }
                        this->thumbnail_ =
                            QString::fromLatin1(result.getData().toBase64());
                        this->updateChannelText();
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ = formatTooltip(twitch, this->thumbnail_, true);
            title += formatTitle(twitch, *getSettings());
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(twitch);
        }
    }
    else if (auto *youtubeChannel =
                 dynamic_cast<YouTubeChannel *>(selectedChannel.get()))
    {
        const auto streamTitle = youtubeChannel->streamTitle();
        if (youtubeChannel->isLive())
        {
            this->isLive_ = true;
            const auto videoID = youtubeChannel->videoID();
            const auto thumbnailUrl =
                videoID.isEmpty() || streamThumbnailHeight() == 0
                    ? QString{}
                    : QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg")
                          .arg(QString::fromUtf8(
                              QUrl::toPercentEncoding(videoID)));
            if (this->thumbnailSource_ != thumbnailUrl)
            {
                this->resetThumbnail();
                this->thumbnailSource_ = thumbnailUrl;
            }
            if (!thumbnailUrl.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(thumbnailUrl, NetworkRequestType::Get)
                    .caller(this)
                    .timeout(10000)
                    .maximumResponseSize(5 * 1024 * 1024)
                    .followRedirects(true)
                    .onSuccess([this, thumbnailUrl](const auto &result) {
                        assert(!isAppAboutToQuit());

                        if (this->thumbnailSource_ != thumbnailUrl)
                        {
                            return;
                        }
                        this->thumbnail_ =
                            QString::fromLatin1(result.getData().toBase64());
                        this->updateChannelText();
                    })
                    .onError([this, thumbnailUrl](const auto &) {
                        if (this->thumbnailSource_ == thumbnailUrl)
                        {
                            this->lastThumbnail_.invalidate();
                        }
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ =
                formatYouTubeTooltip(streamTitle, this->thumbnail_);
            title += QStringLiteral(" (live)");
            if (getSettings()->headerStreamTitle && !streamTitle.isEmpty())
            {
                title += u" - " % streamTitle.simplified();
            }
        }
        else
        {
            this->tooltipText_ =
                QStringLiteral(
                    "<p style=\"text-align: center;\">Offline<br>%1</p>")
                    .arg(streamTitle.toHtmlEscaped());
        }
    }

    else if (auto *tiktok =
                 dynamic_cast<TikTokChannel *>(selectedChannel.get()))
    {
        const auto &room = tiktok->room();
        this->isLive_ = room.live;
        this->tooltipText_ =
            QStringLiteral("<p style=\"text-align: center;\">%1<br>%2</p>")
                .arg(room.paused ? QStringLiteral("Paused on TikTok")
                     : room.live ? QStringLiteral("Live on TikTok")
                                 : QStringLiteral("Offline"),
                     room.title.toHtmlEscaped());
        if (room.live)
        {
            title += room.paused ? QStringLiteral(" (paused)")
                                 : QStringLiteral(" (live)");
            if (getSettings()->headerViewerCount && room.viewers >= 0)
            {
                title += QStringLiteral(" · %1 viewers")
                             .arg(QLocale().toString(room.viewers));
            }
            if (getSettings()->headerStreamTitle && !room.title.isEmpty())
            {
                title += QStringLiteral(" · ") + room.title.simplified();
            }
        }
    }

    if (!title.isEmpty() && !this->split_->getFilters().empty())
    {
        title += " - filtered";
    }

    this->titleLabel_->setText(title.isEmpty() ? "<empty>" : title);

    if (this->tooltipWidget_ != nullptr && this->tooltipWidget_->isVisible())
    {
        if (this->tooltipText_.isEmpty())
        {
            this->hideTooltip();
        }
        else
        {
            this->tooltipWidget_->setOne({nullptr, this->tooltipText_});
            this->tooltipWidget_->setWordWrap(true);
            this->tooltipWidget_->adjustSize();
            const auto pos =
                this->mapToGlobal(this->rect().bottomLeft()) +
                QPoint((this->width() - this->tooltipWidget_->width()) / 2, 1);
            this->tooltipWidget_->moveTo(
                pos, widgets::BoundsChecking::CursorPosition);
        }
    }
}

void SplitHeader::updateIcons()
{
    auto channel = this->split_->getSelectedChannel();

    if (channel->isTwitchOrKickChannel() || channel->isYouTubeChannel() ||
        channel->isTikTokChannel())
    {
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
            twitchChannel != nullptr && !twitchChannel->isEmpty())
        {
            this->manageChannelButton_->setVisible(
                getSettings()->showEditStreamInfoButtonInSplitHeader &&
                canShowChannelManagementButton(*twitchChannel));

            if (!getSettings()->showFollowButtonInSplitHeader ||
                !canUseFollowButtonForChannel(*twitchChannel))
            {
                this->followButton_->hide();
            }
            else
            {
                const auto following =
                    twitchChannel->isFollowingStatusKnown() &&
                    twitchChannel->isFollowing();
                const auto displayName = channel->getLocalizedName().isEmpty()
                                             ? channel->getName()
                                             : channel->getLocalizedName();
                this->followButton_->setSource(followButtonSource(following));
                this->followButton_->setToolTip(
                    following
                        ? QString("Unfollow %1")
                              .arg(displayName)
                        : QString("Follow %1")
                              .arg(displayName));
                this->followButton_->show();
            }
        }
        else
        {
            this->followButton_->hide();
            this->manageChannelButton_->hide();
        }

        auto moderationMode = this->split_->getModerationMode() &&
                              !getSettings()->moderationActions.empty();

        if (moderationMode)
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationEnabled-darkMode.svg",
                .light = ":/buttons/moderationEnabled-lightMode.svg",
            });
        }
        else
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationDisabled-darkMode.svg",
                .light = ":/buttons/moderationDisabled-lightMode.svg",
            });
        }

        const auto rootChannel = this->split_->getChannel();
        const bool isMulti =
            dynamic_cast<MultiChannel *>(rootChannel.get()) != nullptr;
        const bool modRights = hasAnyModRights(rootChannel);
        const bool showModerationButton =
            modRights ||
            (moderationMode &&
             (isMulti || (!channel->isYouTubeChannel() &&
                          !channel->isTikTokChannel())));
        if (showModerationButton)
        {
            this->moderationButton_->show();
        }
        else
        {
            this->moderationButton_->hide();
        }

        if (canShowChatterList(this->split_->getChannel()))
        {
            this->chattersButton_->show();
        }
        else
        {
            this->chattersButton_->hide();
        }
    }
    else
    {
        this->followButton_->hide();
        this->manageChannelButton_->hide();
        this->moderationButton_->hide();
        this->chattersButton_->hide();
    }
}

void SplitHeader::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    const bool polished =
        !themeUsesClassicSplitFrame(this->theme->customization.foundation);
    QColor background = this->theme->splits.header.background;
    QColor border = this->theme->splits.header.border;
    const bool focused = this->split_->hasFocus();

    if (focused && !polished)
    {
        background = this->theme->splits.header.focusedBackground;
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), background);
    if (polished)
    {
        return;
    }

    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 2);
    painter.fillRect(0, this->height() - 1, this->width(), 1, background);
}

void SplitHeader::mousePressEvent(QMouseEvent *event)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            this->split_->setFocus(Qt::MouseFocusReason);

            this->dragging_ = true;

            this->dragStart_ = event->pos();
        }
        break;

        case Qt::RightButton: {
            auto *menu = this->createMainMenu().release();
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(this->mapToGlobal(event->pos() + QPoint(0, 4)));
        }
        break;

        case Qt::MiddleButton: {
            this->split_->openInBrowser();
        }
        break;

        default: {
        }
        break;
    }

    this->doubleClicked_ = false;
}

void SplitHeader::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    this->dragging_ = false;
}

void SplitHeader::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        if (distance(this->dragStart_, event->pos()) > 15 * this->scale())
        {
            this->split_->drag();
            this->dragging_ = false;
        }
    }
}

void SplitHeader::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->split_->changeChannel();
    }
    this->doubleClicked_ = true;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void SplitHeader::enterEvent(QEnterEvent *event)
#else
void SplitHeader::enterEvent(QEvent *event)
#endif
{
    this->updateChannelText();

    if (!this->tooltipText_.isEmpty())
    {
        auto *tooltipWidget = this->ensureTooltipWidget();
        tooltipWidget->setOne({nullptr, this->tooltipText_});
        tooltipWidget->setWordWrap(true);
        tooltipWidget->adjustSize();

        // On Windows, a lot of the resizing/activating happens when calling
        // show() and calling it doesn't synchronously create a visible window,
        // so moving the window won't cause the visible window to jump.
        //
        // On other platforms, this isn't the case, hence we call show() after
        // moving.
#ifdef Q_OS_WIN
        tooltipWidget->show();
#endif

        auto pos = this->mapToGlobal(this->rect().bottomLeft()) +
                   QPoint((this->width() - tooltipWidget->width()) / 2, 1);

        tooltipWidget->moveTo(pos, widgets::BoundsChecking::CursorPosition);

#ifndef Q_OS_WIN
        tooltipWidget->show();
#endif
    }

    BaseWidget::enterEvent(event);
}

void SplitHeader::leaveEvent(QEvent *event)
{
    this->hideTooltip();

    BaseWidget::leaveEvent(event);
}

void SplitHeader::themeChangedEvent()
{
    auto palette = QPalette();

    if (this->split_->hasFocus() && this->theme->customization.foundation !=
                                        ThemeFoundation::MoltorinoPolished)
    {
        palette.setColor(QPalette::WindowText,
                         this->theme->splits.header.focusedText);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    }
    this->titleLabel_->setPalette(palette);

    if (this->theme->customization.foundation ==
        ThemeFoundation::MoltorinoPolished)
    {
        this->addButton_->setOptions({});
    }
    else
    {
        auto bg = this->theme->splits.header.background;
        this->addButton_->setOptions({
            .background = bg,
            .backgroundHover = bg,
        });
    }

    this->update();
}

void SplitHeader::reloadChannelEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedChannelEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedChannelEmotes_ = now;

    auto channel = this->split_->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshFFZChannelEmotes(true);
        twitchChannel->refreshBTTVChannelEmotes(true);
        twitchChannel->refreshSevenTVChannelEmotes(true);
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        kc->reloadSeventvEmotes(true);
    }
}

void SplitHeader::reloadSubscriberEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedSubEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedSubEmotes_ = now;

    auto channel = this->split_->getSelectedChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshTwitchChannelEmotes(true);
    }
}

void SplitHeader::reconnect()
{
    this->split_->getChannel()->reconnect();
}

}  // namespace chatterino
