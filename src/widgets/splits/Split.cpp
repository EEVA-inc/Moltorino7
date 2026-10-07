// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/Split.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/chat/ChatAutomationController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/twitch/ChannelManagement.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeChatServer.hpp"
#include "providers/youtube/YouTubeTypes.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/ThemeCustomization.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CustomPlayer.hpp"
#include "util/MultiChannel.hpp"
#include "util/StreamLink.hpp"
#include "widgets/ChatterListWidget.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/dialogs/SelectChannelFiltersDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/AutoModReviewBar.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/DebugPopup.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/helper/PollBanner.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/helper/SearchPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/OverlayWindow.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/DraggedSplit.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/helper/PinnedMessageBanner.hpp"
#include "widgets/helper/PredictionBanner.hpp"
#include "widgets/splits/SplitOverlay.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDrag>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMovie>
#include <QPainter>
#include <QSet>
#include <QVector>
#include <QVBoxLayout>

#include <functional>
#include <optional>

namespace chatterino {
namespace {
constexpr int DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS = 3500;
constexpr int DEFERRED_TWITCH_POLL_REFRESH_OFFSET_MS = 250;
constexpr int DEFERRED_TWITCH_POINTS_REFRESH_OFFSET_MS = 500;
constexpr int DEFERRED_TWITCH_WARNING_REFRESH_OFFSET_MS = 650;
constexpr int DEFERRED_TWITCH_EDITOR_REFRESH_OFFSET_MS = 375;
constexpr int DEFERRED_TWITCH_ROOM_ID_RETRY_MS = 1000;
constexpr int DEFERRED_TWITCH_ROOM_ID_MAX_RETRIES = 12;
constexpr int INTERACTIVE_TWITCH_FEATURE_REFRESH_DELAY_MS = 75;
constexpr int INTERACTIVE_TWITCH_POLL_REFRESH_OFFSET_MS = 60;
constexpr int INTERACTIVE_TWITCH_POINTS_REFRESH_OFFSET_MS = 120;
constexpr int INTERACTIVE_TWITCH_WARNING_REFRESH_OFFSET_MS = 180;
constexpr int INTERACTIVE_TWITCH_EDITOR_REFRESH_OFFSET_MS = 90;
constexpr int INTERACTIVE_TWITCH_ROOM_ID_RETRY_MS = 250;

QString tikTokInputPlaceholder(const Channel &channel, bool combined)
{
    const auto account = getApp()->getAccounts()->tiktok.current();
    if (account->isAnonymous())
    {
        return QStringLiteral("Connect a TikTok account to send messages...");
    }
    if (account->isLoadingCredentials())
    {
        return QStringLiteral("Loading TikTok account...");
    }
    if (!account->hasCredentials())
    {
        return QStringLiteral("Log in to TikTok again to send messages...");
    }
    return combined ? QStringLiteral("Send in %1 as @%2...")
                          .arg(channel.getDisplayName(), account->handle())
                    : QStringLiteral("Send message as @%1...")
                          .arg(account->handle());
}

std::vector<std::shared_ptr<TwitchChannel>> automationTwitchChannels(
    const ChannelPtr &channel)
{
    std::vector<std::shared_ptr<TwitchChannel>> channels;
    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        channels.push_back(std::move(twitch));
        return channels;
    }
    if (auto multi = std::dynamic_pointer_cast<MultiChannel>(channel))
    {
        for (const auto &child : multi->channels())
        {
            if (child.platform != MultiChannel::Platform::Twitch)
            {
                continue;
            }
            if (auto twitch =
                    std::dynamic_pointer_cast<TwitchChannel>(child.channel))
            {
                channels.push_back(std::move(twitch));
            }
        }
    }
    return channels;
}

QElapsedTimer &twitchStartupTimer()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer created;
        created.start();
        return created;
    }();
    return timer;
}

bool shouldUseColdTwitchFeatureDelay()
{
    return twitchStartupTimer().elapsed() <
           DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS;
}

void showTutorialVideo(QWidget *parent, const QString &source,
                       const QString &title, const QString &description)
{
    auto *window = new BasePopup(
        {
            BaseWindow::EnableCustomFrame,
            BaseWindow::BoundsCheckOnShow,
        },
        parent);
    window->setWindowTitle("Chatterino - " + title);
    window->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(description));
    auto *label = new QLabel(window);
    layout->addWidget(label);
    auto *movie = new QMovie(label);
    movie->setFileName(source);
    label->setMovie(movie);
    movie->start();
    window->getLayoutContainer()->setLayout(layout);
    window->show();
}
}  // namespace

pajlada::Signals::Signal<Qt::KeyboardModifiers> Split::modifierStatusChanged;
Qt::KeyboardModifiers Split::modifierStatus = Qt::NoModifier;

Split::Split(QWidget *parent)
    : BaseWidget(parent)
    , channel_(Channel::getEmpty())
    , vbox_(new QVBoxLayout(this))
    , view_(
          new ChannelView(this, this, ChannelView::Context::None,
                          sanitizeScrollbackLimit(
                              getSettings()->scrollbackSplitLimit.getValue())))
{
    this->setMouseTracking(true);
    this->view_->setPausable(true);

    this->vbox_->setSpacing(0);
    this->themeChangedEvent();

    this->vbox_->addWidget(this->view_, 1);

    // update placeholder text on Twitch account change and channel change
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this] {
            this->updateInputPlaceholder();
            this->deferredTwitchForcePersonalRefresh_ = true;
            this->scheduleDeferredTwitchRefresh(true);
        });
    this->signalHolder_.managedConnect(this->channelChanged, [this] {
        this->updateInputPlaceholder();
    });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.currentChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.credentialsChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.userListUpdated, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->tiktok.currentChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->tiktok.credentialsChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->signalHolder_.managedConnect(this->focused, [this] {
        this->refreshSelectedYouTube();
    });

    getSettings()->showInputPlaceholder.connect(
        [this](const bool &, auto) {
            this->updateInputPlaceholder();
        },
        this->signalHolder_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->editorAccessProbedChannels_.clear();
            this->scheduleDeferredTwitchRefresh(true);
        },
        this->signalHolder_);
    getSettings()->showEditStreamInfoButtonInSplitHeader.connect(
        [this](bool enabled, auto) {
            if (enabled)
            {
                this->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->signalHolder_);

    // clear SplitInput selection when selecting in ChannelView
    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->selectionChanged.connect([this]() {
        if (this->input_ != nullptr && this->input_->hasSelection())
        {
            this->input_->clearSelection();
        }
    });

    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->openChannelIn.connect(
        [this](QString twitchChannel, FromTwitchLinkOpenChannelIn openIn) {
            ChannelPtr channel =
                getApp()->getTwitch()->getOrAddChannel(twitchChannel);
            switch (openIn)
            {
                case FromTwitchLinkOpenChannelIn::Split:
                    this->openSplitRequested.invoke(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Tab:
                    this->joinChannelInNewTab(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::BrowserPlayer:
                    this->openChannelInBrowserPlayer(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Streamlink:
                    this->openChannelInStreamlink(twitchChannel);
                    break;
                case FromTwitchLinkOpenChannelIn::CustomPlayer:
                    this->openChannelInCustomPlayer(twitchChannel);
                default:
                    qCWarning(chatterinoWidget)
                        << "Unhandled \"FromTwitchLinkOpenChannelIn\" enum "
                           "value: "
                        << static_cast<int>(openIn);
            }
        });

    getSettings()->showEmptyInput.connect(
        [this] {
            if (this->input_ != nullptr)
            {
                this->refreshInputState(this->input_->getInputText());
            }
        },
        this->signalHolder_);

    this->setSizePolicy(QSizePolicy::MinimumExpanding,
                        QSizePolicy::MinimumExpanding);

    // update moderation button when items changed
    this->signalHolder_.managedConnect(
        getSettings()->moderationActions.delayedItemsChanged, [this] {
            this->refreshModerationMode();
        });

    this->signalHolder_.managedConnect(
        modifierStatusChanged, [this](Qt::KeyboardModifiers status) {
            if ((status ==
                 SHOW_SPLIT_OVERLAY_MODIFIERS /*|| status == showAddSplitRegions*/) &&
                this->isMouseOver_)
            {
                this->ensureOverlay()->show();
            }
            else if (this->overlay_ != nullptr)
            {
                this->overlay_->hide();
            }

            if (getSettings()->pauseChatModifier.getEnum() != Qt::NoModifier &&
                status == getSettings()->pauseChatModifier.getEnum() &&
                this->isVisible())
            {
                this->view_->pause(PauseReason::KeyboardModifier);
            }
            else
            {
                this->view_->unpause(PauseReason::KeyboardModifier);
            }
        });

    this->deferredTwitchRefreshTimer_ = new QTimer(this);
    this->deferredTwitchRefreshTimer_->setSingleShot(true);
    this->deferredTwitchRefreshTimer_->setInterval(
        DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS);
    QObject::connect(this->deferredTwitchRefreshTimer_, &QTimer::timeout, this,
                     [this] {
                         this->runDeferredTwitchRefresh();
                     });

    getSettings()->imageUploaderEnabled.connect(
        [this](const bool &val) {
            this->setAcceptDrops(val);
        },
        this->signalHolder_);
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           if (!this->shortcutsActive_)
                                           {
                                               return;
                                           }
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    QObject::connect(
        this->view_, &ChannelView::messageAddedToChannel, this,
        [this](MessagePtr &message) {
            if (!getSettings()->pulseTextInputOnSelfMessage ||
                this->input_ == nullptr)
            {
                return;
            }

            bool isSelf = false;
            switch (message->platform)
            {
                case MessagePlatform::AnyOrTwitch: {
                    const auto user =
                        getApp()->getAccounts()->twitch.getCurrent();
                    isSelf =
                        !user->isAnon() && message->userID == user->getUserId();
                }
                break;
                case MessagePlatform::Kick: {
                    const auto user = getApp()->getAccounts()->kick.current();
                    isSelf = !user->isAnonymous() &&
                             message->userID == QString::number(user->userID());
                }
                break;
                case MessagePlatform::YouTube: {
                    const auto user =
                        getApp()->getAccounts()->youtube.current();
                    isSelf = !user->isAnonymous() &&
                             message->userID == user->channelID();
                }
                break;
                case MessagePlatform::TikTok:
                    break;
            }

            if (isSelf)
            {
                // A message from yourself was just received in this split
                this->input_->triggerSelfMessageReceived();
            }
        });
}

SplitHeader *Split::ensureHeader()
{
    if (this->header_ != nullptr)
    {
        return this->header_;
    }

    this->header_ = new SplitHeader(this);
    this->applyScaleToLazyChild(this->header_);
    this->vbox_->insertWidget(0, this->header_);
    this->header_->setAddButtonVisible(this->isTopRightSplit_);
    this->header_->updateRoomModes();
    if (this->autoModReviewBar_ != nullptr)
    {
        this->header_->setAutoModReviewBar(this->autoModReviewBar_);
    }
    return this->header_;
}

SplitInput *Split::ensureInput()
{
    if (this->input_ != nullptr)
    {
        return this->input_;
    }

    this->input_ = new SplitInput(this);
    this->applyScaleToLazyChild(this->input_);
    this->vbox_->addWidget(this->input_);
    this->initializeInputConnections();

    if (auto *eventFilter = this->parentWidget(); eventFilter != nullptr)
    {
        this->input_->ui_.textEdit->installEventFilter(eventFilter);
    }

    if (this->channel_.get()->getType() == Channel::Type::TwitchAutomod)
    {
        this->view_->setFocusProxy(nullptr);
        this->view_->setFocusPolicy(Qt::StrongFocus);
        this->setFocusProxy(this->view_);
    }
    else
    {
        this->view_->setFocusProxy(this->input_->ui_.textEdit);
        this->view_->setFocusPolicy(Qt::ClickFocus);
        this->setFocusProxy(this->input_->ui_.textEdit);
    }

    this->input_->setCheckSpellingOverride(this->checkSpellingOverride_);
    this->input_->setSendWaitStatus(this->pendingSendWaitStatus_);
    this->updateInputPlaceholder();
    this->refreshInputState(this->input_->getInputText());

    return this->input_;
}

void Split::applyScaleToLazyChild(BaseWidget *widget) const
{
    const auto scale = this->overrideScale();
    if (widget == nullptr || !scale)
    {
        return;
    }

    widget->setOverrideScale(scale);
    for (auto *child : widget->findChildren<BaseWidget *>())
    {
        child->setOverrideScale(scale);
    }
}

void Split::initializeInputConnections()
{
    assert(this->input_ != nullptr);

    // clear ChannelView selection when selecting in SplitInput
    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->selectionChanged.connect([this]() {
        if (this->view_->hasSelection())
        {
            this->view_->clearSelection();
        }
    });
    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore =
        this->input_->textChanged.connect([this](const QString &newText) {
            this->refreshInputState(newText);
        });
    std::ignore = this->input_->historySearchStateChanged.connect([this] {
        this->refreshInputState(this->input_->getInputText());
    });

    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focused,
                                       [this] {
                                           // Forward textEdit's focused event
                                           this->focused.invoke();
                                       });
    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focusLost,
                                       [this] {
                                           // Forward textEdit's focusLost event
                                           this->focusLost.invoke();
                                       });

    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->ui_.textEdit->imagePasted.connect(
        [this](const QMimeData *original) {
            if (!getSettings()->imageUploaderEnabled)
            {
                return;
            }

            auto channel = this->getChannel();
            auto *imageUploader = getApp()->getImageUploader();

            auto [images, imageProcessError] =
                imageUploader->getImages(original);
            if (images.empty())
            {
                channel->addSystemMessage(
                    QString(
                        "An error occurred trying to process your image: %1")
                        .arg(imageProcessError));
                return;
            }

            if (getSettings()->askOnImageUpload.getValue())
            {
                QMessageBox msgBox(this->window());
                msgBox.setWindowTitle("Moltorino");
                msgBox.setText("Image upload");
                msgBox.setInformativeText(
                    "You are uploading an image to a 3rd party service not in "
                    "control of the Moltorino team. You may not be able to "
                    "remove the image from the site. Are you okay with this?");
                auto *cancel = msgBox.addButton(QMessageBox::Cancel);
                auto *yes = msgBox.addButton(QMessageBox::Yes);
                auto *yesDontAskAgain = msgBox.addButton("Yes, don't ask again",
                                                         QMessageBox::YesRole);

                msgBox.setDefaultButton(QMessageBox::Yes);
                msgBox.exec();

                auto *clickedButton = msgBox.clickedButton();
                if (clickedButton == yesDontAskAgain)
                {
                    getSettings()->askOnImageUpload.setValue(false);
                }
                else if (clickedButton == yes)
                {
                    // Continue with image upload
                }
                else if (clickedButton == cancel)
                {
                    // Not continuing with image upload
                    return;
                }
                else
                {
                    // An unknown "button" was pressed - handle it as if cancel was pressed
                    // cancel is already handled as the "escape" option, so this should never happen
                    qCWarning(chatterinoImageuploader)
                        << "Unhandled button pressed:" << clickedButton;
                    return;
                }
            }

            QPointer<ResizingTextEdit> edit = this->input_->ui_.textEdit;
            imageUploader->upload(std::move(images), channel, edit);
        });
}

void Split::setSendWaitStatus(QString text)
{
    this->pendingSendWaitStatus_ = std::move(text);
    if (this->input_ != nullptr)
    {
        this->input_->setSendWaitStatus(this->pendingSendWaitStatus_);
    }
}

AutoModReviewBar *Split::ensureAutoModReviewBar()
{
    if (this->autoModReviewBar_ == nullptr)
    {
        this->autoModReviewBar_ = new AutoModReviewBar(this, this->view_);
        this->applyScaleToLazyChild(this->autoModReviewBar_);
        if (this->header_ != nullptr)
        {
            this->header_->setAutoModReviewBar(this->autoModReviewBar_);
        }
        this->autoModReviewBar_->setSelectedChannel(
            this->autoModChannelFilter_);
    }
    return this->autoModReviewBar_;
}

SplitOverlay *Split::ensureOverlay()
{
    if (this->overlay_ == nullptr)
    {
        this->overlay_ = new SplitOverlay(this);
        this->applyScaleToLazyChild(this->overlay_);
        this->overlay_->setGeometry(this->rect());
    }
    return this->overlay_;
}

void Split::hideEvent(QHideEvent *event)
{
    this->isMouseOver_ = false;
    if (this->overlay_ != nullptr)
    {
        this->overlay_->hide();
    }

    this->view_->unpause(PauseReason::KeyboardModifier);

    if (this->shortcutsActive_)
    {
        this->clearShortcuts();
        this->shortcutsActive_ = false;
    }
    BaseWidget::hideEvent(event);
}

void Split::refreshSelectedYouTube()
{
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!this->isVisible() ||
        (container != nullptr && container->getSelectedSplit() != nullptr &&
         container->getSelectedSplit() != this))
    {
        return;
    }

    std::vector<std::shared_ptr<YouTubeChannel>> channels;
    const auto underlying = this->getChannel();
    if (const auto *multi =
            dynamic_cast<const MultiChannel *>(underlying.get()))
    {
        for (const auto &child : multi->channels())
        {
            if (child.platform == MultiChannel::Platform::YouTube)
            {
                if (auto channel = std::dynamic_pointer_cast<YouTubeChannel>(
                        child.channel))
                {
                    channels.emplace_back(std::move(channel));
                }
            }
        }
    }
    else if (auto channel = std::dynamic_pointer_cast<YouTubeChannel>(
                 this->getSelectedChannel()))
    {
        channels.emplace_back(std::move(channel));
    }
    getApp()->getYouTubeChatServer()->setActiveChannels(channels, this);
}

bool Split::hasVisibleBanner() const
{
    return (this->pinnedBanner_ && this->pinnedBanner_->isVisible()) ||
           (this->predictionBanner_ && this->predictionBanner_->isVisible()) ||
           (this->pollBanner_ && this->pollBanner_->isVisible());
}

void Split::refreshInputState(const QString &inputText)
{
    if (this->input_ == nullptr)
    {
        return;
    }

    if (getSettings()->showEmptyInput)
    {
        if (this->input_->isHidden())
        {
            this->input_->show();
        }
        return;
    }

    if (inputText.isEmpty() && !this->input_->isInHistorySearch())
    {
        this->input_->hide();
    }
    else
    {
        this->input_->show();
    }
}

PinnedMessageBanner *Split::ensurePinnedBanner()
{
    if (this->pinnedBanner_ != nullptr)
    {
        return this->pinnedBanner_;
    }

    auto *banner = new PinnedMessageBanner(this, this);
    this->applyScaleToLazyChild(banner);
    QWidget *before = this->view_;
    if (this->pollBanner_ != nullptr)
    {
        before = this->pollBanner_;
    }
    if (this->predictionBanner_ != nullptr)
    {
        before = this->predictionBanner_;
    }
    this->vbox_->insertWidget(this->vbox_->indexOf(before), banner);
    this->pinnedBanner_ = banner;

    this->signalHolder_.managedConnect(banner->toggleBannerRequested, [this] {
        this->cycleBannerSelection();
    });
    this->signalHolder_.managedConnect(banner->dismissed, [this] {
        this->handleBannerDismissed();
    });

    return banner;
}

PredictionBanner *Split::ensurePredictionBanner()
{
    if (this->predictionBanner_ != nullptr)
    {
        return this->predictionBanner_;
    }

    auto *banner = new PredictionBanner(this, this);
    this->applyScaleToLazyChild(banner);
    QWidget *before = this->pollBanner_ != nullptr
                          ? static_cast<QWidget *>(this->pollBanner_)
                          : this->view_;
    this->vbox_->insertWidget(this->vbox_->indexOf(before), banner);
    this->predictionBanner_ = banner;

    this->signalHolder_.managedConnect(banner->toggleBannerRequested, [this] {
        this->cycleBannerSelection();
    });
    this->signalHolder_.managedConnect(banner->dismissed, [this] {
        this->handleBannerDismissed();
    });

    return banner;
}

PollBanner *Split::ensurePollBanner()
{
    if (this->pollBanner_ != nullptr)
    {
        return this->pollBanner_;
    }

    auto *banner = new PollBanner(this, this);
    this->applyScaleToLazyChild(banner);
    this->vbox_->insertWidget(this->vbox_->indexOf(this->view_), banner);
    this->pollBanner_ = banner;

    this->signalHolder_.managedConnect(banner->toggleBannerRequested, [this] {
        this->cycleBannerSelection();
    });
    this->signalHolder_.managedConnect(banner->dismissed, [this] {
        this->handleBannerDismissed();
    });

    return banner;
}

void Split::cycleBannerSelection()
{
    this->clearBannerAttention();

    QVector<int> activeIds;
    if (this->pinnedBanner_ != nullptr &&
        this->pinnedBanner_->hasPinnedMessage())
    {
        activeIds.push_back(0);
    }
    if (this->predictionBanner_ != nullptr &&
        this->predictionBanner_->hasPrediction())
    {
        activeIds.push_back(1);
    }
    if (this->pollBanner_ != nullptr && this->pollBanner_->hasPoll())
    {
        activeIds.push_back(2);
    }

    if (!activeIds.isEmpty())
    {
        int visibleId = this->bannerToggleOverride_;
        if (this->pinnedBanner_ && !this->pinnedBanner_->isHidden())
        {
            visibleId = 0;
        }
        else if (this->predictionBanner_ && !this->predictionBanner_->isHidden())
        {
            visibleId = 1;
        }
        else if (this->pollBanner_ && !this->pollBanner_->isHidden())
        {
            visibleId = 2;
        }
        const int foundIndex = activeIds.indexOf(visibleId);
        const int currentIndex = foundIndex >= 0 ? foundIndex : 0;
        this->bannerToggleOverride_ =
            activeIds.at((currentIndex + 1) % activeIds.size());
    }
    this->updateBannerVisibility();
}

void Split::handleBannerDismissed()
{
    this->bannerToggleOverride_ = -1;
    this->updateBannerVisibility();
}

std::shared_ptr<TwitchChannel> Split::getTwitchFeatureChannel() const
{
    return this->twitchFeatureChannel_.lock();
}

void Split::clearTwitchFeatureState()
{
    this->bannerToggleOverride_ = -1;
    this->clearBannerAttention();
    this->lastPinBannerKey_.clear();
    this->lastPredictionBannerKey_.clear();
    this->lastPollBannerKey_.clear();

    if (this->pinnedBanner_ != nullptr)
    {
        this->pinnedBanner_->setPinnedMessage(std::nullopt, nullptr);
    }
    if (this->predictionBanner_ != nullptr)
    {
        this->predictionBanner_->setPrediction(std::nullopt, nullptr);
    }
    if (this->pollBanner_ != nullptr)
    {
        this->pollBanner_->setPoll(std::nullopt, nullptr);
    }
    this->updateBannerVisibility();
}

void Split::bindTwitchFeatureChannel(
    const std::shared_ptr<TwitchChannel> &channel)
{
    if (!channel)
    {
        return;
    }

    const auto weakChannel = std::weak_ptr<TwitchChannel>(channel);
    auto updatePin = [this, weakChannel] {
        const auto tc = weakChannel.lock();
        if (!tc || tc != this->getTwitchFeatureChannel())
        {
            return;
        }

        if (getSettings()->enablePinnedMessages)
        {
            this->noteBannerStateChanged(tc.get(), 0);
            const auto pin = tc->accessPinnedMessage();
            if (pin->has_value())
            {
                this->ensurePinnedBanner()->setPinnedMessage(*pin, tc.get());
            }
            else if (this->pinnedBanner_ != nullptr)
            {
                this->pinnedBanner_->setPinnedMessage(std::nullopt, tc.get());
            }
        }
        else if (this->pinnedBanner_ != nullptr)
        {
            this->pinnedBanner_->setPinnedMessage(std::nullopt, tc.get());
        }
        this->updateBannerVisibility();
    };

    this->twitchFeatureSignalHolder_.managedConnect(
        channel->pinnedMessageChanged, updatePin);

    this->twitchFeatureSignalHolder_.managedConnect(
        channel->messageReplaced,
        [this, weakChannel](size_t, const MessagePtr &,
                            const MessagePtr &replacement) {
            const auto tc = weakChannel.lock();
            if (!tc || tc != this->getTwitchFeatureChannel() ||
                !getSettings()->enablePinnedMessages)
            {
                return;
            }
            const auto pin = tc->accessPinnedMessage();
            if (pin->has_value() && !(*pin)->messageId.isEmpty() &&
                replacement->id == (*pin)->messageId)
            {
                this->ensurePinnedBanner()->setPinnedMessage(*pin, tc.get());
            }
        });

    this->twitchFeatureSignalHolder_.managedConnect(
        channel->messageAppended,
        [this, weakChannel](MessagePtr &message, std::optional<MessageFlags>) {
            const auto tc = weakChannel.lock();
            if (!tc || tc != this->getTwitchFeatureChannel() ||
                !getSettings()->enablePinnedMessages)
            {
                return;
            }
            const auto pin = tc->accessPinnedMessage();
            if (!pin->has_value() || (*pin)->authorLogin.isEmpty() ||
                message->loginName.compare((*pin)->authorLogin,
                                           Qt::CaseInsensitive) != 0)
            {
                return;
            }
            if (this->pinnedBanner_ != nullptr)
            {
                this->pinnedBanner_->refreshLayout();
            }
            else
            {
                this->ensurePinnedBanner()->setPinnedMessage(*pin, tc.get());
            }
        });

    auto updatePrediction = [this, weakChannel] {
        const auto tc = weakChannel.lock();
        if (!tc || tc != this->getTwitchFeatureChannel())
        {
            return;
        }

        const bool enabled = getSettings()->enablePredictions;
        if (enabled)
        {
            this->noteBannerStateChanged(tc.get(), 1);
        }
        const auto prediction = tc->accessPrediction();
        if (enabled && prediction->has_value())
        {
            this->ensurePredictionBanner()->setPrediction(*prediction,
                                                          tc.get());
        }
        else if (this->predictionBanner_ != nullptr)
        {
            this->predictionBanner_->setPrediction(std::nullopt, tc.get());
        }
        this->updateBannerVisibility();
    };

    auto updatePoll = [this, weakChannel] {
        const auto tc = weakChannel.lock();
        if (!tc || tc != this->getTwitchFeatureChannel())
        {
            return;
        }

        const bool enabled = getSettings()->enablePolls;
        if (enabled)
        {
            this->noteBannerStateChanged(tc.get(), 2);
        }
        const auto poll = tc->accessPoll();
        if (enabled && poll->has_value())
        {
            this->ensurePollBanner()->setPoll(*poll, tc.get());
        }
        else if (this->pollBanner_ != nullptr)
        {
            this->pollBanner_->setPoll(std::nullopt, tc.get());
        }
        this->updateBannerVisibility();
    };

    this->twitchFeatureSignalHolder_.managedConnect(channel->predictionChanged,
                                                    updatePrediction);
    this->twitchFeatureSignalHolder_.managedConnect(channel->pollChanged,
                                                    updatePoll);

    getSettings()->enablePinnedMessages.connect(
        [this, updatePin](const bool &enabled, auto) {
            updatePin();
            if (enabled)
            {
                this->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->twitchFeatureSignalHolder_);
    getSettings()->enablePredictions.connect(
        [this, updatePrediction](const bool &enabled, auto) {
            updatePrediction();
            if (enabled)
            {
                this->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->twitchFeatureSignalHolder_);
    getSettings()->enablePolls.connect(
        [this, updatePoll](const bool &enabled, auto) {
            updatePoll();
            if (enabled)
            {
                this->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->twitchFeatureSignalHolder_);
    getSettings()->bannerStackMode.connect(
        [this](const int &, auto) {
            this->bannerToggleOverride_ = -1;
            this->updateBannerVisibility();
        },
        this->twitchFeatureSignalHolder_);

    this->twitchFeatureSignalHolder_.managedConnect(this->focused, [this] {
        this->scheduleDeferredTwitchRefresh(true);
    });

    updatePin();
    updatePrediction();
    updatePoll();
}

void Split::updateTwitchFeatureChannel()
{
    const auto current = this->getTwitchFeatureChannel();
    std::shared_ptr<TwitchChannel> preferred;
    const auto underlying = this->getChannel();

    if (const auto direct =
            std::dynamic_pointer_cast<TwitchChannel>(underlying))
    {
        preferred = direct;
    }
    else if (const auto *multi =
                 dynamic_cast<const MultiChannel *>(underlying.get()))
    {
        if (const auto *active = multi->activeChannel();
            active != nullptr &&
            active->platform == MultiChannel::Platform::Twitch)
        {
            preferred =
                std::dynamic_pointer_cast<TwitchChannel>(active->channel);
        }

        if (!preferred && current)
        {
            for (const auto &child : multi->channels())
            {
                if (child.platform == MultiChannel::Platform::Twitch &&
                    child.channel == current)
                {
                    preferred = current;
                    break;
                }
            }
        }

        if (!preferred)
        {
            for (const auto &child : multi->channels())
            {
                if (child.platform == MultiChannel::Platform::Twitch)
                {
                    preferred =
                        std::dynamic_pointer_cast<TwitchChannel>(child.channel);
                    if (preferred)
                    {
                        break;
                    }
                }
            }
        }
    }

    if (preferred == current)
    {
        return;
    }

    const bool wasPriming = this->primingBannerState_;
    this->primingBannerState_ = true;
    if (this->deferredTwitchRefreshTimer_ != nullptr)
    {
        this->deferredTwitchRefreshTimer_->stop();
    }
    this->deferredTwitchRefreshRetries_ = 0;
    this->deferredTwitchRefreshInteractive_ = false;
    this->twitchFeatureSignalHolder_.clear();
    this->twitchFeatureChannel_ = preferred;
    this->clearTwitchFeatureState();
    this->bindTwitchFeatureChannel(preferred);
    this->primingBannerState_ = wasPriming;
}

void Split::updateChannelConnections()
{
    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->sendWaitConnection_ = pajlada::Signals::ScopedConnection{};
    this->setSendWaitStatus({});

    auto *channel = this->channel_.get().get();
    auto *mc = dynamic_cast<MultiChannel *>(channel);
    if (mc)
    {
        const auto *active = mc->activeChannel();
        if (active == nullptr)
        {
            return;
        }
        channel = active->channel.get();
    }

    auto *tc = dynamic_cast<TwitchChannel *>(channel);
    auto *kc = dynamic_cast<KickChannel *>(channel);
    auto *youtube = dynamic_cast<YouTubeChannel *>(channel);
    if (auto *tiktok = dynamic_cast<TikTokChannel *>(channel))
    {
        this->usermodeChangedConnection_ =
            tiktok->userStateChanged.connect([this] {
                this->updateInputPlaceholder();
            });
    }
    if (tc)
    {
        this->usermodeChangedConnection_ = tc->userStateChanged.connect([this] {
            this->updateInputPlaceholder();
            this->refreshModerationMode();
            if (this->header_ != nullptr)
            {
                this->header_->updateChannelText();
                this->header_->updateRoomModes();
            }
        });

        this->roomModeChangedConnection_ = tc->roomModesChanged.connect([this] {
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->sendWaitConnection_ =
            tc->sendWaitUpdate.connect([this](const QString &text) {
                this->setSendWaitStatus(text);
            });
    }
    else if (kc != nullptr)
    {
        this->usermodeChangedConnection_ = kc->userStateChanged.connect([this] {
            this->refreshModerationMode();
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->roomModeChangedConnection_ = kc->roomModesChanged.connect([this] {
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->sendWaitConnection_ =
            kc->sendWaitUpdate.connect([this](const QString &text) {
                this->setSendWaitStatus(text);
            });
    }
    else if (youtube != nullptr)
    {
        this->usermodeChangedConnection_ =
            youtube->userStateChanged.connect([this] {
                this->refreshModerationMode();
            });
    }
}

void Split::setAutoModChannelFilter(QString channel)
{
    this->autoModChannelFilter_ = std::move(channel);
    if (this->autoModReviewBar_ != nullptr)
    {
        this->autoModReviewBar_->setSelectedChannel(
            this->autoModChannelFilter_);
    }
}

QString Split::getAutoModChannelFilter() const
{
    return this->autoModReviewBar_ != nullptr
               ? this->autoModReviewBar_->selectedChannel()
               : this->autoModChannelFilter_;
}

void Split::themeChangedEvent()
{
    const int frameWidth =
        themeUsesClassicSplitFrame(this->theme->customization.foundation) ? 1
                                                                          : 0;
    this->vbox_->setContentsMargins(frameWidth, frameWidth, frameWidth,
                                    frameWidth);
    this->update();
}

void Split::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"toggleChatRecording",
         [this](const std::vector<QString> &) -> QString {
             if (auto *recordings = getApp()->getChatRecordings())
             {
                 recordings->toggle(recordingTabFor(this));
             }
             return {};
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->deleteFromContainer();
             return "";
         }},
        {"changeChannel",
         [this](const std::vector<QString> &) -> QString {
             this->changeChannel();
             return "";
         }},
        {"showSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(true);
             return "";
         }},
        {"showGlobalSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(false);
             return "";
         }},
        {"reconnect",
         [this](const std::vector<QString> &) -> QString {
             this->reconnect();
             return "";
         }},
        {"debug",
         [](const std::vector<QString> &) -> QString {
             auto *popup = new DebugPopup;
             popup->setAttribute(Qt::WA_DeleteOnClose);
             popup->setWindowTitle("Chatterino - Debug popup");
             popup->show();
             return "";
         }},
        {"focus",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "focus action requires only one argument: the "
                        "focus direction Use \"up\", \"above\", \"down\", "
                        "\"below\", \"left\" or \"right\".";
             }
             const auto &direction = arguments.at(0);
             if (direction == "up" || direction == "above")
             {
                 this->actionRequested.invoke(Action::SelectSplitAbove);
             }
             else if (direction == "down" || direction == "below")
             {
                 this->actionRequested.invoke(Action::SelectSplitBelow);
             }
             else if (direction == "left")
             {
                 this->actionRequested.invoke(Action::SelectSplitLeft);
             }
             else if (direction == "right")
             {
                 this->actionRequested.invoke(Action::SelectSplitRight);
             }
             else
             {
                 return "focus in unknown direction. Use \"up\", "
                        "\"above\", \"down\", \"below\", \"left\" or "
                        "\"right\".";
             }
             return "";
         }},
        {"scrollToBottom",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToBottom(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollToTop",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToTop(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollPage",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             const auto &direction = arguments.at(0);

             auto &scrollbar = this->getChannelView().getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"pickFilters",
         [this](const std::vector<QString> &) -> QString {
             this->setFiltersDialog();
             return "";
         }},
        {"openInBrowser",
         [this](const std::vector<QString> &) -> QString {
             if (this->getChannel()->getType() == Channel::Type::TwitchWhispers)
             {
                 this->openWhispersInBrowser();
             }
             else
             {
                 this->openInBrowser();
             }

             return "";
         }},
        {"openInStreamlink",
         [this](const std::vector<QString> &) -> QString {
             this->openInStreamlink();
             return "";
         }},
        {"openInCustomPlayer",
         [this](const std::vector<QString> &) -> QString {
             this->openWithCustomScheme();
             return "";
         }},
        {"openPlayerInBrowser",
         [this](const std::vector<QString> &) -> QString {
             this->openBrowserPlayer();
             return "";
         }},
        {"openModView",
         [this](const std::vector<QString> &) -> QString {
             this->openModViewInBrowser();
             return "";
         }},
        {"createClip",
         [this](const std::vector<QString> &) -> QString {
             // Alt+X: create clip LUL
             auto channel = this->getSelectedChannel();
             if (const auto type = channel->getType();
                 type != Channel::Type::Twitch &&
                 type != Channel::Type::TwitchWatching)
             {
                 return "Cannot create clips in a non-Twitch channel.";
             }

             auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());

             twitchChannel->createClip({}, {});
             return "";
         }},
        {"reloadEmotes",
         [this](const std::vector<QString> &arguments) -> QString {
             auto reloadChannel = true;
             auto reloadSubscriber = true;
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "channel")
                 {
                     reloadSubscriber = false;
                 }
                 else if (arg == "subscriber")
                 {
                     reloadChannel = false;
                 }
             }

             if (reloadChannel)
             {
                 this->ensureHeader()->reloadChannelEmotes();
             }
             if (reloadSubscriber)
             {
                 this->ensureHeader()->reloadSubscriberEmotes();
             }
             return "";
         }},
        {"setModerationMode",
         [this](const std::vector<QString> &arguments) -> QString {
             const auto channel = this->getSelectedChannel();
             if (!channel->isTwitchOrKickChannel() &&
                 !channel->isYouTubeChannel())
             {
                 return "Cannot set moderation mode in this channel.";
             }
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             switch (mode)
             {
                 case 0:
                     this->setModerationMode(false);
                     break;
                 case 1:
                     this->setModerationMode(true);
                     break;
                 default:
                     this->setModerationMode(!this->getModerationMode());
             }

             return "";
         }},
        {"openViewerList",
         [this](const std::vector<QString> &) -> QString {
             this->openChatterList();
             return "";
         }},
        {"clearMessages",
         [this](const std::vector<QString> &) -> QString {
             this->clear();
             return "";
         }},
        {"runCommand",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "runCommand hotkey called without arguments!";
                 return "runCommand hotkey called without arguments!";
             }
             QString requestedText = QString(arguments[0]).replace('\n', ' ');
             auto channel = this->getSelectedChannel();
             if (channel == nullptr)
             {
                 return "Cannot run a command without a selected channel.";
             }

             QString inputText = this->getInput().getInputText();
             QString message = getApp()->getCommands()->execCustomCommand(
                 requestedText.split(' '), Command{"(hotkey)", requestedText},
                 true, channel, nullptr,
                 {
                     {"input.text", inputText},
                 });

             message =
                 getApp()->getCommands()->execCommand(message, channel, false);
             channel->sendMessage(message);
             return "";
         }},
        {"setChannelNotification",
         [this](const std::vector<QString> &arguments) -> QString {
             auto channel = this->getSelectedChannel();
             if (!channel->isTwitchChannel() && !channel->isYouTubeChannel())
             {
                 return "Channel notifications are available for Twitch and "
                        "YouTube channels.";
             }
             const auto platform = channel->isYouTubeChannel()
                                       ? Platform::YouTube
                                       : Platform::Twitch;
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             auto *notifications = getApp()->getNotifications();
             const QString channelName = channel->getName();
             const auto *youtube =
                 dynamic_cast<YouTubeChannel *>(channel.get());
             const auto channelId = youtube ? youtube->channelID() : QString{};
             switch (mode)
             {
                 case 0:
                     notifications->removeChannelNotification(
                         channelName, platform, channelId);
                     break;
                 case 1:
                     notifications->addChannelNotification(channelName,
                                                           platform, channelId);
                     break;
                 default:
                     notifications->updateChannelNotification(
                         channelName, platform, channelId);
             }
             return "";
         }},
        {"popupOverlay",
         [this](const auto &) -> QString {
             this->showOverlayWindow();
             return {};
         }},
        {"toggleOverlayInertia",
         [this](const auto &args) -> QString {
             if (args.empty())
             {
                 return "No arguments provided to toggleOverlayInertia "
                        "(expected one)";
             }
             const auto &arg = args.front();

             if (arg == "this")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 return {};
             }
             if (arg == "thisOrAll")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 else
                 {
                     getApp()->getWindows()->toggleAllOverlayInertia();
                 }
                 return {};
             }
             if (arg == "all")
             {
                 getApp()->getWindows()->toggleAllOverlayInertia();
                 return {};
             }
             return {};
         }},
        {"setHighlightSounds",
         [this](const std::vector<QString> &arguments) -> QString {
             auto channelPtr = this->getSelectedChannel();
             if (!channelPtr->isTwitchChannel())
             {
                 return "Cannot set highlight sounds in a non-Twitch "
                        "channel.";
             }

             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             const QString channel = channelPtr->getName();

             switch (mode)
             {
                 case 0:
                     getSettings()->mute(channel);
                     break;
                 case 1:
                     getSettings()->unmute(channel);
                     break;
                 default:
                     getSettings()->toggleMutedChannel(channel);
             }
             return "";
         }},
        {"openSubscriptionPage",
         [this](const auto &) -> QString {
             if (!this->getSelectedChannel()->isTwitchChannel())
             {
                 return "Cannot subscribe to a non-Twitch "
                        "channel.";
             }

             this->openSubPage();
             return "";
         }},
        {"changeMultichannelContext",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "Expected at least one argument";
             }
             auto *mc =
                 dynamic_cast<MultiChannel *>(this->channel_.get().get());
             if (!mc || mc->channels().empty())
             {
                 return {};
             }

             size_t nextIndex = mc->activeChannelIndex();
             QStringView arg = arguments[0];
             if (arg == u"next")
             {
                 nextIndex =
                     (mc->activeChannelIndex() + 1) % mc->channels().size();
             }
             else if (arg == u"prev")
             {
                 if (mc->activeChannelIndex() == 0)
                 {
                     nextIndex = mc->channels().size() - 1;
                 }
                 else
                 {
                     nextIndex -= 1;
                 }
             }
             else
             {
                 bool ok = false;
                 nextIndex = arg.toULongLong(&ok);
                 if (!ok)
                 {
                     return "Failed to parse argument as integer";
                 }
             }
             mc->setActiveChannelIndex(nextIndex);
             getApp()->getWindows()->forceLayoutChannelViews();
             return {};
         }},
        {"automodReviewApprove",
         [this](const auto &) -> QString {
             return this->view_->approveSelectedAutoMod()
                        ? QString{}
                        : "Select an actionable message in /automod first.";
         }},
        {"automodReviewDeny",
         [this](const auto &) -> QString {
             return this->view_->denySelectedAutoMod()
                        ? QString{}
                        : "Select an actionable message in /automod first.";
         }},
        {"automodReviewRetry",
         [this](const auto &) -> QString {
             return this->view_->retrySelectedAutoMod()
                        ? QString{}
                        : "The selected AutoMod item has no failed action.";
         }},
        {"automodReviewOpenUsercard",
         [this](const auto &) -> QString {
             return this->view_->openSelectedAutoModUsercard()
                        ? QString{}
                        : "Select a message in /automod first.";
         }},
        {"automodReviewCopy",
         [this](const auto &) -> QString {
             return this->view_->copySelectedAutoModDetails()
                        ? QString{}
                        : "Select a message in /automod first.";
         }},
        {"automodReviewTimeout",
         [this](const auto &) -> QString {
             return this->view_->timeoutSelectedAutoMod()
                        ? QString{}
                        : "Select an actionable message in /automod first.";
         }},
        {"automodReviewBan",
         [this](const auto &) -> QString {
             return this->view_->banSelectedAutoMod()
                        ? QString{}
                        : "Select an actionable message in /automod first.";
         }},
        {"automodReviewSelect",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "AutoMod Review selection requires a direction.";
             }
             const auto &argument = arguments.front();
             const bool actionable = argument.endsWith("-actionable");
             const int direction = argument.startsWith("previous") ? -1 : 1;
             return this->view_->selectAutoModReview(direction, actionable)
                        ? QString{}
                        : "No matching AutoMod review item is visible.";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::Split, actions, this);
}

void Split::showEvent(QShowEvent *event)
{
    const auto pauseModifier = getSettings()->pauseChatModifier.getEnum();
    if (pauseModifier != Qt::NoModifier &&
        QGuiApplication::queryKeyboardModifiers() == pauseModifier)
    {
        this->view_->pause(PauseReason::KeyboardModifier);
    }
    else
    {
        this->view_->unpause(PauseReason::KeyboardModifier);
    }

    if (!this->shortcutsActive_)
    {
        this->addShortcuts();
        this->shortcutsActive_ = true;
    }
    this->ensureHeader();
    this->ensureInput();
    BaseWidget::showEvent(event);
    this->scheduleDeferredTwitchRefresh(!shouldUseColdTwitchFeatureDelay());
    this->refreshSelectedYouTube();
}

Split::~Split()
{
    if (auto *app = tryGetApp(); app && app->getChatAutomations())
    {
        app->getChatAutomations()->removeOpenChannels(this);
    }
    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->channelIDChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
}

void Split::scheduleDeferredTwitchRefresh(bool interactive)
{
    if (this->deferredTwitchRefreshTimer_ == nullptr)
    {
        return;
    }

    if (!this->getTwitchFeatureChannel())
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (container != nullptr && container->getSelectedSplit() != nullptr &&
        container->getSelectedSplit() != this)
    {
        return;
    }

    const int delayMs = interactive ? INTERACTIVE_TWITCH_FEATURE_REFRESH_DELAY_MS
                                    : DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS;

    if (this->deferredTwitchRefreshTimer_->isActive())
    {
        const int remaining = this->deferredTwitchRefreshTimer_->remainingTime();
        if (remaining >= 0 && remaining <= delayMs)
        {
            this->deferredTwitchRefreshInteractive_ =
                this->deferredTwitchRefreshInteractive_ || interactive;
            return;
        }
    }

    this->deferredTwitchRefreshRetries_ = 0;
    this->deferredTwitchRefreshInteractive_ = interactive;
    this->deferredTwitchRefreshTimer_->start(delayMs);
}

void Split::runDeferredTwitchRefresh()
{
    const bool interactive = this->deferredTwitchRefreshInteractive_;

    if (!this->isVisible())
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (container != nullptr && container->getSelectedSplit() != nullptr &&
        container->getSelectedSplit() != this)
    {
        return;
    }

    const auto channel = this->getTwitchFeatureChannel();
    if (!channel)
    {
        return;
    }

    if (channel->roomId().isEmpty())
    {
        if (this->deferredTwitchRefreshRetries_ <
            DEFERRED_TWITCH_ROOM_ID_MAX_RETRIES)
        {
            ++this->deferredTwitchRefreshRetries_;
            this->deferredTwitchRefreshTimer_->start(
                interactive ? INTERACTIVE_TWITCH_ROOM_ID_RETRY_MS
                            : DEFERRED_TWITCH_ROOM_ID_RETRY_MS);
        }
        return;
    }

    this->deferredTwitchRefreshRetries_ = 0;
    this->deferredTwitchRefreshInteractive_ = false;
    const bool forcePersonalRefresh =
        this->deferredTwitchForcePersonalRefresh_;
    this->deferredTwitchForcePersonalRefresh_ = false;

    const int pollOffsetMs =
        interactive ? INTERACTIVE_TWITCH_POLL_REFRESH_OFFSET_MS
                    : DEFERRED_TWITCH_POLL_REFRESH_OFFSET_MS;
    const int editorOffsetMs = interactive
                                   ? INTERACTIVE_TWITCH_EDITOR_REFRESH_OFFSET_MS
                                   : DEFERRED_TWITCH_EDITOR_REFRESH_OFFSET_MS;
    const int pointsOffsetMs =
        interactive ? INTERACTIVE_TWITCH_POINTS_REFRESH_OFFSET_MS
                    : DEFERRED_TWITCH_POINTS_REFRESH_OFFSET_MS;
    const int warningOffsetMs =
        interactive ? INTERACTIVE_TWITCH_WARNING_REFRESH_OFFSET_MS
                    : DEFERRED_TWITCH_WARNING_REFRESH_OFFSET_MS;

    const auto weakChannel = std::weak_ptr<TwitchChannel>(channel);
    auto runIfStillActive = [this, weakChannel](auto &&callback) {
        if (!this->isVisible())
        {
            return;
        }

        auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
        if (container != nullptr && container->getSelectedSplit() != nullptr &&
            container->getSelectedSplit() != this)
        {
            return;
        }

        const auto tc = weakChannel.lock();
        if (!tc || tc != this->getTwitchFeatureChannel() ||
            tc->roomId().isEmpty())
        {
            return;
        }

        callback(tc.get());
    };

    channel->showPendingChatWarningIfVisible();
    const bool shouldRefreshWarnings =
        interactive || this->deferredTwitchWarningStartupSeen_;
    this->deferredTwitchWarningStartupSeen_ = true;

    if (getSettings()->enablePinnedMessages)
    {
        runIfStillActive([](TwitchChannel *tc) {
            tc->refreshPinnedMessageIfStale();
        });
    }

    if (getSettings()->enablePredictions)
    {
        runIfStillActive([forcePersonalRefresh](TwitchChannel *tc) {
            tc->refreshPrediction(forcePersonalRefresh);
        });
    }

    if (getSettings()->showEditStreamInfoButtonInSplitHeader)
    {
        QTimer::singleShot(editorOffsetMs, this, [this, runIfStillActive] {
            runIfStillActive([this](TwitchChannel *tc) {
                const auto roomId = tc->roomId();
                if (this->editorAccessProbedChannels_.contains(roomId))
                {
                    return;
                }

                const auto managedChannel = this->getTwitchFeatureChannel();
                if (!managedChannel || managedChannel.get() != tc)
                {
                    return;
                }

                this->editorAccessProbedChannels_.insert(roomId);
                const QPointer<Split> self(this);
                const auto refreshHeader = [self, roomId] {
                    if (!self)
                    {
                        return;
                    }
                    const auto current = self->getTwitchFeatureChannel();
                    if (current && current->roomId() == roomId)
                    {
                        if (self->header_ != nullptr)
                        {
                            self->header_->updateIcons();
                        }
                    }
                };
                ChannelManagement::verifyAccess(
                    managedChannel, false,
                    [refreshHeader](ChannelManagementAccess) {
                        refreshHeader();
                    },
                    [refreshHeader](const QString &) {
                        refreshHeader();
                    });
            });
        });
    }
    if (getSettings()->enablePolls)
    {
        QTimer::singleShot(pollOffsetMs, this,
                           [this, runIfStillActive, forcePersonalRefresh] {
                               runIfStillActive([forcePersonalRefresh](
                                                    TwitchChannel *tc) {
                                   tc->refreshPollIfStale(forcePersonalRefresh);
                               });
                           });
    }
    if (getSettings()->enableChannelPointsDisplay)
    {
        QTimer::singleShot(pointsOffsetMs, this,
                           [this, runIfStillActive, forcePersonalRefresh] {
                               runIfStillActive([forcePersonalRefresh](
                                                    TwitchChannel *tc) {
                                   tc->refreshChannelPointsIfStale(
                                       forcePersonalRefresh);
                               });
                           });
    }
    if (shouldRefreshWarnings)
    {
        QTimer::singleShot(warningOffsetMs, this,
                           [this, runIfStillActive, forcePersonalRefresh] {
                               runIfStillActive([forcePersonalRefresh](
                                                    TwitchChannel *tc) {
                                   tc->refreshChatWarningIfStale(
                                       forcePersonalRefresh);
                               });
                           });
    }
}

ChannelView &Split::getChannelView()
{
    return *this->view_;
}

SplitInput &Split::getInput()
{
    return *this->ensureInput();
}

void Split::updateInputPlaceholder()
{
    if (this->input_ == nullptr)
    {
        return;
    }

    // If the user disabled placeholder text, clear it and bail out
    if (!getSettings()->showInputPlaceholder)
    {
        this->input_->ui_.textEdit->setPlaceholderText({});
        return;
    }

    auto channel = this->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        const auto *active = multiChannel->activeChannel();
        if (!active)
        {
            this->input_->ui_.textEdit->setPlaceholderText({});
        }
        else
        {
            if (auto *tc = dynamic_cast<TwitchChannel *>(active->channel.get());
                tc && tc->isReadingAnonymously())
            {
                this->input_->ui_.textEdit->setPlaceholderText(
                    "Anonymous channel - read only");
            }
            else if (active->channel->isTikTokChannel())
            {
                this->input_->ui_.textEdit->setPlaceholderText(
                    tikTokInputPlaceholder(*active->channel, true));
            }
            else if (active->channel->isYouTubeChannel())
            {
                const auto user = getApp()->getAccounts()->youtube.current();
                const auto identity = user->handle().isEmpty()
                                          ? user->displayName()
                                          : user->handle();
                const auto placeholder =
                    !getApp()->getAccounts()->youtube.isLoggedIn()
                        ? QStringLiteral(
                              "Connect a YouTube account to send messages...")
                        : QString(u"Send in " %
                                  active->channel->getDisplayName() % u" as " %
                                  identity % u"...");
                this->input_->ui_.textEdit->setPlaceholderText(placeholder);
            }
            else
            {
                this->input_->ui_.textEdit->setPlaceholderText(
                    u"Send message in " % active->channel->getName() % u"...");
            }
        }
        return;
    }
    if (this->getChannel()->isKickChannel())
    {
        auto user = getApp()->getAccounts()->kick.current();
        QString placeholderText = [&] {
            if (user->isAnonymous())
            {
                return QString{};
            }
            return QString(u"Send message as " % user->username() % u"...");
        }();
        this->input_->ui_.textEdit->setPlaceholderText(placeholderText);
        return;
    }

    if (this->getChannel()->isYouTubeChannel())
    {
        const auto user = getApp()->getAccounts()->youtube.current();
        const auto identity =
            user->handle().isEmpty() ? user->displayName() : user->handle();
        const auto placeholder =
            !getApp()->getAccounts()->youtube.isLoggedIn()
                ? QStringLiteral(
                      "Connect a YouTube account to send messages...")
                : QString(u"Send message as " % identity % u"...");
        this->input_->ui_.textEdit->setPlaceholderText(placeholder);
        return;
    }

    if (this->getChannel()->isTikTokChannel())
    {
        this->input_->ui_.textEdit->setPlaceholderText(
            tikTokInputPlaceholder(*channel, false));
        return;
    }

    if (!this->getChannel()->isTwitchChannel())
    {
        return;
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->getChannel().get()))
    {
        if (twitchChannel->isReadingAnonymously())
        {
            this->input_->ui_.textEdit->setPlaceholderText(
                "Anonymous channel - read only");
            return;
        }
    }

    auto user = getApp()->getAccounts()->twitch.getCurrent();
    QString placeholderText;

    if (user->isAnon())
    {
        placeholderText = "Log in to send messages...";
    }
    else
    {
        placeholderText = QString("Send message as %1...")
                              .arg(getApp()
                                       ->getAccounts()
                                       ->twitch.getCurrent()
                                       ->getUserName());
    }

    this->input_->ui_.textEdit->setPlaceholderText(placeholderText);
}

void Split::joinChannelInNewTab(const ChannelPtr &channel)
{
    auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
    SplitContainer *container = nb.addPage(true);

    auto *split = new Split(container);
    split->setChannel(channel);
    container->insertSplit(split);
}

void Split::refreshModerationMode()
{
    if (this->header_ != nullptr)
    {
        this->header_->updateIcons();
    }
    this->view_->queueLayout();
}

namespace {

QString pinBannerKey(const std::optional<TwitchChannel::PinnedMessage> &pin)
{
    if (!pin)
    {
        return {};
    }

    QString key = !pin->pinId.isEmpty() ? pin->pinId : pin->messageId;
    if (key.isEmpty())
    {
        key = pin->authorLogin + QStringLiteral(":") + pin->text.left(80);
    }

    if (pin->endsAt && pin->endsAt->isValid())
    {
        key += QStringLiteral("|") +
               pin->endsAt->toUTC().toString(Qt::ISODate);
    }
    return key;
}

QString predictionBannerKey(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    if (!prediction)
    {
        return {};
    }

    return prediction->id + QStringLiteral("|") +
           prediction->status.toUpper() + QStringLiteral("|") +
           prediction->winningOutcomeId;
}

QString pollBannerKey(const std::optional<TwitchChannel::PollEvent> &poll)
{
    if (!poll)
    {
        return {};
    }

    return poll->id + QStringLiteral("|") + poll->status.toUpper();
}

bool predictionIsActive(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    return prediction &&
           prediction->status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
}

bool pollIsActive(const std::optional<TwitchChannel::PollEvent> &poll)
{
    return poll && poll->status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
}

}  // namespace

void Split::clearBannerAttention()
{
    this->bannerAttentionOverride_ = -1;
    this->bannerAttentionUntil_ = {};
}

void Split::noteBannerStateChanged(TwitchChannel *channel, int bannerId)
{
    if (channel == nullptr)
    {
        this->lastPinBannerKey_.clear();
        this->lastPredictionBannerKey_.clear();
        this->lastPollBannerKey_.clear();
        this->clearBannerAttention();
        return;
    }

    const auto pin = *channel->accessPinnedMessage();
    const auto prediction = *channel->accessPrediction();
    const auto poll = *channel->accessPoll();

    const auto newPinKey = pinBannerKey(pin);
    const auto newPredictionKey = predictionBannerKey(prediction);
    const auto newPollKey = pollBannerKey(poll);

    QString *oldKey = nullptr;
    QString newKey;
    switch (bannerId)
    {
        case 0:
            oldKey = &this->lastPinBannerKey_;
            newKey = newPinKey;
            break;
        case 1:
            oldKey = &this->lastPredictionBannerKey_;
            newKey = newPredictionKey;
            break;
        case 2:
            oldKey = &this->lastPollBannerKey_;
            newKey = newPollKey;
            break;
        default:
            return;
    }

    const bool changed = oldKey != nullptr && *oldKey != newKey;

    this->lastPinBannerKey_ = newPinKey;
    this->lastPredictionBannerKey_ = newPredictionKey;
    this->lastPollBannerKey_ = newPollKey;

    if (this->primingBannerState_ || !changed || newKey.isEmpty() ||
        getSettings()->bannerStackMode != 3)
    {
        return;
    }

    const bool hasLivePrediction = predictionIsActive(prediction);
    const bool hasLivePoll = pollIsActive(poll);
    int durationMs = 5000;

    if (bannerId == 0)
    {
        durationMs = (hasLivePrediction || hasLivePoll) ? 5000 : 20000;
    }
    else if (bannerId == 1 && prediction)
    {
        const auto status = prediction->status.toUpper();
        if (status == "ACTIVE")
        {
            durationMs = 12000;
        }
        else if (status == "LOCKED")
        {
            durationMs = 6000;
        }
        else if (status == "RESOLVED")
        {
            durationMs = 10000;
        }
        else
        {
            durationMs = 5000;
        }
    }
    else if (bannerId == 2 && poll)
    {
        const auto status = poll->status.toUpper();
        if (status == "ACTIVE")
        {
            durationMs = 12000;
        }
        else if (status == "COMPLETED")
        {
            durationMs = 8000;
        }
        else
        {
            durationMs = 5000;
        }
    }

    this->bannerToggleOverride_ = -1;
    this->bannerAttentionOverride_ = bannerId;
    this->bannerAttentionUntil_ =
        QDateTime::currentDateTimeUtc().addMSecs(durationMs);

    QTimer::singleShot(durationMs + 50, this, [this, bannerId] {
        if (this->bannerAttentionOverride_ == bannerId &&
            this->bannerAttentionUntil_.isValid() &&
            QDateTime::currentDateTimeUtc() >= this->bannerAttentionUntil_)
        {
            this->clearBannerAttention();
            this->updateBannerVisibility();
        }
    });
}

void Split::updateBannerVisibility()
{
    const bool hasPin = this->pinnedBanner_ != nullptr &&
                        this->pinnedBanner_->hasPinnedMessage();
    const bool hasPred = this->predictionBanner_ != nullptr &&
                         this->predictionBanner_->hasPrediction();
    const int mode = getSettings()->bannerStackMode;
    const bool hasPoll =
        this->pollBanner_ != nullptr && this->pollBanner_->hasPoll();

    const int activeCount = int(hasPin) + int(hasPred) + int(hasPoll);
    auto setVisibility = [this](bool showPin, bool showPred, bool showPoll,
                                bool showToggle) {
        if (this->pinnedBanner_ != nullptr)
        {
            this->pinnedBanner_->setVisible(showPin);
            this->pinnedBanner_->setToggleButtonVisible(showToggle && showPin);
        }
        if (this->predictionBanner_ != nullptr)
        {
            this->predictionBanner_->setVisible(showPred);
            this->predictionBanner_->setToggleButtonVisible(showToggle &&
                                                            showPred);
        }
        if (this->pollBanner_ != nullptr)
        {
            this->pollBanner_->setVisible(showPoll);
            this->pollBanner_->setToggleButtonVisible(showToggle && showPoll);
        }
    };

    if (mode == 0 || activeCount <= 1)
    {
        setVisibility(hasPin, hasPred, hasPoll, false);
        return;
    }

    QVector<int> activeBannerIds;
    if (hasPin)
    {
        activeBannerIds.push_back(0);
    }
    if (hasPred)
    {
        activeBannerIds.push_back(1);
    }
    if (hasPoll)
    {
        activeBannerIds.push_back(2);
    }

    auto firstActiveFromOrder = [&activeBannerIds](
                                    std::initializer_list<int> order) {
        for (const int id : order)
        {
            if (activeBannerIds.contains(id))
            {
                return id;
            }
        }
        return activeBannerIds.isEmpty() ? -1 : activeBannerIds.front();
    };

    int selectedId = -1;
    if (this->bannerAttentionOverride_ >= 0)
    {
        if (this->bannerAttentionUntil_.isValid() &&
            QDateTime::currentDateTimeUtc() < this->bannerAttentionUntil_ &&
            activeBannerIds.contains(this->bannerAttentionOverride_))
        {
            selectedId = this->bannerAttentionOverride_;
        }
        else
        {
            this->clearBannerAttention();
        }
    }

    if (selectedId < 0 && this->bannerToggleOverride_ >= 0 &&
        activeBannerIds.contains(this->bannerToggleOverride_))
    {
        selectedId = this->bannerToggleOverride_;
    }
    else if (selectedId < 0 && mode == 1)
    {
        selectedId = firstActiveFromOrder({0, 1, 2});
    }
    else if (selectedId < 0 && mode == 2)
    {
        selectedId = firstActiveFromOrder({1, 2, 0});
    }
    else if (selectedId < 0 && mode == 4)
    {
        selectedId = firstActiveFromOrder({2, 1, 0});
    }
    else if (selectedId < 0)
    {
        auto urgencyBonus = [](qint64 secondsLeft) {
            if (secondsLeft <= 0)
            {
                return 30;
            }
            if (secondsLeft <= 15)
            {
                return 30;
            }
            if (secondsLeft <= 60)
            {
                return 20;
            }
            if (secondsLeft <= 120)
            {
                return 12;
            }
            if (secondsLeft <= 300)
            {
                return 5;
            }
            return 0;
        };

        auto pollSelfVoteCount = [](const TwitchChannel::PollEvent &poll) {
            int total = 0;
            for (const auto &vote : poll.selfVotes)
            {
                total += vote.freeVotes + vote.channelPointsVotes;
            }
            return total;
        };

        const auto now = QDateTime::currentDateTimeUtc();
        int pinScore = hasPin ? 58 : -1;
        int predictionScore = hasPred ? 0 : -1;
        int pollScore = hasPoll ? 0 : -1;

        if (const auto tc = this->getTwitchFeatureChannel())
        {
            if (hasPin)
            {
                auto pinGuard = tc->accessPinnedMessage();
                if (*pinGuard)
                {
                    const auto &pin = **pinGuard;
                    if (pin.pinnedAt && pin.pinnedAt->isValid() &&
                        pin.pinnedAt->secsTo(now) <= 120)
                    {
                        pinScore += 12;
                    }
                    if (pin.endsAt && pin.endsAt->isValid())
                    {
                        pinScore += urgencyBonus(now.secsTo(*pin.endsAt)) / 2;
                    }
                }
            }

            if (hasPred)
            {
                auto predictionGuard = tc->accessPrediction();
                if (*predictionGuard)
                {
                    const auto &prediction = **predictionGuard;
                    const auto status = prediction.status.toUpper();
                    if (status == "RESOLVED")
                    {
                        predictionScore = 52;
                    }
                    else if (status == "LOCKED")
                    {
                        predictionScore = 36;
                    }
                    else if (status == "CANCELED")
                    {
                        predictionScore = 25;
                    }
                    else if (status == "ACTIVE")
                    {
                        predictionScore = 90;
                        if (prediction.selfOutcomeId.isEmpty())
                        {
                            predictionScore += 15;
                        }
                        if (prediction.createdAt.isValid() &&
                            prediction.predictionWindowSeconds > 0)
                        {
                            const auto end = prediction.createdAt.addSecs(
                                prediction.predictionWindowSeconds);
                            predictionScore += urgencyBonus(now.secsTo(end));
                        }
                    }
                    else
                    {
                        predictionScore = 38;
                    }
                }
            }

            if (hasPoll)
            {
                auto pollGuard = tc->accessPoll();
                if (*pollGuard)
                {
                    const auto &poll = **pollGuard;
                    const auto status = poll.status.toUpper();
                    if (status == "ACTIVE")
                    {
                        pollScore = 88;
                        if (pollSelfVoteCount(poll) == 0)
                        {
                            pollScore += 14;
                        }
                        if (poll.channelPointsVotingEnabled)
                        {
                            pollScore += 4;
                        }

                        std::optional<QDateTime> end = poll.endsAt;
                        if (!end && poll.createdAt.isValid() &&
                            poll.durationSeconds > 0)
                        {
                            end = poll.createdAt.addSecs(poll.durationSeconds);
                        }
                        if (end && end->isValid())
                        {
                            pollScore += urgencyBonus(now.secsTo(*end));
                        }
                    }
                    else if (status == "COMPLETED")
                    {
                        pollScore = 50;
                    }
                    else if (status == "TERMINATED" || status == "ARCHIVED")
                    {
                        pollScore = 25;
                    }
                    else
                    {
                        pollScore = 38;
                    }
                }
            }
        }

        struct Candidate {
            int id = -1;
            int score = -1;
            int tieBreak = 0;
        };

        Candidate best;
        const Candidate candidates[] = {
            {0, pinScore, 0},
            {1, predictionScore, 2},
            {2, pollScore, 1},
        };
        for (const auto &candidate : candidates)
        {
            if (!activeBannerIds.contains(candidate.id))
            {
                continue;
            }
            if (candidate.score > best.score ||
                (candidate.score == best.score &&
                 candidate.tieBreak > best.tieBreak))
            {
                best = candidate;
            }
        }

        selectedId = best.id >= 0 ? best.id : firstActiveFromOrder({1, 2, 0});
    }

    setVisibility(selectedId == 0, selectedId == 1, selectedId == 2, true);
}

void Split::openChannelInBrowserPlayer(ChannelPtr channel)
{
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(
            QUrl(TWITCH_PLAYER_URL.arg(twitchChannel->getName())));
    }
}

void Split::openChannelInStreamlink(const QString channelName)
{
    try
    {
        openStreamlinkForChannel(channelName);
    }
    catch (const Exception &ex)
    {
        qCWarning(chatterinoWidget)
            << "Error in doOpenStreamlink:" << ex.what();
    }
}

void Split::openChannelInCustomPlayer(const QString channelName)
{
    openInCustomPlayer(channelName);
}

IndirectChannel Split::getIndirectChannel()
{
    return this->channel_;
}

ChannelPtr Split::getChannel() const
{
    return this->channel_.get();
}

ChannelPtr Split::getSelectedChannel() const
{
    ChannelPtr chan = this->channel_.get();
    auto *multiChannel = dynamic_cast<MultiChannel *>(chan.get());
    if (multiChannel)
    {
        const auto *active = multiChannel->activeChannel();
        if (active)
        {
            chan = active->channel;
        }
    }
    return chan;
}

void Split::setChannel(IndirectChannel newChannel)
{
    this->channel_ = newChannel;

    const bool isAutoMod =
        newChannel.get()->getType() == Channel::Type::TwitchAutomod;
    if (!isAutoMod && this->autoModReviewBar_ != nullptr)
    {
        this->autoModReviewBar_->setActive(false);
    }
    this->view_->setChannel(newChannel.get());
    if (isAutoMod)
    {
        this->ensureAutoModReviewBar()->setActive(true);
    }
    else if (this->isVisible())
    {
        this->ensureInput();
    }
    if (isAutoMod)
    {
        this->view_->setFocusProxy(nullptr);
        this->view_->setFocusPolicy(Qt::StrongFocus);
        this->setFocusProxy(this->view_);
    }
    else
    {
        if (this->input_ != nullptr)
        {
            this->view_->setFocusProxy(this->input_->ui_.textEdit);
            this->view_->setFocusPolicy(Qt::ClickFocus);
            this->setFocusProxy(this->input_->ui_.textEdit);
        }
        else
        {
            this->view_->setFocusProxy(nullptr);
            this->view_->setFocusPolicy(Qt::StrongFocus);
            this->setFocusProxy(this->view_);
        }
    }
    this->twitchFeatureSignalHolder_.clear();
    this->twitchFeatureChannel_.reset();
    this->primingBannerState_ = true;
    this->clearTwitchFeatureState();

    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
    this->channelSignalHolder_.clear();
    this->sendWaitConnection_ = pajlada::Signals::ScopedConnection{};
    this->setSendWaitStatus({});

    TwitchChannel *tc = dynamic_cast<TwitchChannel *>(newChannel.get().get());
    auto *kc = dynamic_cast<KickChannel *>(newChannel.get().get());
    auto *youtube = dynamic_cast<YouTubeChannel *>(newChannel.get().get());
    auto *mc = dynamic_cast<MultiChannel *>(newChannel.get().get());

    if (mc)
    {
        this->channelSignalHolder_.managedConnect(
            mc->activeChannelChanged, [this] {
                this->updateInputPlaceholder();
                this->updateChannelConnections();
                this->updateTwitchFeatureChannel();
                this->scheduleDeferredTwitchRefresh(true);
                this->refreshSelectedYouTube();
            });

        this->updateChannelConnections();
        this->updateTwitchFeatureChannel();
    }
    else if (tc != nullptr)
    {
        this->usermodeChangedConnection_ = tc->userStateChanged.connect([this] {
            this->updateInputPlaceholder();
            this->refreshModerationMode();
            if (this->header_ != nullptr)
            {
                this->header_->updateChannelText();
                this->header_->updateRoomModes();
            }
        });

        this->roomModeChangedConnection_ = tc->roomModesChanged.connect([this] {
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->updateTwitchFeatureChannel();

        this->channelSignalHolder_.managedConnect(
            tc->sendWaitUpdate, [this](const QString &text) {
                this->setSendWaitStatus(text);
            });
    }
    else if (kc != nullptr)
    {
        this->usermodeChangedConnection_ = kc->userStateChanged.connect([this] {
            this->refreshModerationMode();
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->roomModeChangedConnection_ = kc->roomModesChanged.connect([this] {
            if (this->header_ != nullptr)
            {
                this->header_->updateRoomModes();
            }
        });

        this->channelSignalHolder_.managedConnect(
            kc->sendWaitUpdate, [this](const QString &text) {
                this->setSendWaitStatus(text);
            });
    }
    else if (youtube != nullptr)
    {
        this->usermodeChangedConnection_ =
            youtube->userStateChanged.connect([this] {
                this->refreshModerationMode();
            });
    }

    if (auto *tiktok = dynamic_cast<TikTokChannel *>(newChannel.get().get()))
    {
        this->usermodeChangedConnection_ =
            tiktok->userStateChanged.connect([this] {
                this->updateInputPlaceholder();
            });
    }

    this->primingBannerState_ = false;

    if (this->isVisible())
    {
        this->scheduleDeferredTwitchRefresh(!shouldUseColdTwitchFeatureDelay());
    }

    this->indirectChannelChangedConnection_ =
        newChannel.getChannelChanged().connect([this] {
            QTimer::singleShot(0, this, [this] {
                this->setChannel(this->channel_);
            });
        });

    if (this->header_ != nullptr)
    {
        this->header_->updateIcons();
        this->header_->updateChannelText();
        this->header_->updateRoomModes();
    }

    this->channelSignalHolder_.managedConnect(
        this->channel_.get()->displayNameChanged, [this] {
            if (this->header_ != nullptr)
            {
                this->header_->updateChannelText();
            }
            this->actionRequested.invoke(Action::RefreshTab);
        });

    this->channelChanged.invoke();
    this->actionRequested.invoke(Action::RefreshTab);
    this->refreshSelectedYouTube();

    if (auto *automations = getApp()->getChatAutomations())
    {
        automations->updateOpenChannels(
            this, automationTwitchChannels(this->channel_.get()));
    }

    // Queue up save because: Split channel changed
    getApp()->getWindows()->queueSave();
}

void Split::setModerationMode(bool value)
{
    this->moderationMode_ = value;
    this->refreshModerationMode();
}

bool Split::getModerationMode() const
{
    return this->moderationMode_;
}

std::optional<bool> Split::checkSpellingOverride() const
{
    return this->input_ != nullptr ? this->input_->checkSpellingOverride()
                                   : this->checkSpellingOverride_;
}

void Split::setCheckSpellingOverride(std::optional<bool> override)
{
    this->checkSpellingOverride_ = override;
    if (this->input_ != nullptr)
    {
        this->input_->setCheckSpellingOverride(override);
    }
}

void Split::insertTextToInput(const QString &text)
{
    this->ensureInput()->insertText(text);
}

void Split::showChangeChannelPopup(const char *dialogTitle, bool empty,
                                   std::function<void(bool)> callback)
{
    if (!this->selectChannelDialog_.isNull())
    {
        this->selectChannelDialog_->raise();

        return;
    }

    auto *dialog = new SelectChannelDialog(this);
    if (!empty)
    {
        dialog->setSelectedChannel(this->getIndirectChannel());
        if (this->channel_.getType() == Channel::Type::TwitchAutomod)
        {
            dialog->setAutoModChannelFilter(this->getAutoModChannelFilter());
        }
    }
    else
    {
        dialog->setSelectedChannel({});
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(dialogTitle);
    dialog->show();
    // We can safely ignore this signal connection since the dialog will be closed before
    // this Split is closed
    std::ignore = dialog->closed.connect([=, this] {
        if (dialog->hasSeletedChannel())
        {
            auto selected = dialog->getSelectedChannel();
            this->setChannel(selected);
            if (selected.getType() == Channel::Type::TwitchAutomod)
            {
                this->setAutoModChannelFilter(
                    dialog->getAutoModChannelFilter());
            }
        }

        callback(dialog->hasSeletedChannel());
    });
    this->selectChannelDialog_ = dialog;
}

void Split::updateGifEmotes()
{
    this->view_->queueUpdate();
}

void Split::updateLastReadMessage()
{
    this->view_->updateLastReadMessage();
}

void Split::paintEvent(QPaintEvent *)
{
    // color the background of the chat
    QPainter painter(this);

    painter.fillRect(this->rect(), this->theme->splits.background);
}

void Split::mouseMoveEvent(QMouseEvent *event)
{
    (void)event;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyPressEvent(QKeyEvent *event)
{
    if (this->view_->handleAutoModReviewKey(event))
    {
        event->accept();
        return;
    }

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyReleaseEvent(QKeyEvent *event)
{
    (void)event;

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::resizeEvent(QResizeEvent *event)
{
    // Queue up save because: Split resized
    getApp()->getWindows()->queueSave();

    BaseWidget::resizeEvent(event);

    if (this->overlay_ != nullptr)
    {
        this->overlay_->setGeometry(this->rect());
    }
}

void Split::enterEvent(QEnterEvent * /*event*/)
{
    this->isMouseOver_ = true;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());

    if (modifierStatus ==
        SHOW_SPLIT_OVERLAY_MODIFIERS /*|| modifierStatus == showAddSplitRegions*/)
    {
        this->ensureOverlay()->show();
    }

    this->actionRequested.invoke(Action::ResetMouseStatus);
}

void Split::leaveEvent(QEvent *event)
{
    (void)event;

    this->isMouseOver_ = false;

    if (this->overlay_ != nullptr)
    {
        this->overlay_->hide();
    }

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::handleModifiers(Qt::KeyboardModifiers modifiers)
{
    if (modifierStatus != modifiers)
    {
        modifierStatus = modifiers;
        modifierStatusChanged.invoke(modifiers);
    }
}

void Split::setIsTopRightSplit(bool value)
{
    this->isTopRightSplit_ = value;
    if (this->header_ != nullptr)
    {
        this->header_->setAddButtonVisible(value);
    }
}

/// Slots
void Split::addSibling()
{
    this->actionRequested.invoke(Action::AppendNewSplit);
}

void Split::deleteFromContainer()
{
    this->actionRequested.invoke(Action::Delete);
}

void Split::changeChannel()
{
    this->showChangeChannelPopup(
        "Change channel", false, [this](bool didSelectChannel) {
            if (!didSelectChannel)
            {
                return;
            }

            // After changing channel (i.e. pressing OK in the channel switcher), close all open Chatter Lists
            // We could consider updating the chatter list with the new channel
            for (const auto &w : this->findChildren<ChatterListWidget *>())
            {
                w->close();
            }
        });
}

void Split::explainMoving()
{
    showTutorialVideo(this, ":/examples/moving.gif", "Moving",
                      "Hold <Ctrl+Alt> to move splits.\n\nExample:");
}

void Split::explainSplitting()
{
    showTutorialVideo(this, ":/examples/splitting.gif", "Splitting",
                      "Hold <Ctrl+Alt> to add new splits.\n\nExample:");
}

void Split::popup()
{
    auto *app = getApp();
    Window &window = app->getWindows()->createWindow(WindowType::Popup);

    auto *split = new Split(window.getNotebook().getOrAddSelectedPage());

    split->setChannel(this->getIndirectChannel());
    split->setModerationMode(this->getModerationMode());
    split->setFilters(this->getFilters());

    window.getNotebook().getOrAddSelectedPage()->insertSplit(split);
    window.show();
}

OverlayWindow *Split::overlayWindow()
{
    return this->overlayWindow_.data();
}

void Split::showOverlayWindow()
{
    if (!this->overlayWindow_)
    {
        this->overlayWindow_ =
            new OverlayWindow(this->getIndirectChannel(), this->getFilters());
    }
    this->overlayWindow_->show();
}

void Split::clear()
{
    this->view_->clearMessages();
}

void Split::openInBrowser()
{
    auto channel = this->getSelectedChannel();

    if (auto *tiktok = dynamic_cast<TikTokChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(tiktok->browserUrl());
        return;
    }

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://kick.com/" + kc->slug());
    }
    else if (auto *youtube = dynamic_cast<YouTubeChannel *>(channel.get()))
    {
        const auto url = youtube->browserUrl();
        if (url.isValid())
        {
            QDesktopServices::openUrl(url);
        }
    }
}

void Split::openWhispersInBrowser()
{
    auto userName = getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    QDesktopServices::openUrl("https://www.twitch.tv/popout/moderator/" +
                              userName + "/whispers");
}

void Split::openBrowserPlayer()
{
    this->openChannelInBrowserPlayer(this->getSelectedChannel());
}

void Split::openModViewInBrowser()
{
    auto channel = this->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/moderator/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://dashboard.kick.com/moderator/" +
                                  kc->slug());
    }
}

void Split::openInStreamlink()
{
    auto chan = this->getSelectedChannel();
    auto *kc = dynamic_cast<KickChannel *>(chan.get());
    if (kc)
    {
        openStreamlinkForChannel(kc->slug(), u"kick.com/");
        return;
    }
    this->openChannelInStreamlink(chan->getName());
}

void Split::openWithCustomScheme()
{
    auto *const channel = this->getSelectedChannel().get();
    if (auto *const twitchChannel = dynamic_cast<TwitchChannel *>(channel))
    {
        this->openChannelInCustomPlayer(twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel))
    {
        openInCustomPlayer(kc->slug(), u"https://kick.com/");
    }
}

void Split::openChatterList()
{
    auto channel = this->getChannel();
    if (!channel)
    {
        qCWarning(chatterinoWidget)
            << "Chatter list opened when no channel was defined";
        return;
    }

    if (!ChatterListWidget::supportsChannel(channel.get()))
    {
        qCWarning(chatterinoWidget)
            << "Chatter list opened without a supported channel";
        return;
    }

    for (auto *window : this->findChildren<ChatterListWidget *>())
    {
        if (window->channelName().compare(channel->getName(),
                                          Qt::CaseInsensitive) == 0)
        {
            window->showNormal();
            window->raise();
            window->activateWindow();
            return;
        }
    }

    const auto chatterListWidth = static_cast<int>(this->width() * 0.5);
    auto *header = this->ensureHeader();
    const auto chatterListHeight =
        this->height() - header->height() -
        (this->input_ != nullptr ? this->input_->height() : 0);

    auto *chatterDock = new ChatterListWidget(std::move(channel), this);

    QObject::connect(chatterDock, &ChatterListWidget::userClicked, this,
                     [this](const QString &userLogin, MessagePlatform platform,
                            const QString &channelName, const QString &userId) {
                         if (platform == MessagePlatform::TikTok)
                         {
                             this->view_->showTikTokUserPopup(userId, userLogin,
                                                              channelName);
                             return;
                         }
                         if (platform == MessagePlatform::YouTube)
                         {
                             const auto url = youtubeChannelUrl(userId);
                             if (!url.isEmpty())
                             {
                                 QDesktopServices::openUrl(QUrl(url));
                             }
                             return;
                         }
                         this->view_->showUserInfoPopup(userLogin, platform,
                                                        channelName);
                     });

    chatterDock->resize(chatterListWidth, chatterListHeight);
    widgets::showAndMoveWindowTo(chatterDock,
                                 this->mapToGlobal(QPoint{0, header->height()}),
                                 widgets::BoundsChecking::CursorPosition);
}

void Split::openSubPage()
{
    ChannelPtr channel = this->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(twitchChannel->subscriptionUrl());
    }
}

void Split::setFiltersDialog()
{
    SelectChannelFiltersDialog d(this->getFilters(), this);
    d.setWindowTitle("Select filters");

    if (d.exec() == QDialog::Accepted)
    {
        this->setFilters(d.getSelection());
    }
}

void Split::setFilters(const QList<QUuid> ids)
{
    this->view_->setFilters(ids);
    if (this->header_ != nullptr)
    {
        this->header_->updateChannelText();
    }
}

QList<QUuid> Split::getFilters() const
{
    return this->view_->getFilterIds();
}

void Split::showSearch(bool singleChannel)
{
    auto *popup = new SearchPopup(this, this);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    if (singleChannel)
    {
        popup->addChannel(this->getChannelView());
        popup->show();
        return;
    }

    // Pass every ChannelView for every Split across the main window's tabs to
    // the search popup.
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    bool addedAnyChannel = false;
    notebook.forEachSplit([&](Split *split) {
        if (split == nullptr)
        {
            return;
        }

        if (split->channel_.getType() == Channel::Type::TwitchAutomod)
        {
            return;
        }

        popup->addChannel(split->getChannelView());
        addedAnyChannel = true;
    });

    if (!addedAnyChannel)
    {
        popup->addChannel(this->getChannelView());
    }

    popup->show();
}

void Split::reconnect()
{
    this->getChannel()->reconnect();
}

void Split::dragEnterEvent(QDragEnterEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        event->acceptProposedAction();
    }
    else
    {
        BaseWidget::dragEnterEvent(event);
    }
}

void Split::dropEvent(QDropEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        this->ensureInput()->ui_.textEdit->imagePasted.invoke(
            event->mimeData());
    }
    else
    {
        BaseWidget::dropEvent(event);
    }
}

void Split::drag()
{
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
    {
        qCWarning(chatterinoWidget) << "Attempted to initiate split drag "
                                       "without a container parent";
        return;
    }

    startDraggingSplit();

    auto originalLocation = container->releaseSplit(this);
    auto *drag = new QDrag(this);
    auto *mimeData = new QMimeData;

    mimeData->setData("chatterino/split", "xD");
    drag->setMimeData(mimeData);

    // drag->exec is a blocking action
    auto dragRes = drag->exec(Qt::MoveAction);
    if (dragRes != Qt::MoveAction || drag->target() == nullptr)
    {
        // The split wasn't dropped in a valid spot, return it to its original position
        container->insertSplit(this, {.position = originalLocation});
    }

    stopDraggingSplit();
}

void Split::setInputReply(const MessagePtr &reply,
                          std::weak_ptr<Channel> channel)
{
    this->ensureInput()->setReply(reply, std::move(channel));
}

void Split::unpause()
{
    this->view_->unpause(PauseReason::KeyboardModifier);
    this->view_->unpause(PauseReason::DoubleClick);
    // Mouse intentionally left out, we may still have the mouse over the split
}

}  // namespace chatterino

QDebug operator<<(QDebug dbg, const chatterino::Split &split)
{
    auto channel = split.getChannel();
    if (channel)
    {
        dbg.nospace() << "Split(" << (void *)&split
                      << ", channel:" << channel->getName() << ")";
    }
    else
    {
        dbg.nospace() << "Split(" << (void *)&split << ", no channel)";
    }

    return dbg;
}

QDebug operator<<(QDebug dbg, const chatterino::Split *split)
{
    if (split != nullptr)
    {
        return operator<<(dbg, *split);
    }

    dbg.nospace() << "Split(nullptr)";

    return dbg;
}
